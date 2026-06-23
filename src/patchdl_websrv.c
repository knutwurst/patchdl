#include "patchdl_websrv.h"
#include "patchdl_assets.h"
#include "patchdl_fw.h"
#include "patchdl_scan.h"
#include "patchdl_install.h"
#include "patchdl_net.h"
#include "patchdl_resolve.h"
#include "patchdl_verxml.h"

#include <microhttpd.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct MHD_Daemon *web_daemon;

static patchdl_fw_t     g_fw;
static patchdl_title_t *g_titles;
static size_t           g_title_count;
static pthread_mutex_t  g_mutex = PTHREAD_MUTEX_INITIALIZER;
static char             g_status_json[512];
static char            *g_debug_json; /* built once at startup */

static const char config_json[] =
  "{"
  "\"default_policy\":\"deny\","
  "\"download_dir\":\"/mnt/usb0/patches\","
  "\"install_after_download\":false,"
  "\"delete_pkg_after_install\":false,"
  "\"source_policy\":{"
  "\"official\":{\"allow_check\":true,\"allow_download\":true,\"allow_install\":true},"
  "\"external\":{\"allow_check\":true,\"allow_download\":true,\"allow_install\":true},"
  "\"shadowmount\":{\"allow_check\":true,\"allow_download\":true,\"allow_install\":false},"
  "\"unknown\":{\"allow_check\":true,\"allow_download\":false,\"allow_install\":false}"
  "},"
  "\"cdn_allowlist\":["
  "\"sgst.prod.dl.playstation.net\","
  "\"gst.prod.dl.playstation.net\","
  "\"gs2.ww.prod.dl.playstation.net\""
  "]}";

static const char downloads_json[] = "[]";

/* ---------- JSON builder ------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} jbuf_t;

static void
jbuf_ensure(jbuf_t *j, size_t extra) {
    if (!j->buf || j->len + extra + 1 > j->cap) {
        size_t newcap = (j->cap + extra + 1) * 2;
        if (newcap < 1024) newcap = 1024;
        char *p = realloc(j->buf, newcap);
        if (!p) return;
        j->buf = p;
        j->cap = newcap;
    }
}

static void
jbuf_append(jbuf_t *j, const char *s) {
    size_t slen = strlen(s);
    jbuf_ensure(j, slen);
    if (!j->buf) return;
    memcpy(j->buf + j->len, s, slen + 1);
    j->len += slen;
}

static void
jbuf_appendf(jbuf_t *j, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    jbuf_append(j, tmp);
}

static void
jbuf_append_str(jbuf_t *j, const char *s) {
    jbuf_append(j, "\"");
    for (const char *p = s; *p; p++) {
        if      (*p == '"')  jbuf_append(j, "\\\"");
        else if (*p == '\\') jbuf_append(j, "\\\\");
        else if (*p == '\n') jbuf_append(j, "\\n");
        else if (*p == '\r') jbuf_append(j, "\\r");
        else if (*p == '\t') jbuf_append(j, "\\t");
        else { char c[2] = {*p, '\0'}; jbuf_append(j, c); }
    }
    jbuf_append(j, "\"");
}

static void
jbuf_append_ver_or_null(jbuf_t *j, const char *ver) {
    if (ver[0])
        jbuf_append_str(j, ver);
    else
        jbuf_append(j, "null");
}

/* ---------- HTTP helpers ------------------------------------------------ */

static enum MHD_Result
queue_buffer(struct MHD_Connection *conn, unsigned int status,
             const char *mime, const void *data, size_t size,
             enum MHD_ResponseMemoryMode mm) {
    struct MHD_Response *resp;
    enum MHD_Result      ret;

    resp = MHD_create_response_from_buffer(size, (void *)data, mm);
    if (!resp) return MHD_NO;

    MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN, "*");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    if (mime)
        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);

    ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

static enum MHD_Result
queue_text(struct MHD_Connection *conn, unsigned int status, const char *text) {
    return queue_buffer(conn, status, "text/plain; charset=utf-8",
                        text, strlen(text), MHD_RESPMEM_PERSISTENT);
}

static enum MHD_Result
queue_json(struct MHD_Connection *conn, unsigned int status, const char *json) {
    return queue_buffer(conn, status, "application/json",
                        json, strlen(json), MHD_RESPMEM_PERSISTENT);
}

/* Takes ownership of json — MHD will free() it */
static enum MHD_Result
queue_json_owned(struct MHD_Connection *conn, unsigned int status, char *json) {
    if (!json) return queue_text(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");
    return queue_buffer(conn, status, "application/json",
                        json, strlen(json), MHD_RESPMEM_MUST_FREE);
}

static enum MHD_Result
queue_asset(struct MHD_Connection *conn, const char *url) {
    const patchdl_asset_t *asset = patchdl_asset_find(url);
    if (!asset) return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
    return queue_buffer(conn, MHD_HTTP_OK, asset->mime,
                        asset->data, asset->size, MHD_RESPMEM_PERSISTENT);
}

/* ---------- status JSON ------------------------------------------------- */

static void
rebuild_status_json(void) {
    snprintf(g_status_json, sizeof(g_status_json),
             "{\"firmware\":\"%s\","
             "\"firmware_build\":\"0x%08x\","
             "\"dns_guard\":\"Active\","
             "\"resolver\":\"Internal allowlist\","
             "\"free_space_mb\":0,"
             "\"download_dir\":\"/mnt/usb0/patches\"}",
             g_fw.str, g_fw.bin);
}

/* ---------- titles JSON (built per request under mutex) ----------------- */

static const char *
title_status_str(const patchdl_title_t *t) {
    if (!t->verxml_done)                          return "checking";
    if (t->source_type == PATCHDL_SOURCE_UNKNOWN) return "blocked";
    if (t->compatible_version[0]) {
        /* Versions are zero-padded (01.000.011), so strcmp orders them.
           A compatible patch newer than what's installed = update available. */
        if (t->installed_version[0] &&
            strcmp(t->compatible_version, t->installed_version) <= 0)
            return "up_to_date";
        return "available";
    }
    if (t->latest_version[0]) return "incompatible_fw";
    return "no_update";
}

static const char *
title_mode_str(patchdl_source_t src) {
    switch (src) {
    case PATCHDL_SOURCE_SHADOWMOUNT: return "download_only";
    case PATCHDL_SOURCE_UNKNOWN:     return "disabled";
    default:                          return "latest_compatible";
    }
}

static char *
build_titles_json(void) {
    jbuf_t j = {0};

    pthread_mutex_lock(&g_mutex);

    jbuf_append(&j, "[");
    for (size_t i = 0; i < g_title_count; i++) {
        const patchdl_title_t *t = &g_titles[i];
        if (i > 0) jbuf_append(&j, ",");
        jbuf_append(&j, "{");
        jbuf_append(&j, "\"title_id\":");      jbuf_append_str(&j, t->title_id);
        jbuf_append(&j, ",\"name\":");          jbuf_append_str(&j, t->name);
        jbuf_append(&j, ",\"content_id\":");   jbuf_append_str(&j, t->content_id);
        jbuf_append(&j, ",\"installed_version\":");
            jbuf_append_ver_or_null(&j, t->installed_version);
        jbuf_append(&j, ",\"compatible_version\":");
            jbuf_append_ver_or_null(&j, t->compatible_version);
        jbuf_append(&j, ",\"latest_version\":");
            jbuf_append_ver_or_null(&j, t->latest_version);
        jbuf_append(&j, ",\"latest_required_fw\":");
            jbuf_append_ver_or_null(&j, t->latest_required_fw);
        jbuf_append(&j, ",\"source_type\":");
            jbuf_append_str(&j, patchdl_source_str(t->source_type));
        jbuf_append(&j, ",\"source_path\":");  jbuf_append_str(&j, t->source_path);
        jbuf_append(&j, ",\"mount_from\":");   jbuf_append_str(&j, t->mount_from);
        jbuf_append(&j, ",\"version_file_uri\":");
            jbuf_append_ver_or_null(&j, t->version_file_uri);
        jbuf_append(&j, ",\"patch_title_id\":");
            jbuf_append_ver_or_null(&j, t->patch_title_id);
        /* The patch package's title must match the game; a mismatch means a
           cross-region/title package that must NOT be installed. */
        jbuf_appendf(&j, ",\"patch_title_match\":%s",
                     (!t->patch_title_id[0] ||
                      !strncmp(t->patch_title_id, t->title_id, 9)) ? "true" : "false");
        jbuf_appendf(&j, ",\"enabled\":%s",
                     t->source_type == PATCHDL_SOURCE_UNKNOWN ? "false" : "true");
        jbuf_append(&j, ",\"mode\":");
            jbuf_append_str(&j, title_mode_str(t->source_type));
        jbuf_append(&j, ",\"queued\":false");
        jbuf_append(&j, ",\"status\":");
            jbuf_append_str(&j, title_status_str(t));
        jbuf_append(&j, "}");
    }
    jbuf_append(&j, "]");

    pthread_mutex_unlock(&g_mutex);
    return j.buf; /* caller owns; use queue_json_owned */
}

/* ---------- verxml background fetch ------------------------------------ */

static void *
verxml_fetch_thread(void *arg) {
    (void)arg;

    for (size_t i = 0; i < g_title_count; i++) {
        patchdl_title_t  *t = &g_titles[i];
        char              url[512] = {0};
        patchdl_verinfo_t info;

        memset(&info, 0, sizeof(info));

        /* The version.xml URL comes straight from the app.db (UUID included);
           fall back to the resolver only if the app.db had no entry. */
        if (t->version_file_uri[0])
            patchdl_verxml_query(t->version_file_uri, g_fw.bin, &info);
        else if (!patchdl_resolve_url(t->title_id, url, sizeof(url)))
            patchdl_verxml_query(url, g_fw.bin, &info);

        pthread_mutex_lock(&g_mutex);
        strncpy(t->compatible_version, info.compatible_version,
                sizeof(t->compatible_version) - 1);
        strncpy(t->latest_version,     info.latest_version,
                sizeof(t->latest_version) - 1);
        strncpy(t->latest_required_fw, info.latest_required_fw,
                sizeof(t->latest_required_fw) - 1);
        strncpy(t->patch_url,          info.compatible_url,
                sizeof(t->patch_url) - 1);
        strncpy(t->patch_title_id,     info.compatible_title,
                sizeof(t->patch_title_id) - 1);
        t->verxml_done = 1;
        pthread_mutex_unlock(&g_mutex);
    }
    return NULL;
}

/* ---------- title action helper ---------------------------------------- */

/* Snapshot the fields an action needs, under the lock. Returns 0 if found. */
static int
get_title_action_info(const char *title_id, patchdl_source_t *src,
                      char *patch_url, size_t url_sz,
                      char *patch_title_id, size_t pt_sz) {
    int found = 0;

    pthread_mutex_lock(&g_mutex);
    for (size_t i = 0; i < g_title_count; i++) {
        if (!strcmp(g_titles[i].title_id, title_id)) {
            *src = g_titles[i].source_type;
            strncpy(patch_url, g_titles[i].patch_url, url_sz - 1);
            patch_url[url_sz - 1] = '\0';
            strncpy(patch_title_id, g_titles[i].patch_title_id, pt_sz - 1);
            patch_title_id[pt_sz - 1] = '\0';
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return found;
}

#define PATCHDL_DL_DIR "/data/patchdl"

/* Local on-disk path a title's patch downloads to / installs from. */
static void
title_pkg_path(const char *title_id, const char *patch_url,
               char *out, size_t out_sz) {
    const char *base = strrchr(patch_url, '/');
    base = base ? base + 1 : "patch.pkg";
    snprintf(out, out_sz, "%s/%s/%s", PATCHDL_DL_DIR, title_id, base);
}

static enum MHD_Result
do_download(struct MHD_Connection *conn, const char *title_id,
            patchdl_source_t src, const char *patch_url) {
    char        dir[256], dest[320], resp[640];
    long long   bytes = 0;

    if (src == PATCHDL_SOURCE_UNKNOWN)
        return queue_json(conn, MHD_HTTP_FORBIDDEN,
                          "{\"ok\":false,\"reason\":\"source_unknown\"}");

    if (!patch_url[0])
        return queue_json(conn, MHD_HTTP_CONFLICT,
                          "{\"ok\":false,\"reason\":\"no_compatible_patch\"}");

    /* /data/patchdl/<title_id>/<pkg-basename> — homebrew data dir, not system */
    mkdir(PATCHDL_DL_DIR, 0777);
    snprintf(dir, sizeof(dir), "%s/%s", PATCHDL_DL_DIR, title_id);
    mkdir(dir, 0777);
    title_pkg_path(title_id, patch_url, dest, sizeof(dest));

    if (patchdl_http_download(patch_url, dest, &bytes)) {
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"reason\":\"download_failed\"}");
        return queue_json_owned(conn, MHD_HTTP_BAD_GATEWAY, strdup(resp));
    }

    /* shadowmount: download allowed, install is not (per source policy). */
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"downloaded\":true,\"bytes\":%lld,\"path\":\"%s\","
             "\"install_allowed\":%s}",
             bytes, dest,
             src == PATCHDL_SOURCE_SHADOWMOUNT ? "false" : "true");
    return queue_json_owned(conn, MHD_HTTP_OK, strdup(resp));
}

static enum MHD_Result
do_install(struct MHD_Connection *conn, const char *title_id,
           patchdl_source_t src, const char *patch_url,
           const char *patch_title_id) {
    char dest[320], msg[256], resp[640];
    int  rc;

    /* Source policy: only genuine installs may be patched in place.
       Shadowmounts are download-only; unknown/preinstall are blocked. */
    if (src != PATCHDL_SOURCE_OFFICIAL && src != PATCHDL_SOURCE_EXTERNAL)
        return queue_json(conn, MHD_HTTP_FORBIDDEN,
                          "{\"ok\":false,\"reason\":\"install_not_allowed_for_source\"}");

    if (!patch_url[0])
        return queue_json(conn, MHD_HTTP_CONFLICT,
                          "{\"ok\":false,\"reason\":\"no_compatible_patch\"}");

    /* GUARD (app layer): the patch package's title id must match the game.
       A cross-title/region patch would install as a phantom title. */
    if (patch_title_id[0] && strncmp(patch_title_id, title_id, 9) != 0) {
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"reason\":\"patch_title_mismatch\","
                 "\"patch_title_id\":\"%.15s\",\"title_id\":\"%.15s\"}",
                 patch_title_id, title_id);
        return queue_json_owned(conn, MHD_HTTP_CONFLICT, strdup(resp));
    }

    title_pkg_path(title_id, patch_url, dest, sizeof(dest));

    /* GUARD (authoritative): patchdl_install reads the PKG's real title id and
       refuses if it does not match `title_id`. */
    rc = patchdl_install_local_pkg(dest, title_id, msg, sizeof(msg));
    snprintf(resp, sizeof(resp),
             "{\"ok\":%s,\"rc\":%d,\"message\":\"%s\",\"path\":\"%s\"}",
             rc == 0 ? "true" : "false", rc, msg, dest);
    return queue_json_owned(conn, rc == 0 ? MHD_HTTP_OK : MHD_HTTP_BAD_GATEWAY,
                            strdup(resp));
}

/* ---------- request routing -------------------------------------------- */

static int
parse_title_action(const char *url, char *title_id, size_t tid_sz,
                   char *action, size_t act_sz) {
    const char *prefix = "/api/titles/";
    const char *start, *slash;
    size_t len;

    if (strncmp(url, prefix, strlen(prefix))) return -1;
    start = url + strlen(prefix);
    slash = strchr(start, '/');
    if (!slash || slash == start || !slash[1]) return -1;

    len = (size_t)(slash - start);
    if (len >= tid_sz) return -1;
    memcpy(title_id, start, len);
    title_id[len] = '\0';

    len = strlen(slash + 1);
    if (len >= act_sz) return -1;
    memcpy(action, slash + 1, len + 1);
    return 0;
}

static enum MHD_Result
handle_title_action(struct MHD_Connection *conn, const char *url) {
    char             title_id[32];
    char             action[24];
    char             patch_url[512] = {0};
    char             patch_title_id[16] = {0};
    patchdl_source_t src = PATCHDL_SOURCE_UNKNOWN;

    if (parse_title_action(url, title_id, sizeof(title_id),
                           action, sizeof(action)))
        return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");

    if (!get_title_action_info(title_id, &src, patch_url, sizeof(patch_url),
                               patch_title_id, sizeof(patch_title_id)))
        return queue_text(conn, MHD_HTTP_NOT_FOUND, "unknown title");

    if (!strcmp(action, "download"))
        return do_download(conn, title_id, src, patch_url);

    if (!strcmp(action, "check"))
        return queue_json(conn, MHD_HTTP_ACCEPTED,
                          "{\"ok\":true,\"queued\":true,\"action\":\"check\"}");

    if (!strcmp(action, "install"))
        return do_install(conn, title_id, src, patch_url, patch_title_id);

    return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
}

static enum MHD_Result
on_request(void *cls, struct MHD_Connection *conn, const char *url,
           const char *method, const char *version, const char *upload_data,
           size_t *upload_data_size, void **con_cls) {
    (void)cls; (void)version; (void)upload_data; (void)con_cls;

    if (!strcmp(method, MHD_HTTP_METHOD_OPTIONS))
        return queue_text(conn, MHD_HTTP_NO_CONTENT, "");

    if (!strcmp(method, MHD_HTTP_METHOD_POST)) {
        if (*upload_data_size) { *upload_data_size = 0; return MHD_YES; }
        if (!strcmp(url, "/api/config"))
            return queue_json(conn, MHD_HTTP_OK, config_json);
        if (!strncmp(url, "/api/titles/", 12))
            return handle_title_action(conn, url);
        return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
    }

    if (strcmp(method, MHD_HTTP_METHOD_GET) &&
        strcmp(method, MHD_HTTP_METHOD_HEAD))
        return queue_text(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");

    if (!strcmp(url, "/api/status"))
        return queue_json(conn, MHD_HTTP_OK, g_status_json);

    if (!strcmp(url, "/api/config"))
        return queue_json(conn, MHD_HTTP_OK, config_json);

    if (!strcmp(url, "/api/titles"))
        return queue_json_owned(conn, MHD_HTTP_OK, build_titles_json());

    if (!strcmp(url, "/api/downloads"))
        return queue_json(conn, MHD_HTTP_OK, downloads_json);

    if (!strcmp(url, "/api/debug"))
        return queue_json(conn, MHD_HTTP_OK,
                          g_debug_json ? g_debug_json : "{}");

    if (!strcmp(url, "/api/installcheck")) {
        char m[256], r[320];
        int rc = patchdl_install_backend_check(m, sizeof(m));
        snprintf(r, sizeof(r), "{\"ready\":%s,\"message\":\"%s\"}",
                 rc == 0 ? "true" : "false", m);
        return queue_json_owned(conn, MHD_HTTP_OK, strdup(r));
    }

    if (!strcmp(url, "/api/netcheck")) {
        char diag[1024] = "{\"error\":\"no title with version_file_uri\"}";
        pthread_mutex_lock(&g_mutex);
        for (size_t i = 0; i < g_title_count; i++) {
            if (g_titles[i].version_file_uri[0]) {
                patchdl_net_diag(g_titles[i].version_file_uri, diag, sizeof(diag));
                break;
            }
        }
        pthread_mutex_unlock(&g_mutex);
        return queue_json(conn, MHD_HTTP_OK, diag);
    }

    return queue_asset(conn, url);
}

/* ---------- lifecycle -------------------------------------------------- */

int
patchdl_websrv_start(unsigned short port) {
    pthread_t tid;
    pthread_attr_t attr;

    if (web_daemon) return 0;

    /* Collect real FW version and installed titles synchronously.
       The web UI is reachable only after this completes. */
    patchdl_fw_get(&g_fw);
    rebuild_status_json();

    if (patchdl_scan(&g_titles, &g_title_count))
        g_title_count = 0;

    /* Build the diagnostic dump now, while single-threaded — the root-vnode
       swap it performs is unsafe once MHD worker threads are running. */
    g_debug_json = patchdl_scan_debug_json();

    web_daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION,
        port, NULL, NULL, &on_request, NULL,
        MHD_OPTION_END);

    if (!web_daemon) {
        patchdl_scan_free(g_titles, g_title_count);
        g_titles = NULL;
        g_title_count = 0;
        return -1;
    }

    /* Start background verxml fetch — detached, runs until complete */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, verxml_fetch_thread, NULL);
    pthread_attr_destroy(&attr);

    return 0;
}

void
patchdl_websrv_stop(void) {
    if (web_daemon) {
        MHD_stop_daemon(web_daemon);
        web_daemon = NULL;
    }
    pthread_mutex_lock(&g_mutex);
    patchdl_scan_free(g_titles, g_title_count);
    g_titles      = NULL;
    g_title_count = 0;
    pthread_mutex_unlock(&g_mutex);
}
