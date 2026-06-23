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
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static struct MHD_Daemon *web_daemon;

static patchdl_fw_t     g_fw;
static patchdl_title_t *g_titles;
static size_t           g_title_count;
static pthread_mutex_t  g_mutex = PTHREAD_MUTEX_INITIALIZER;
static char            *g_debug_json; /* built once at startup */
static unsigned long    g_pkg_hits;
static char             g_pkg_diag_json[768] = "{\"hits\":0}";

/* Persisted, user-editable settings. Per-title enable/disable lives in the
   title structs (t->enabled) so it travels with the scan; the global fields
   live here. Both are saved to PATCHDL_CFG_PATH (homebrew data dir, never a
   system file) and reloaded on the next start. Guarded by g_mutex. */
static struct {
    char default_policy[8];        /* "deny" | "allow" */
    int  install_after_download;
    int  delete_pkg_after_install;
    int  verify_downloads;         /* SHA-256 each manifest piece (default off) */
} g_cfg = { "deny", 0, 1, 0 };

static struct {
    int       active;
    int       cancel;      /* set by a cancel request; the worker aborts */
    char      title_id[32];
    char      name[128];
    char      version[16];
    char      path[320];
    long long downloaded;
    long long total;
} g_dl;

#define PATCHDL_DL_DIR    "/data/patchdl"
#define PATCHDL_CFG_PATH  "/data/patchdl/config.json"

/* The source policy and CDN allowlist are fixed (the safety model), so they
   stay constant; only the four mutable fields above are user-controlled. */
static const char config_tail_json[] =
  "\"download_dir\":\"/data/patchdl (internal)\","
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
/* ---------- tiny JSON value lookups (flat objects only) ----------------- */

/* Find `"key"` then the following `true`/`false`; returns dflt if absent. */
static int
json_get_bool(const char *s, const char *key, int dflt) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(s, pat);
    if (!p) return dflt;
    p = strchr(p + strlen(pat), ':');
    if (!p) return dflt;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!strncmp(p, "true", 4))  return 1;
    if (!strncmp(p, "false", 5)) return 0;
    return dflt;
}

/* Find `"key":"value"` and copy value into out. */
static void
json_get_str(const char *s, const char *key, char *out, size_t sz) {
    out[0] = '\0';
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(s, pat);
    if (!p) return;
    p = strchr(p + strlen(pat), ':');
    if (!p) return;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < sz) out[i++] = *p++;
    out[i] = '\0';
}

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

static int
path_segment_safe(const char *s) {
    if (!s || !s[0] || strstr(s, "..")) return 0;
    for (const char *p = s; *p; p++) {
        int ok = (*p >= 'A' && *p <= 'Z') ||
                 (*p >= 'a' && *p <= 'z') ||
                 (*p >= '0' && *p <= '9') ||
                 *p == '_' || *p == '-' || *p == '.';
        if (!ok) return 0;
    }
    return 1;
}

typedef struct {
    int      fd;
    uint64_t start;
    uint64_t size;
} pkg_reader_t;

static ssize_t
pkg_reader_cb(void *cls, uint64_t pos, char *buf, size_t max) {
    pkg_reader_t *r = (pkg_reader_t *)cls;
    uint64_t rem;
    ssize_t n;

    if (!r || pos >= r->size)
        return MHD_CONTENT_READER_END_WITH_ERROR;

    rem = r->size - pos;
    if ((uint64_t)max > rem)
        max = (size_t)rem;

    n = pread(r->fd, buf, max, (off_t)(r->start + pos));
    if (n <= 0)
        return MHD_CONTENT_READER_END_WITH_ERROR;
    return n;
}

static void
pkg_reader_free(void *cls) {
    pkg_reader_t *r = (pkg_reader_t *)cls;
    if (!r) return;
    close(r->fd);
    free(r);
}

static void
record_pkg_diag(const char *url, const char *range, unsigned int status,
                uint64_t start, uint64_t end, uint64_t total) {
    pthread_mutex_lock(&g_mutex);
    g_pkg_hits++;
    snprintf(g_pkg_diag_json, sizeof(g_pkg_diag_json),
             "{\"hits\":%lu,\"url\":\"%.220s\",\"range\":\"%.160s\","
             "\"status\":%u,\"start\":%llu,\"end\":%llu,\"total\":%llu}",
             g_pkg_hits, url ? url : "", range ? range : "", status,
             (unsigned long long)start,
             (unsigned long long)end,
             (unsigned long long)total);
    pthread_mutex_unlock(&g_mutex);
}

/* Serve a downloaded package back to Sony's installer over localhost. This is
   only for files PatchDL already placed under /data/patchdl/<title>/<pkg>. */
static enum MHD_Result
queue_pkg_file(struct MHD_Connection *conn, const char *url) {
    const char *prefix = "/api/pkg/";
    const char *p, *slash;
    char title_id[32], file[160], path[360];
    size_t len;
    struct stat st;
    int fd;
    const char *range;
    uint64_t total, start = 0, end = 0, send_size = 0;
    unsigned int status = MHD_HTTP_OK;
    char content_range[96];
    struct MHD_Response *resp;
    pkg_reader_t *reader;
    enum MHD_Result ret;

    if (strncmp(url, prefix, strlen(prefix)))
        return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
    p = url + strlen(prefix);
    slash = strchr(p, '/');
    if (!slash || slash == p || !slash[1])
        return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");

    len = (size_t)(slash - p);
    if (len >= sizeof(title_id))
        return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
    memcpy(title_id, p, len);
    title_id[len] = '\0';

    len = strlen(slash + 1);
    if (len >= sizeof(file))
        return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
    memcpy(file, slash + 1, len + 1);

    if (!path_segment_safe(title_id) || !path_segment_safe(file))
        return queue_text(conn, MHD_HTTP_FORBIDDEN, "forbidden");

    snprintf(path, sizeof(path), "%s/%s/%s", PATCHDL_DL_DIR, title_id, file);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
    if (fstat(fd, &st) || st.st_size <= 0) {
        close(fd);
        return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
    }

    total = (uint64_t)st.st_size;
    end = total - 1;
    send_size = total;

    range = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                        MHD_HTTP_HEADER_RANGE);
    if (range && !strncmp(range, "bytes=", 6)) {
        const char *spec = range + 6;
        char *dash = strchr(spec, '-');
        if (!dash || strchr(dash + 1, ',')) {
            close(fd);
            return queue_text(conn, MHD_HTTP_RANGE_NOT_SATISFIABLE, "invalid range");
        }

        if (dash == spec) {
            uint64_t suffix = strtoull(dash + 1, NULL, 10);
            if (!suffix) {
                close(fd);
                return queue_text(conn, MHD_HTTP_RANGE_NOT_SATISFIABLE, "invalid range");
            }
            start = suffix >= total ? 0 : total - suffix;
        } else {
            start = strtoull(spec, NULL, 10);
            if (dash[1])
                end = strtoull(dash + 1, NULL, 10);
        }

        if (start >= total || end < start) {
            close(fd);
            return queue_text(conn, MHD_HTTP_RANGE_NOT_SATISFIABLE, "range not satisfiable");
        }
        if (end >= total) end = total - 1;
        send_size = end - start + 1;
        status = 206;
    }
    record_pkg_diag(url, range, status, start, end, total);

    reader = calloc(1, sizeof(*reader));
    if (!reader) {
        close(fd);
        return queue_text(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");
    }
    reader->fd    = fd;
    reader->start = start;
    reader->size  = send_size;

    resp = MHD_create_response_from_callback(send_size, 65536,
                                             pkg_reader_cb, reader,
                                             pkg_reader_free);
    if (!resp) {
        close(fd);
        free(reader);
        return MHD_NO;
    }
    MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN, "*");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "application/octet-stream");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCEPT_RANGES, "bytes");
    if (status == 206) {
        snprintf(content_range, sizeof(content_range), "bytes %llu-%llu/%llu",
                 (unsigned long long)start,
                 (unsigned long long)end,
                 (unsigned long long)total);
        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_RANGE, content_range);
    }
    ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp); /* pkg_reader_free closes fd */
    return ret;
}

/* ---------- status JSON ------------------------------------------------- */

/* Free space on the download partition, in MB. Best-effort: 0 if unavailable. */
static long long
data_free_mb(void) {
    struct statvfs vfs;
    if (statvfs(PATCHDL_DL_DIR, &vfs) == 0 || statvfs("/data", &vfs) == 0)
        return (long long)(((unsigned long long)vfs.f_bavail *
                            (unsigned long long)vfs.f_frsize) / (1024ULL * 1024ULL));
    return 0;
}

/* Built fresh per request so free space stays live (a 60+ GB download moves
   it a lot). g_fw is read-only after startup, so this is thread-safe. */
static char *
build_status_json(void) {
    char *out = malloc(512);
    if (!out) return NULL;
    snprintf(out, 512,
             "{\"firmware\":\"%s\","
             "\"firmware_build\":\"0x%08x\","
             "\"dns_guard\":\"Active\","
             "\"resolver\":\"Internal allowlist\","
             "\"free_space_mb\":%lld,"
             "\"download_dir\":\"/data/patchdl (internal)\"}",
             g_fw.str, g_fw.bin, data_free_mb());
    return out;
}

/* ---------- config persistence (/data/patchdl/config.json) -------------- */

/* Serialize the current global settings; the fixed source policy + allowlist
   are appended from config_tail_json. Caller owns the result (queue_json_owned). */
static char *
build_config_json(void) {
    char head[192];

    pthread_mutex_lock(&g_mutex);
    snprintf(head, sizeof(head),
             "{\"default_policy\":\"%s\","
             "\"install_after_download\":%s,"
             "\"delete_pkg_after_install\":%s,"
             "\"verify_downloads\":%s,",
             g_cfg.default_policy[0] ? g_cfg.default_policy : "deny",
             g_cfg.install_after_download ? "true" : "false",
             g_cfg.delete_pkg_after_install ? "true" : "false",
             g_cfg.verify_downloads ? "true" : "false");
    pthread_mutex_unlock(&g_mutex);

    char *out = malloc(strlen(head) + sizeof(config_tail_json));
    if (!out) return NULL;
    strcpy(out, head);
    strcat(out, config_tail_json);
    return out;
}

/* Write global settings + per-title enabled flags. Must NOT be called while
   holding g_mutex (it takes the lock itself). */
static void
save_config(void) {
    FILE *f;

    mkdir(PATCHDL_DL_DIR, 0777);
    f = fopen(PATCHDL_CFG_PATH, "w");
    if (!f) return;

    pthread_mutex_lock(&g_mutex);
    fprintf(f,
            "{\n\"default_policy\":\"%s\",\n"
            "\"install_after_download\":%s,\n"
            "\"delete_pkg_after_install\":%s,\n"
            "\"verify_downloads\":%s,\n\"titles\":{",
            g_cfg.default_policy[0] ? g_cfg.default_policy : "deny",
            g_cfg.install_after_download ? "true" : "false",
            g_cfg.delete_pkg_after_install ? "true" : "false",
            g_cfg.verify_downloads ? "true" : "false");
    for (size_t i = 0; i < g_title_count; i++)
        fprintf(f, "%s\"%s\":%s", i ? "," : "",
                g_titles[i].title_id, g_titles[i].enabled ? "true" : "false");
    fprintf(f, "}\n}\n");
    pthread_mutex_unlock(&g_mutex);

    fclose(f);
}

/* Load persisted settings over the defaults. Called once at startup, after the
   scan, while still single-threaded. */
static void
load_config(void) {
    FILE  *f;
    char   buf[16384];
    size_t n;
    char   pol[8];

    f = fopen(PATCHDL_CFG_PATH, "r");
    if (!f) return;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    json_get_str(buf, "default_policy", pol, sizeof(pol));
    if (pol[0]) {
        strncpy(g_cfg.default_policy, pol, sizeof(g_cfg.default_policy) - 1);
        g_cfg.default_policy[sizeof(g_cfg.default_policy) - 1] = '\0';
    }
    g_cfg.install_after_download =
        json_get_bool(buf, "install_after_download", g_cfg.install_after_download);
    g_cfg.delete_pkg_after_install =
        json_get_bool(buf, "delete_pkg_after_install", g_cfg.delete_pkg_after_install);
    g_cfg.verify_downloads =
        json_get_bool(buf, "verify_downloads", g_cfg.verify_downloads);

    /* per-title overrides live under "titles": { "<id>": true|false, ... } */
    const char *titles = strstr(buf, "\"titles\"");
    if (titles)
        for (size_t i = 0; i < g_title_count; i++)
            g_titles[i].enabled =
                json_get_bool(titles, g_titles[i].title_id, g_titles[i].enabled);
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
        jbuf_append(&j, ",\"patch_storage_title_id\":");
            jbuf_append_ver_or_null(&j, t->patch_storage_title_id);
        /* This is the target title id parsed from version.xml/manifest_url.
           CDN storage paths may use another regional/master title id; that is
           not exposed here and must not block a valid target match. */
        jbuf_appendf(&j, ",\"patch_title_match\":%s",
                     (!t->patch_title_id[0] ||
                      !strncmp(t->patch_title_id, t->title_id, 9)) ? "true" : "false");
        jbuf_appendf(&j, ",\"enabled\":%s", t->enabled ? "true" : "false");
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

static char *
build_downloads_json(void) {
    jbuf_t j = {0};
    long long downloaded, total;
    int progress = 0;
    char detail[128];

    pthread_mutex_lock(&g_mutex);
    jbuf_append(&j, "[");
    if (g_dl.active) {
        downloaded = g_dl.downloaded;
        total = g_dl.total;
        if (total > 0 && downloaded >= 0)
            progress = (int)((downloaded * 100) / total);
        if (progress < 0) progress = 0;
        if (progress > 100) progress = 100;
        if (total > 0)
            snprintf(detail, sizeof(detail), "%.1f GB of %.1f GB",
                     downloaded / 1073741824.0, total / 1073741824.0);
        else
            snprintf(detail, sizeof(detail), "%.1f MB downloaded",
                     downloaded / 1048576.0);

        jbuf_append(&j, "{");
        jbuf_append(&j, "\"title_id\":"); jbuf_append_str(&j, g_dl.title_id);
        jbuf_append(&j, ",\"name\":");    jbuf_append_str(&j, g_dl.name);
        jbuf_append(&j, ",\"version\":"); jbuf_append_str(&j, g_dl.version);
        jbuf_appendf(&j, ",\"progress\":%d", progress);
        jbuf_append(&j, ",\"detail\":");  jbuf_append_str(&j, detail);
        jbuf_appendf(&j, ",\"bytes\":%lld,\"total_bytes\":%lld",
                     downloaded, total);
        jbuf_append(&j, ",\"path\":");    jbuf_append_str(&j, g_dl.path);
        jbuf_append(&j, "}");
    }
    jbuf_append(&j, "]");
    pthread_mutex_unlock(&g_mutex);
    return j.buf;
}

/* ---------- verxml background fetch ------------------------------------ */

/* Remove a downloaded patch once the game is at/past that version, i.e. the
   install completed. Patches stay internal (/data/patchdl) and are cleaned up
   here at the next scan — not during the async install, which still reads the
   file. */
static void
cleanup_installed_download(const char *title_id, const char *patch_url) {
    char        dir[256], path[320];
    const char *base = strrchr(patch_url, '/');
    base = base ? base + 1 : "patch.pkg";
    snprintf(dir, sizeof(dir), "/data/patchdl/%s", title_id);
    snprintf(path, sizeof(path), "%s/%s", dir, base);
    unlink(path);
    rmdir(dir);
}

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
        strncpy(t->patch_storage_title_id, info.compatible_storage_title,
                sizeof(t->patch_storage_title_id) - 1);
        t->verxml_done = 1;
        int up_to_date = (t->installed_version[0] && info.compatible_version[0] &&
                          strcmp(t->installed_version, info.compatible_version) >= 0);
        pthread_mutex_unlock(&g_mutex);

        if (up_to_date && info.compatible_url[0])
            cleanup_installed_download(t->title_id, info.compatible_url);
    }
    return NULL;
}

/* ---------- title action helper ---------------------------------------- */

/* Snapshot the fields an action needs, under the lock. Returns 0 if found. */
static int
get_title_action_info(const char *title_id, patchdl_source_t *src,
                      char *patch_url, size_t url_sz,
                      char *patch_title_id, size_t pt_sz,
                      char *patch_storage_title_id, size_t pst_sz,
                      char *content_id, size_t ci_sz,
                      char *name, size_t name_sz,
                      char *version, size_t ver_sz,
                      int *enabled) {
    int found = 0;

    pthread_mutex_lock(&g_mutex);
    for (size_t i = 0; i < g_title_count; i++) {
        if (!strcmp(g_titles[i].title_id, title_id)) {
            *src = g_titles[i].source_type;
            strncpy(patch_url, g_titles[i].patch_url, url_sz - 1);
            patch_url[url_sz - 1] = '\0';
            strncpy(patch_title_id, g_titles[i].patch_title_id, pt_sz - 1);
            patch_title_id[pt_sz - 1] = '\0';
            strncpy(patch_storage_title_id, g_titles[i].patch_storage_title_id, pst_sz - 1);
            patch_storage_title_id[pst_sz - 1] = '\0';
            strncpy(content_id, g_titles[i].content_id, ci_sz - 1);
            content_id[ci_sz - 1] = '\0';
            strncpy(name, g_titles[i].name, name_sz - 1);
            name[name_sz - 1] = '\0';
            strncpy(version, g_titles[i].compatible_version, ver_sz - 1);
            version[ver_sz - 1] = '\0';
            *enabled = g_titles[i].enabled;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return found;
}

/* Persist a per-title enable/disable toggle. */
static enum MHD_Result
set_title_enabled(struct MHD_Connection *conn, const char *title_id, int en) {
    int found = 0;
    char r[64];

    pthread_mutex_lock(&g_mutex);
    for (size_t i = 0; i < g_title_count; i++) {
        if (!strcmp(g_titles[i].title_id, title_id)) {
            g_titles[i].enabled = en;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_mutex);

    if (!found) return queue_text(conn, MHD_HTTP_NOT_FOUND, "unknown title");

    save_config();
    snprintf(r, sizeof(r), "{\"ok\":true,\"enabled\":%s}", en ? "true" : "false");
    return queue_json_owned(conn, MHD_HTTP_OK, strdup(r));
}

/* Local on-disk path a title's patch downloads to / installs from. */
static void
title_pkg_path(const char *title_id, const char *patch_url,
               char *out, size_t out_sz) {
    const char *base = strrchr(patch_url, '/');
    char name[192];
    size_t n;

    base = base ? base + 1 : "patch.pkg";
    snprintf(name, sizeof(name), "%s", base);
    n = strlen(name);
    if (n > 5 && !strcmp(name + n - 5, ".json"))
        snprintf(name + n - 5, sizeof(name) - (n - 5), ".pkg");
    snprintf(out, out_sz, "%s/%s/%s", PATCHDL_DL_DIR, title_id, name);
}

static int
url_is_manifest(const char *url) {
    size_t n = url ? strlen(url) : 0;
    return n > 5 && !strcmp(url + n - 5, ".json");
}

/* Returns non-zero to abort the download when a cancel has been requested. */
static int
download_progress_cb(void *ctx, long long downloaded, long long total) {
    int cancel;
    (void)ctx;
    pthread_mutex_lock(&g_mutex);
    if (g_dl.active) {
        g_dl.downloaded = downloaded;
        g_dl.total = total;
    }
    cancel = g_dl.cancel;
    pthread_mutex_unlock(&g_mutex);
    return cancel;
}

/* Delete a title's internal download directory and its contents (a partial or
   a finished package). Best-effort; only ever touches /data/patchdl. */
static void
remove_title_dir(const char *title_id) {
    char           dir[288], path[560];
    DIR           *d;
    struct dirent *e;

    snprintf(dir, sizeof(dir), "%s/%s", PATCHDL_DL_DIR, title_id);
    if ((d = opendir(dir))) {
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            unlink(path);
        }
        closedir(d);
    }
    rmdir(dir);
}

/* Cancel an in-progress download for this title (the worker aborts and removes
   the partial), or delete an already-downloaded package if nothing is running. */
static enum MHD_Result
do_cancel(struct MHD_Connection *conn, const char *title_id) {
    char resp[96];
    int  was_active = 0;

    pthread_mutex_lock(&g_mutex);
    if (g_dl.active && !strcmp(g_dl.title_id, title_id)) {
        g_dl.cancel = 1;
        was_active = 1;
    }
    pthread_mutex_unlock(&g_mutex);

    if (!was_active)
        remove_title_dir(title_id);

    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"cancelled\":%s,\"deleted\":true}",
             was_active ? "true" : "false");
    return queue_json_owned(conn, MHD_HTTP_OK, strdup(resp));
}

static enum MHD_Result
do_download(struct MHD_Connection *conn, const char *title_id,
            patchdl_source_t src, const char *patch_url,
            const char *name, const char *version, int enabled) {
    char        dir[256], dest[320], resp[640];
    long long   bytes = 0;
    int         verify = 0, dlrc;

    if (!enabled)
        return queue_json(conn, MHD_HTTP_FORBIDDEN,
                          "{\"ok\":false,\"reason\":\"title_disabled\"}");

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

    pthread_mutex_lock(&g_mutex);
    if (g_dl.active) {
        pthread_mutex_unlock(&g_mutex);
        return queue_json(conn, MHD_HTTP_CONFLICT,
                          "{\"ok\":false,\"reason\":\"download_in_progress\"}");
    }
    memset(&g_dl, 0, sizeof(g_dl));
    g_dl.active = 1;
    strncpy(g_dl.title_id, title_id, sizeof(g_dl.title_id) - 1);
    strncpy(g_dl.name, name && name[0] ? name : title_id, sizeof(g_dl.name) - 1);
    strncpy(g_dl.version, version ? version : "", sizeof(g_dl.version) - 1);
    strncpy(g_dl.path, dest, sizeof(g_dl.path) - 1);
    verify = g_cfg.verify_downloads;
    pthread_mutex_unlock(&g_mutex);

    dlrc = url_is_manifest(patch_url)
        ? patchdl_http_download_manifest_progress(patch_url, dest, &bytes,
                                                  download_progress_cb, NULL, verify)
        : patchdl_http_download_progress(patch_url, dest, &bytes,
                                         download_progress_cb, NULL);
    if (dlrc) {
        int was_cancel;
        pthread_mutex_lock(&g_mutex);
        was_cancel = g_dl.cancel;
        g_dl.active = 0;
        g_dl.cancel = 0;
        pthread_mutex_unlock(&g_mutex);

        /* The download function already unlinked the partial file on failure;
           drop the now-empty title dir too. */
        unlink(dest);
        rmdir(dir);

        if (was_cancel)
            return queue_json(conn, MHD_HTTP_OK,
                              "{\"ok\":false,\"cancelled\":true,"
                              "\"reason\":\"download_cancelled\"}");
        /* -2 = a piece failed its SHA-256 (only possible when verify is on). */
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"reason\":\"%s\"}",
                 dlrc == -2 ? "piece_verify_failed" : "download_failed");
        return queue_json_owned(conn, MHD_HTTP_BAD_GATEWAY, strdup(resp));
    }

    pthread_mutex_lock(&g_mutex);
    g_dl.downloaded = bytes;
    g_dl.total = bytes;
    g_dl.active = 0;
    pthread_mutex_unlock(&g_mutex);

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
           const char *patch_title_id, const char *patch_storage_title_id,
           const char *content_id, int enabled) {
    char dest[320], msg[256], resp[640];
    int  rc;

    if (!enabled)
        return queue_json(conn, MHD_HTTP_FORBIDDEN,
                          "{\"ok\":false,\"reason\":\"title_disabled\"}");

    /* Source policy: only genuine installs may be patched in place.
       Shadowmounts are download-only; unknown/preinstall are blocked. */
    if (src != PATCHDL_SOURCE_OFFICIAL && src != PATCHDL_SOURCE_EXTERNAL)
        return queue_json(conn, MHD_HTTP_FORBIDDEN,
                          "{\"ok\":false,\"reason\":\"install_not_allowed_for_source\"}");

    if (!patch_url[0])
        return queue_json(conn, MHD_HTTP_CONFLICT,
                          "{\"ok\":false,\"reason\":\"no_compatible_patch\"}");

    /* GUARD (app layer): the version.xml target title id must match the game.
       CDN storage under a different regional/master id is valid and has
       already been normalized by patchdl_verxml. */
    if (patch_title_id[0] && strncmp(patch_title_id, title_id, 9) != 0) {
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"reason\":\"patch_title_mismatch\","
                 "\"patch_title_id\":\"%.15s\",\"title_id\":\"%.15s\"}",
                 patch_title_id, title_id);
        return queue_json_owned(conn, MHD_HTTP_CONFLICT, strdup(resp));
    }

    title_pkg_path(title_id, patch_url, dest, sizeof(dest));

    rc = patchdl_install_local_pkg(dest, title_id, patch_storage_title_id,
                                   content_id, msg, sizeof(msg));
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
    char             patch_storage_title_id[16] = {0};
    char             content_id[64] = {0};
    char             name[128] = {0};
    char             version[16] = {0};
    int              enabled = 0;
    patchdl_source_t src = PATCHDL_SOURCE_UNKNOWN;

    if (parse_title_action(url, title_id, sizeof(title_id),
                           action, sizeof(action)))
        return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");

    if (!get_title_action_info(title_id, &src, patch_url, sizeof(patch_url),
                               patch_title_id, sizeof(patch_title_id),
                               patch_storage_title_id,
                               sizeof(patch_storage_title_id),
                               content_id, sizeof(content_id),
                               name, sizeof(name),
                               version, sizeof(version),
                               &enabled))
        return queue_text(conn, MHD_HTTP_NOT_FOUND, "unknown title");

    if (!strcmp(action, "enable"))
        return set_title_enabled(conn, title_id, 1);

    if (!strcmp(action, "disable"))
        return set_title_enabled(conn, title_id, 0);

    if (!strcmp(action, "download"))
        return do_download(conn, title_id, src, patch_url, name, version, enabled);

    if (!strcmp(action, "cancel"))
        return do_cancel(conn, title_id);

    if (!strcmp(action, "check"))
        return queue_json(conn, MHD_HTTP_ACCEPTED,
                          "{\"ok\":true,\"queued\":true,\"action\":\"check\"}");

    if (!strcmp(action, "install"))
        return do_install(conn, title_id, src, patch_url, patch_title_id,
                          patch_storage_title_id, content_id, enabled);

    return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
}

/* ---------- POST body accumulation ------------------------------------- */

/* MHD delivers a POST body across several callback invocations. We stash a
   growing buffer in con_cls and dispatch once the body is complete. */
typedef struct {
    char  *data;
    size_t len;
} post_body_t;

static void
request_completed(void *cls, struct MHD_Connection *conn, void **con_cls,
                  enum MHD_RequestTerminationCode toe) {
    (void)cls; (void)conn; (void)toe;
    post_body_t *pb = *con_cls;
    if (pb) {
        free(pb->data);
        free(pb);
        *con_cls = NULL;
    }
}

static enum MHD_Result
handle_config_post(struct MHD_Connection *conn, const char *body) {
    char pol[8];

    json_get_str(body, "default_policy", pol, sizeof(pol));

    pthread_mutex_lock(&g_mutex);
    if (pol[0]) {
        strncpy(g_cfg.default_policy, pol, sizeof(g_cfg.default_policy) - 1);
        g_cfg.default_policy[sizeof(g_cfg.default_policy) - 1] = '\0';
    }
    g_cfg.install_after_download =
        json_get_bool(body, "install_after_download", g_cfg.install_after_download);
    g_cfg.delete_pkg_after_install =
        json_get_bool(body, "delete_pkg_after_install", g_cfg.delete_pkg_after_install);
    g_cfg.verify_downloads =
        json_get_bool(body, "verify_downloads", g_cfg.verify_downloads);
    pthread_mutex_unlock(&g_mutex);

    save_config();
    return queue_json_owned(conn, MHD_HTTP_OK, build_config_json());
}

static enum MHD_Result
on_request(void *cls, struct MHD_Connection *conn, const char *url,
           const char *method, const char *version, const char *upload_data,
           size_t *upload_data_size, void **con_cls) {
    (void)cls; (void)version;

    if (!strcmp(method, MHD_HTTP_METHOD_OPTIONS))
        return queue_text(conn, MHD_HTTP_NO_CONTENT, "");

    if (!strcmp(method, MHD_HTTP_METHOD_POST)) {
        post_body_t *pb = *con_cls;

        if (!pb) {                          /* first call: set up the buffer */
            pb = calloc(1, sizeof(*pb));
            if (!pb) return MHD_NO;
            *con_cls = pb;
            return MHD_YES;
        }

        if (*upload_data_size) {            /* a body chunk: append it */
            char *n = realloc(pb->data, pb->len + *upload_data_size + 1);
            if (n) {
                memcpy(n + pb->len, upload_data, *upload_data_size);
                pb->len += *upload_data_size;
                n[pb->len] = '\0';
                pb->data = n;
            }
            *upload_data_size = 0;
            return MHD_YES;
        }

        /* final call: the full body (if any) is in pb->data */
        const char *body = pb->data ? pb->data : "";
        if (!strcmp(url, "/api/config"))
            return handle_config_post(conn, body);
        if (!strncmp(url, "/api/titles/", 12))
            return handle_title_action(conn, url);
        return queue_text(conn, MHD_HTTP_NOT_FOUND, "not found");
    }

    if (strcmp(method, MHD_HTTP_METHOD_GET) &&
        strcmp(method, MHD_HTTP_METHOD_HEAD))
        return queue_text(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed");

    if (!strcmp(url, "/api/status"))
        return queue_json_owned(conn, MHD_HTTP_OK, build_status_json());

    if (!strcmp(url, "/api/config"))
        return queue_json_owned(conn, MHD_HTTP_OK, build_config_json());

    if (!strcmp(url, "/api/titles"))
        return queue_json_owned(conn, MHD_HTTP_OK, build_titles_json());

    if (!strcmp(url, "/api/downloads"))
        return queue_json_owned(conn, MHD_HTTP_OK, build_downloads_json());

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

    if (!strcmp(url, "/api/apiprobe")) {
        char p[2048];
        patchdl_install_api_probe(p, sizeof(p));
        return queue_json_owned(conn, MHD_HTTP_OK, strdup(p));
    }

    if (!strcmp(url, "/api/pkgdiag"))
        return queue_json(conn, MHD_HTTP_OK, g_pkg_diag_json);

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

    if (!strncmp(url, "/api/pkg/", 9))
        return queue_pkg_file(conn, url);

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

    if (patchdl_scan(&g_titles, &g_title_count))
        g_title_count = 0;

    /* Default per-title policy (unknown sources off), then overlay anything the
       user previously saved to /data/patchdl/config.json. */
    for (size_t i = 0; i < g_title_count; i++)
        g_titles[i].enabled = (g_titles[i].source_type != PATCHDL_SOURCE_UNKNOWN);
    load_config();

    /* Build the diagnostic dump now, while single-threaded — the root-vnode
       swap it performs is unsafe once MHD worker threads are running. */
    g_debug_json = patchdl_scan_debug_json();

    web_daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION,
        port, NULL, NULL, &on_request, NULL,
        MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
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
