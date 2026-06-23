#include "patchdl_scan.h"
#include "patchdl_appdb.h"

#include <ps5/kernel.h>

#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_TITLES 512

static const char *SCAN_DIRS[] = {
    "/user/app",
    "/mnt/ext0/user/app",
    "/mnt/ext1/user/app",
    "/system_ex/app",
    NULL,
};

/* ---------- param.json -------------------------------------------------- */

static int
json_str(const char *json, const char *key, char *out, size_t out_sz) {
    char needle[80];
    const char *p;
    size_t len;

    if ((size_t)snprintf(needle, sizeof(needle), "\"%s\"", key) >= sizeof(needle))
        return -1;

    p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r')
        p++;
    if (*p != '"') return -1;
    p++;
    for (len = 0; p[len] && p[len] != '"' && len < out_sz - 1; len++)
        ;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* Extract the title name for the default language out of the nested
   localizedParameters block. PS5 param.json has no flat titleName key. */
static void
json_title_name(const char *json, char *out, size_t out_sz) {
    char        lang[16] = {0};
    const char *p;

    if (!json_str(json, "defaultLanguage", lang, sizeof(lang)) && lang[0]) {
        char anchor[24];
        if ((size_t)snprintf(anchor, sizeof(anchor), "\"%s\"", lang) <
            sizeof(anchor)) {
            p = strstr(json, anchor);
            if (p) {
                const char *tn = strstr(p, "\"titleName\"");
                if (tn && !json_str(tn, "titleName", out, out_sz))
                    return;
            }
        }
    }

    /* Fallback: first titleName anywhere in the file */
    json_str(json, "titleName", out, out_sz);
}

static int
try_param_json(const char *dir, patchdl_title_t *t) {
    char  path[PATH_MAX];
    FILE *fp;
    char  buf[8192];
    size_t n;

    snprintf(path, sizeof(path), "%s/sce_sys/param.json", dir);
    fp = fopen(path, "r");
    if (!fp) return -1;
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (!n) return -1;
    buf[n] = '\0';

    /* PS5 keys first, then PS4 fallbacks */
    if (json_str(buf, "titleId",   t->title_id, sizeof(t->title_id)) &&
        json_str(buf, "TITLE_ID",  t->title_id, sizeof(t->title_id)))
        return -1;

    json_str(buf, "contentId",      t->content_id,        sizeof(t->content_id));
    json_str(buf, "contentVersion", t->installed_version, sizeof(t->installed_version));
    if (!t->content_id[0])
        json_str(buf, "CONTENT_ID", t->content_id, sizeof(t->content_id));
    if (!t->installed_version[0])
        json_str(buf, "APP_VER",    t->installed_version, sizeof(t->installed_version));

    json_title_name(buf, t->name, sizeof(t->name));

    return 0;
}

/* ---------- param.sfo --------------------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;             /* 0x46535000 = "\x00PSF" */
    uint32_t version;
    uint32_t key_table_start;
    uint32_t data_table_start;
    uint32_t num_entries;
} sfo_hdr_t;

typedef struct {
    uint16_t key_offset;
    uint16_t data_fmt;
    uint32_t data_len;
    uint32_t data_max_len;
    uint32_t data_offset;
} sfo_entry_t;
#pragma pack(pop)

#define SFO_MAGIC    0x46535000u
#define SFO_FMT_STR  0x0204u

static int
sfo_get(const uint8_t *buf, size_t bufsz, const char *key,
        char *out, size_t out_sz) {
    const sfo_hdr_t   *h;
    const sfo_entry_t *entries;
    uint32_t i;

    if (bufsz < sizeof(*h)) return -1;
    h = (const sfo_hdr_t *)buf;
    if (h->magic != SFO_MAGIC || h->num_entries > 256) return -1;

    entries = (const sfo_entry_t *)(buf + sizeof(*h));

    for (i = 0; i < h->num_entries; i++) {
        size_t key_off = h->key_table_start  + entries[i].key_offset;
        size_t val_off = h->data_table_start + entries[i].data_offset;

        if (key_off >= bufsz || val_off >= bufsz) continue;

        const char *k = (const char *)(buf + key_off);
        if (strcmp(k, key)) continue;
        if (entries[i].data_fmt != SFO_FMT_STR) return -1;

        size_t len = entries[i].data_len;
        if (val_off + len > bufsz) return -1;
        if (len >= out_sz) len = out_sz - 1;
        memcpy(out, buf + val_off, len);
        out[len] = '\0';
        return 0;
    }
    return -1;
}

static int
try_param_sfo(const char *dir, patchdl_title_t *t) {
    char     path[PATH_MAX];
    FILE    *fp;
    uint8_t  buf[8192];
    size_t   n;
    struct stat st;

    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", dir);
    fp = fopen(path, "rb");
    if (!fp) return -1;

    if (fstat(fileno(fp), &st) || (size_t)st.st_size > sizeof(buf)) {
        fclose(fp);
        return -1;
    }
    n = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (n != (size_t)st.st_size) return -1;

    if (sfo_get(buf, n, "TITLE_ID", t->title_id, sizeof(t->title_id)))
        return -1;

    sfo_get(buf, n, "CONTENT_ID", t->content_id,        sizeof(t->content_id));
    sfo_get(buf, n, "APP_VER",    t->installed_version,  sizeof(t->installed_version));
    sfo_get(buf, n, "TITLE",      t->name,               sizeof(t->name));
    if (!t->name[0])
        sfo_get(buf, n, "TITLE_00", t->name, sizeof(t->name));
    return 0;
}

/* ---------- install-type classification (file-based) -------------------- */

static int
path_exists(const char *dir, const char *rel) {
    char p[PATH_MAX];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    return stat(p, &st) == 0;
}

/* Read up to sz-1 bytes of dir/rel into buf (NUL-terminated). 0 on success. */
static int
read_small_file(const char *dir, const char *rel, char *buf, size_t sz) {
    char   p[PATH_MAX];
    FILE  *fp;
    size_t n;

    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    fp = fopen(p, "rb");
    if (!fp) return -1;
    n = fread(buf, 1, sz - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return 0;
}

/* Keep only actual games. Skip PS5 system apps (NPXS), the Payload Manager
   fake app (PLDM) and ELF-loader homebrew (PSPS). */
static int
is_game_title(const char *title_id) {
    if (!strncmp(title_id, "NPXS", 4)) return 0;
    if (!strncmp(title_id, "PLDM", 4)) return 0;
    if (!strncmp(title_id, "PSPS", 4)) return 0;
    return 1;
}

/* ---------- directory scanner ------------------------------------------- */

/*
 * Distinguish genuine installs from ShadowMountPlus mounts by on-disk layout
 * under /user/app/<TID>/ (verified on fw 11.60):
 *   - mount.lnk / mount_img.lnk + full sce_sys/  -> ShadowMountPlus mount
 *   - app.pkg (no mount.lnk)                     -> genuine install; app.json
 *       with CDN piece URLs means a not-downloaded preinstall stub, local
 *       URLs mean a real install
 *   - app.json with "fake":true                  -> homebrew fake (skip)
 */
static int
scan_one(const char *base, const char *name, patchdl_title_t *t) {
    char dir[PATH_MAX];
    char appjson[4096];
    int  has_mountlnk, has_app_pkg, has_paramjson, is_fake = 0, is_cdn = 0;

    snprintf(dir, sizeof(dir), "%s/%s", base, name);
    memset(t, 0, sizeof(*t));
    strncpy(t->source_path, dir, sizeof(t->source_path) - 1);
    strncpy(t->title_id, name, sizeof(t->title_id) - 1);

    if (!is_game_title(t->title_id))
        return -1;

    has_mountlnk  = path_exists(dir, "mount.lnk") ||
                    path_exists(dir, "mount_img.lnk");
    has_app_pkg   = path_exists(dir, "app.pkg");
    has_paramjson = path_exists(dir, "sce_sys/param.json");

    if (!read_small_file(dir, "app.json", appjson, sizeof(appjson))) {
        if (strstr(appjson, "\"fake\"") && strstr(appjson, "true"))
            is_fake = 1;
        if (strstr(appjson, "http"))
            is_cdn = 1; /* piece URLs point at the Sony CDN -> preinstall */
    }
    if (is_fake)
        return -1;

    if (has_mountlnk) {
        /* metadata lives in the mounted sce_sys (param.json, or param.sfo
           for PS4 titles) */
        if (try_param_json(dir, t))
            try_param_sfo(dir, t);
        t->source_type = PATCHDL_SOURCE_SHADOWMOUNT;
    } else if (has_app_pkg) {
        t->source_type = is_cdn ? PATCHDL_SOURCE_UNKNOWN
                                : PATCHDL_SOURCE_OFFICIAL;
    } else if (has_paramjson || path_exists(dir, "sce_sys/param.sfo")) {
        if (try_param_json(dir, t)) /* genuine game currently mounted */
            try_param_sfo(dir, t);
        t->source_type = PATCHDL_SOURCE_OFFICIAL;
    } else {
        return -1; /* empty / leftover directory */
    }

    if (!t->title_id[0])
        strncpy(t->title_id, name, sizeof(t->title_id) - 1);
    if (!t->name[0])
        strncpy(t->name, t->title_id, sizeof(t->name) - 1);

    return 0;
}

static int
already_seen(const patchdl_title_t *arr, size_t cnt, const char *title_id) {
    for (size_t i = 0; i < cnt; i++)
        if (!strcmp(arr[i].title_id, title_id))
            return 1;
    return 0;
}

static void
scan_base(const char *base, patchdl_title_t *arr, size_t *cnt, size_t cap) {
    DIR           *d;
    struct dirent *de;

    d = opendir(base);
    if (!d) return;

    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (*cnt >= cap) break;
        if (scan_one(base, de->d_name, &arr[*cnt]) != 0)
            continue;
        if (already_seen(arr, *cnt, arr[*cnt].title_id))
            continue; /* dedupe a title already found in an earlier base */
        (*cnt)++;
    }
    closedir(d);
}

/* ---------- public API -------------------------------------------------- */

const char *
patchdl_source_str(patchdl_source_t src) {
    switch (src) {
    case PATCHDL_SOURCE_OFFICIAL:    return "official";
    case PATCHDL_SOURCE_EXTERNAL:    return "external";
    case PATCHDL_SOURCE_SHADOWMOUNT: return "shadowmount";
    default:                          return "unknown";
    }
}

/* Overlay app.db metadata (real title name, installed version, version.xml
   URL) onto the scanned titles. Genuine installs have no readable param.json,
   so this is the only source of their name/version. */
static void
merge_appdb(patchdl_title_t *arr, size_t cnt) {
    patchdl_appinfo_t *info = NULL;
    size_t n = 0;

    if (patchdl_appdb_load(&info, &n))
        return;

    for (size_t i = 0; i < cnt; i++) {
        const patchdl_appinfo_t *a = patchdl_appdb_find(info, n, arr[i].title_id);
        if (!a) continue;
        if (a->title_name[0])
            strncpy(arr[i].name, a->title_name, sizeof(arr[i].name) - 1);
        if (a->content_version[0])
            strncpy(arr[i].installed_version, a->content_version,
                    sizeof(arr[i].installed_version) - 1);
        if (a->content_id[0] && !arr[i].content_id[0])
            strncpy(arr[i].content_id, a->content_id,
                    sizeof(arr[i].content_id) - 1);
        if (a->version_file_uri[0])
            strncpy(arr[i].version_file_uri, a->version_file_uri,
                    sizeof(arr[i].version_file_uri) - 1);
    }

    patchdl_appdb_free(info);
}

int
patchdl_scan(patchdl_title_t **titles_out, size_t *count_out) {
    patchdl_title_t *arr;
    size_t cnt = 0;
    pid_t pid = getpid();
    intptr_t saved_root = 0, root_vnode;
    int using_vswap = 0;

    arr = calloc(MAX_TITLES, sizeof(*arr));
    if (!arr) return -1;

    /* Elevate so privileged paths (the app.db under /system_data) are
       readable; same authid ftpsrv uses. No-op without kernel R/W. */
    kernel_set_ucred_authid(pid, 0x4801000000000013L);

    root_vnode = kernel_get_root_vnode();
    if (root_vnode) {
        saved_root = kernel_get_proc_rootdir(pid);
        kernel_set_proc_rootdir(pid, root_vnode);
        using_vswap = 1;
    }

    for (int i = 0; SCAN_DIRS[i]; i++)
        scan_base(SCAN_DIRS[i], arr, &cnt, MAX_TITLES);

    merge_appdb(arr, cnt);

    if (using_vswap)
        kernel_set_proc_rootdir(pid, saved_root);

    *titles_out = arr;
    *count_out  = cnt;
    return 0;
}

void
patchdl_scan_free(patchdl_title_t *titles, size_t count) {
    (void)count;
    free(titles);
}

/* ---------- diagnostic dump --------------------------------------------- */

static char *
dbg_append(char *buf, size_t *len, size_t *cap, const char *s) {
    size_t sl = strlen(s);
    if (*len + sl + 1 > *cap) {
        size_t nc = (*cap + sl + 1) * 2;
        char *p = realloc(buf, nc);
        if (!p) return buf;
        buf = p;
        *cap = nc;
    }
    memcpy(buf + *len, s, sl + 1);
    *len += sl;
    return buf;
}

char *
patchdl_scan_debug_json(void) {
    char  *buf = NULL;
    size_t len = 0, cap = 0;
    char   tmp[2048];
    pid_t  pid = getpid();
    intptr_t saved_root = 0, root_vnode;
    int    using_vswap = 0;
    struct statfs *mounts = NULL;
    int    nmounts;

    /* getmntinfo returns the global mount table — no root-vnode swap needed.
       Must NOT free the returned buffer (libc-managed). */
    nmounts = getmntinfo(&mounts, MNT_NOWAIT);
    if (nmounts < 0) { nmounts = 0; mounts = NULL; }

    buf = dbg_append(buf, &len, &cap, "{\"mounts\":[");
    for (int i = 0; i < nmounts; i++) {
        snprintf(tmp, sizeof(tmp),
                 "%s{\"from\":\"%s\",\"on\":\"%s\",\"type\":\"%s\",\"flags\":\"0x%lx\"}",
                 i ? "," : "",
                 mounts[i].f_mntfromname, mounts[i].f_mntonname,
                 mounts[i].f_fstypename, (unsigned long)mounts[i].f_flags);
        buf = dbg_append(buf, &len, &cap, tmp);
    }

    buf = dbg_append(buf, &len, &cap, "],\"bases\":[");

    root_vnode = kernel_get_root_vnode();
    if (root_vnode) {
        saved_root = kernel_get_proc_rootdir(pid);
        kernel_set_proc_rootdir(pid, root_vnode);
        using_vswap = 1;
    }

    for (int i = 0; SCAN_DIRS[i]; i++) {
        DIR *d = opendir(SCAN_DIRS[i]);
        snprintf(tmp, sizeof(tmp), "%s{\"base\":\"%s\",\"exists\":%s,\"entries\":[",
                 i ? "," : "", SCAN_DIRS[i], d ? "true" : "false");
        buf = dbg_append(buf, &len, &cap, tmp);
        if (d) {
            struct dirent *de;
            int first = 1;
            while ((de = readdir(d))) {
                char dir[PATH_MAX];
                if (de->d_name[0] == '.') continue;
                snprintf(dir, sizeof(dir), "%s/%s", SCAN_DIRS[i], de->d_name);
                snprintf(tmp, sizeof(tmp),
                         "%s{\"name\":\"%s\",\"pj\":%s,\"pkg\":%s,\"lnk\":%s}",
                         first ? "" : ",", de->d_name,
                         path_exists(dir, "sce_sys/param.json") ? "true" : "false",
                         path_exists(dir, "app.pkg") ? "true" : "false",
                         (path_exists(dir, "mount.lnk") ||
                          path_exists(dir, "mount_img.lnk")) ? "true" : "false");
                buf = dbg_append(buf, &len, &cap, tmp);
                first = 0;
            }
            closedir(d);
        }
        buf = dbg_append(buf, &len, &cap, "]}");
    }

    if (using_vswap)
        kernel_set_proc_rootdir(pid, saved_root);

    buf = dbg_append(buf, &len, &cap, "]}");
    return buf;
}
