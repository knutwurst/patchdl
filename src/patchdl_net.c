#include "patchdl_net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef PATCHDL_HAVE_CURL
#include <curl/curl.h>
#include "patchdl_ca.h"
#endif

#define DNS_SERVER     "1.1.1.1"
#define DNS_PORT       53
#define DNS_TIMEOUT_MS 3000

#ifdef PATCHDL_HAVE_CURL

static const char *ALLOWED_HOSTS[] = {
    "sgst.prod.dl.playstation.net",
    "gst.prod.dl.playstation.net",
    "gs2.ww.prod.dl.playstation.net",
    "prosperopatches.com",
    NULL,
};

static int
host_allowed(const char *host) {
    size_t hlen = strlen(host);
    for (int i = 0; ALLOWED_HOSTS[i]; i++) {
        if (!strcmp(host, ALLOWED_HOSTS[i]))
            return 1;
        size_t alen = strlen(ALLOWED_HOSTS[i]);
        if (hlen > alen + 1 &&
            host[hlen - alen - 1] == '.' &&
            !strcmp(host + hlen - alen, ALLOWED_HOSTS[i]))
            return 1;
    }
    return 0;
}

/* Extract hostname from https://host/path or http://host/path */
static int
url_host(const char *url, char *out, size_t out_sz) {
    const char *p = strstr(url, "://");
    const char *end;
    size_t len;

    if (!p) return -1;
    p += 3;
    end = strpbrk(p, "/:?");
    len = end ? (size_t)(end - p) : strlen(p);
    if (len == 0 || len >= out_sz) return -1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* ---------- raw UDP DNS resolver ---------------------------------------- */

/* Diagnostics from the last dns_resolve() call (read by patchdl_net_diag).
   step: 0=ok 1=socket 2=sendto 3=recv<12 4=no-answer. */
static int g_dns_step, g_dns_errno, g_dns_n, g_dns_ancount;

static int
dns_encode_name(const char *host, uint8_t *buf, size_t bufsz) {
    size_t pos = 0;
    const char *p = host;

    while (*p) {
        const char *dot = strchr(p, '.');
        size_t lablen = dot ? (size_t)(dot - p) : strlen(p);
        if (lablen > 63 || pos + 1 + lablen + 1 >= bufsz) return -1;
        buf[pos++] = (uint8_t)lablen;
        memcpy(buf + pos, p, lablen);
        pos += lablen;
        p += lablen;
        if (*p == '.') p++;
    }
    buf[pos++] = 0;
    return (int)pos;
}

static int
dns_resolve(const char *host, char *ip_out, size_t ip_sz) {
    int              fd;
    struct sockaddr_in srv;
    uint8_t          pkt[512], resp[512];
    ssize_t          n;
    int              name_len;
    size_t           pos;
    struct timeval   tv;

    g_dns_step = 0; g_dns_errno = 0; g_dns_n = 0; g_dns_ancount = 0;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { g_dns_step = 1; g_dns_errno = errno; return -1; }

    tv.tv_sec  = DNS_TIMEOUT_MS / 1000;
    tv.tv_usec = (DNS_TIMEOUT_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&srv, 0, sizeof(srv));
    srv.sin_family      = AF_INET;
    srv.sin_port        = htons(DNS_PORT);
    srv.sin_addr.s_addr = inet_addr(DNS_SERVER);

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xDE; pkt[1] = 0xAD; /* ID */
    pkt[2] = 0x01; pkt[3] = 0x00; /* standard query, RD */
    pkt[4] = 0x00; pkt[5] = 0x01; /* QDCOUNT = 1 */
    pos = 12;

    name_len = dns_encode_name(host, pkt + pos, sizeof(pkt) - pos - 4);
    if (name_len < 0) { close(fd); return -1; }
    pos += (size_t)name_len;
    pkt[pos++] = 0x00; pkt[pos++] = 0x01; /* QTYPE = A */
    pkt[pos++] = 0x00; pkt[pos++] = 0x01; /* QCLASS = IN */

    if (sendto(fd, pkt, pos, 0,
               (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        g_dns_step = 2; g_dns_errno = errno; close(fd); return -1;
    }

    n = recv(fd, resp, sizeof(resp), 0);
    g_dns_n = (int)n;
    if (n < 0) g_dns_errno = errno;
    close(fd);
    if (n < 12) { g_dns_step = 3; return -1; }

    uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
    g_dns_ancount = ancount;
    if (!ancount) { g_dns_step = 4; return -1; }

    /* Skip header and question section */
    pos = 12;
    while (pos < (size_t)n) {
        if (!resp[pos]) { pos++; break; }
        if ((resp[pos] & 0xC0) == 0xC0) { pos += 2; break; }
        pos += 1 + resp[pos];
    }
    if (pos + 4 > (size_t)n) { g_dns_step = 5; return -1; }
    pos += 4; /* QTYPE + QCLASS */

    /* Walk every answer RR; the CDN returns a CNAME chain, so skip
       non-A records and return the first A (type 1, rdlen 4). */
    for (int a = 0; a < ancount && pos < (size_t)n; a++) {
        if ((resp[pos] & 0xC0) == 0xC0) {
            pos += 2;
        } else {
            while (pos < (size_t)n && resp[pos])
                pos += 1 + resp[pos];
            pos++;
        }
        if (pos + 10 > (size_t)n) break;

        uint16_t type  = (uint16_t)((resp[pos] << 8) | resp[pos + 1]); pos += 2;
        pos += 6; /* CLASS + TTL */
        uint16_t rdlen = (uint16_t)((resp[pos] << 8) | resp[pos + 1]); pos += 2;

        if (type == 1 && rdlen == 4 && pos + 4 <= (size_t)n) {
            snprintf(ip_out, ip_sz, "%u.%u.%u.%u",
                     resp[pos], resp[pos + 1], resp[pos + 2], resp[pos + 3]);
            return 0;
        }
        pos += rdlen; /* skip this RR's data (e.g. a CNAME target) */
    }

    g_dns_step = 6; /* no A record among the answers */
    return -1;
}

/* Cache resolved IPs by host. The batch fetch hits the same CDN host dozens
   of times; a burst of identical raw-UDP queries gets dropped/rate-limited,
   so resolve once (with retries) and reuse. */
static pthread_mutex_t dns_cache_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct { char host[256]; char ip[INET_ADDRSTRLEN]; } dns_cache[64];
static int dns_cache_n;

static int
dns_lookup(const char *host, char *ip_out, size_t ip_sz) {
    int rc = -1;

    pthread_mutex_lock(&dns_cache_mtx);
    for (int i = 0; i < dns_cache_n; i++) {
        if (!strcmp(dns_cache[i].host, host)) {
            strncpy(ip_out, dns_cache[i].ip, ip_sz - 1);
            ip_out[ip_sz - 1] = '\0';
            pthread_mutex_unlock(&dns_cache_mtx);
            return 0;
        }
    }
    pthread_mutex_unlock(&dns_cache_mtx);

    for (int attempt = 0; attempt < 4 && rc; attempt++)
        rc = dns_resolve(host, ip_out, ip_sz);
    if (rc) return -1;

    pthread_mutex_lock(&dns_cache_mtx);
    if (dns_cache_n < (int)(sizeof(dns_cache) / sizeof(dns_cache[0]))) {
        strncpy(dns_cache[dns_cache_n].host, host,
                sizeof(dns_cache[0].host) - 1);
        strncpy(dns_cache[dns_cache_n].ip, ip_out,
                sizeof(dns_cache[0].ip) - 1);
        dns_cache_n++;
    }
    pthread_mutex_unlock(&dns_cache_mtx);
    return 0;
}

/* ---------- HTTP GET via curl ------------------------------------------- */

static size_t
write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    patchdl_buf_t *b     = userdata;
    size_t         total = size * nmemb;
    char          *newp  = realloc(b->data, b->size + total + 1);
    if (!newp) return 0;
    b->data = newp;
    memcpy(b->data + b->size, ptr, total);
    b->size += total;
    b->data[b->size] = '\0';
    return total;
}

int
patchdl_http_get(const char *url, patchdl_buf_t *out) {
    CURL             *curl;
    CURLcode          res;
    char              host[256], ip[INET_ADDRSTRLEN];
    char              resolve_str[512];
    struct curl_slist *resolve_list = NULL;
    struct curl_blob  ca_blob;

    if (url_host(url, host, sizeof(host))) return -1;
    if (!host_allowed(host)) return -1;
    if (dns_lookup(host, ip, sizeof(ip))) return -1;

    snprintf(resolve_str, sizeof(resolve_str), "%s:443:%s", host, ip);
    resolve_list = curl_slist_append(NULL, resolve_str);

    /* Also cover port 80 in case a redirect lands there */
    char resolve_80[512];
    snprintf(resolve_80, sizeof(resolve_80), "%s:80:%s", host, ip);
    resolve_list = curl_slist_append(resolve_list, resolve_80);

    memset(out, 0, sizeof(*out));

    curl = curl_easy_init();
    if (!curl) { curl_slist_free_all(resolve_list); return -1; }

    /* Trust only the embedded SCEI DNAS Root (Sony's private CDN CA).
       CAINFO_BLOB keeps the cert in memory — no file written anywhere. */
    ca_blob.data  = (void *)PATCHDL_SCEI_DNAS_ROOT_PEM;
    ca_blob.len   = strlen(PATCHDL_SCEI_DNAS_ROOT_PEM);
    ca_blob.flags = CURL_BLOB_COPY;

    curl_easy_setopt(curl, CURLOPT_URL,             url);
    curl_easy_setopt(curl, CURLOPT_RESOLVE,         resolve_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       out);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB,     &ca_blob);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,  1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,  2L);
    /* Some Sony CDN edges still serve SHA-1-signed leaf certs, which
       OpenSSL 3.x rejects at the default security level. Lower it so the
       chain still verifies against the pinned SCEI root. */
    curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST, "DEFAULT@SECLEVEL=0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,         15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,       3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,       "patchdl/1.0");

    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(resolve_list);

    if (res != CURLE_OK) {
        free(out->data);
        out->data = NULL;
        out->size = 0;
        return -1;
    }
    return 0;
}

static size_t
file_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    return fwrite(ptr, size, nmemb, (FILE *)userdata);
}

int
patchdl_http_download(const char *url, const char *dest_path,
                      long long *bytes_out) {
    CURL             *curl;
    CURLcode          res;
    char              host[256], ip[INET_ADDRSTRLEN], rs443[512], rs80[512];
    struct curl_slist *rl = NULL;
    struct curl_blob  ca_blob;
    FILE             *fp;
    curl_off_t        dl = 0;

    if (bytes_out) *bytes_out = 0;
    if (url_host(url, host, sizeof(host))) return -1;
    if (!host_allowed(host)) return -1;
    if (dns_lookup(host, ip, sizeof(ip))) return -1;

    fp = fopen(dest_path, "wb");
    if (!fp) return -1;

    snprintf(rs443, sizeof(rs443), "%s:443:%s", host, ip);
    rl = curl_slist_append(NULL, rs443);
    snprintf(rs80, sizeof(rs80), "%s:80:%s", host, ip);
    rl = curl_slist_append(rl, rs80);

    ca_blob.data  = (void *)PATCHDL_SCEI_DNAS_ROOT_PEM;
    ca_blob.len   = strlen(PATCHDL_SCEI_DNAS_ROOT_PEM);
    ca_blob.flags = CURL_BLOB_COPY;

    curl = curl_easy_init();
    if (!curl) { fclose(fp); curl_slist_free_all(rl); return -1; }

    curl_easy_setopt(curl, CURLOPT_URL,             url);
    curl_easy_setopt(curl, CURLOPT_RESOLVE,         rl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   file_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       fp);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB,     &ca_blob);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,  1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,  2L);
    curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST, "DEFAULT@SECLEVEL=0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,       5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,  20L);
    /* No total timeout (patches can be large); abort only on a long stall. */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,       "patchdl/1.0");

    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &dl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(rl);
    fclose(fp);

    if (res != CURLE_OK) {
        unlink(dest_path);
        return -1;
    }
    if (bytes_out) *bytes_out = (long long)dl;
    return 0;
}

void
patchdl_net_diag(const char *url, char *out_json, size_t sz) {
    char     host[256] = {0}, ip[INET_ADDRSTRLEN] = {0};
    int      hostrc, allowrc, dnsrc;
    long     http_code = 0;
    CURLcode res = CURLE_OK;
    int      did_curl = 0;

    hostrc  = url_host(url, host, sizeof(host));
    allowrc = (hostrc == 0) ? host_allowed(host) : 0;
    dnsrc   = (hostrc == 0) ? dns_resolve(host, ip, sizeof(ip)) : -1;

    if (hostrc == 0 && allowrc && dnsrc == 0) {
        patchdl_buf_t buf;
        memset(&buf, 0, sizeof(buf));
        CURL *curl = curl_easy_init();
        if (curl) {
            char rs[512];
            struct curl_slist *rl;
            struct curl_blob ca_blob;
            snprintf(rs, sizeof(rs), "%s:443:%s", host, ip);
            rl = curl_slist_append(NULL, rs);
            ca_blob.data = (void *)PATCHDL_SCEI_DNAS_ROOT_PEM;
            ca_blob.len = strlen(PATCHDL_SCEI_DNAS_ROOT_PEM);
            ca_blob.flags = CURL_BLOB_COPY;
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_RESOLVE, rl);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
            curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_blob);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST, "DEFAULT@SECLEVEL=0");
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
            res = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_cleanup(curl);
            curl_slist_free_all(rl);
            free(buf.data);
            did_curl = 1;
        }
    }

    snprintf(out_json, sz,
             "{\"host\":\"%s\",\"host_ok\":%d,\"allowed\":%d,"
             "\"dns_rc\":%d,\"ip\":\"%s\","
             "\"dns_step\":%d,\"dns_errno\":%d,\"dns_recv\":%d,\"dns_ancount\":%d,"
             "\"curl_ran\":%d,\"curl_rc\":%d,\"curl_err\":\"%s\",\"http_code\":%ld}",
             host, hostrc == 0, allowrc, dnsrc, ip,
             g_dns_step, g_dns_errno, g_dns_n, g_dns_ancount,
             did_curl, (int)res,
             did_curl ? curl_easy_strerror(res) : "n/a", http_code);
}

#else /* !PATCHDL_HAVE_CURL */

int
patchdl_http_get(const char *url, patchdl_buf_t *out) {
    (void)url;
    (void)out;
    return -1; /* curl not available in this build; set CURL_DIR= to enable */
}

int
patchdl_http_download(const char *url, const char *dest_path,
                      long long *bytes_out) {
    (void)url; (void)dest_path;
    if (bytes_out) *bytes_out = 0;
    return -1;
}

void
patchdl_net_diag(const char *url, char *out_json, size_t sz) {
    (void)url;
    snprintf(out_json, sz, "{\"error\":\"curl not built\"}");
}

#endif /* PATCHDL_HAVE_CURL */

patchdl_buf_t *
patchdl_buf_new(void) {
    return calloc(1, sizeof(patchdl_buf_t));
}

void
patchdl_buf_free(patchdl_buf_t *b) {
    if (!b) return;
    free(b->data);
    free(b);
}
