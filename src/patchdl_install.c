#include "patchdl_install.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <ps5/kernel.h>

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>

/* AppInstUtil structs/signatures, reverse-engineered by the PS5 homebrew
   scene (notmaj0r/CheatRunner, ps5-payload-dev/websrv). Sizes per CheatRunner:
   CONTENTID=0x30, PLAYGOSCENARIOID=3, LANGUAGE=8, NUM_LANGUAGES=30, NUM_IDS=64.
   The trailing `unknown` padding makes the playgo struct safely large. */
#define AI_CONTENTID_SIZE        0x30
#define AI_PLAYGOSCENARIOID_SIZE 3
#define AI_LANGUAGE_SIZE         8
#define AI_NUM_LANGUAGES         30
#define AI_NUM_IDS               64

typedef struct {
    char content_id[AI_CONTENTID_SIZE];
    int  content_type;
    int  content_platform;
} ai_pkg_info_t;

typedef struct {
    const char *uri;
    const char *ex_uri;
    const char *playgo_scenario_id;
    const char *content_id;
    const char *content_name;
    const char *icon_url;
} ai_meta_info_t;

typedef struct {
    char languages[AI_NUM_LANGUAGES][AI_LANGUAGE_SIZE];
    char playgo_scenario_ids[AI_NUM_IDS][AI_PLAYGOSCENARIOID_SIZE];
    char content_ids[AI_NUM_IDS][AI_CONTENTID_SIZE];
    long unknown[810];
} ai_playgo_info_t;

/* Sysmodule IDs (from ps5-payload-dev/sdk crt/rtld_sprx.c). */
#define SYSMOD_IPMI           0x8000001d
#define SYSMOD_USERSERVICE    0x80000011
#define SYSMOD_SYSTEMSERVICE  0x80000010
#define SYSMOD_APPINSTUTIL    0x80000014

typedef int (*sysmod_load_fn)(unsigned int id);
typedef int (*ai_init_fn)(void);
typedef int (*ai_install_pkg_fn)(const char *path, ai_pkg_info_t *info);
typedef int (*ai_install_by_pkg_fn)(ai_meta_info_t *meta, ai_pkg_info_t *info,
                                    ai_playgo_info_t *playgo);
typedef int (*ai_title_from_pkg_fn)(const char *path, char *title_id, int *is_app);

static ai_init_fn           ai_initialize;
static ai_install_pkg_fn    ai_install_pkg;
static ai_install_by_pkg_fn ai_install_by_package;
static ai_title_from_pkg_fn ai_title_from_pkg;

/* Resolve + initialize the AppInstUtil backend WITHOUT linking the sce libs
   (that makes the ELF unloadable by the elfldr) and WITHOUT raw
   sceKernelLoadStartModule (that wedged the process). Instead use the proper
   sysmodule loader — the same path the SDK's rtld_sprx takes — then resolve
   symbols via the kernel dynlib helpers. Runs in a detached thread; the HTTP
   handler reports the stage and never blocks.
   stage: 0 idle, 1 resolve loader, 2 load modules, 3 resolve symbols,
   4 initialize, 5 ready, negative = failure at that step. */
static volatile int    g_stage;
static int             g_err;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static char            g_probe_json[2048]; /* filled by the backend thread */

static intptr_t
dynsym(const char *module, const char *sym) {
    uint32_t h = 0;
    if (kernel_dynlib_handle(-1, module, &h) < 0)
        return 0;
    return kernel_dynlib_dlsym(-1, h, sym);
}

static void
local_ip(char *out, size_t n) {
    struct ifaddrs *ifa = NULL, *p;

    out[0] = '\0';
    if (getifaddrs(&ifa))
        return;

    for (p = ifa; p; p = p->ifa_next) {
        char ip[INET_ADDRSTRLEN];
        struct sockaddr_in *s;

        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        s = (struct sockaddr_in *)p->ifa_addr;
        if (!inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip)))
            continue;
        if (strcmp(ip, "127.0.0.1") && strncmp(ip, "0.", 2)) {
            strncpy(out, ip, n - 1);
            out[n - 1] = '\0';
            break;
        }
    }
    freeifaddrs(ifa);
}

/* Resolve (dlsym, never call) a list of candidate patch-install symbols and
   record which exist. Runs inside the backend thread, where the AppInstUtil
   module is already loaded — the same proven-safe context as the normal symbol
   resolution. No sysmod_load and no calls, so it is side-effect free. */
static void
fill_probe(void) {
    static const char *ai_syms[] = {
        "sceAppInstUtilInitialize",
        "sceAppInstUtilAppInstallPkg",
        "sceAppInstUtilAppInstallTitleDir",     /* takes an explicit title id */
        "sceAppInstUtilInstallByPackage",
        "sceAppInstUtilInstallByPackageEx",
        "sceAppInstUtilGetTitleIdFromPkg",
        "sceAppInstUtilGetContentIdFromPkg",
        "sceAppInstUtilAppExist",
        "sceAppInstUtilAppGetInstallStatus",
        "sceAppInstUtilAppInstallStatus",
        "sceAppInstUtilAppUnInstall",
        "sceAppInstUtilGetMetaInfoFromPkg",
        "sceAppInstUtilUpdateTitleByTitleId",
        "sceAppInstUtilInstallByChunk",
        NULL
    };
    static const char *bgft_syms[] = {
        "sceBgftInitialize",
        "sceBgftServiceIntInit",
        "sceBgftServiceDownloadRegisterTask",
        "sceBgftServiceDownloadRegisterTaskByStorage",
        "sceBgftServiceDownloadRegisterTaskByStorageEx",
        "sceBgftServiceIntDownloadRegisterTaskByStorageEx",
        "sceBgftServiceDownloadStartTask",
        "sceBgftServiceDownloadGetProgress",
        "sceBgftServiceInstallPackage",
        NULL
    };
    uint32_t h = 0;
    int ai_loaded   = (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &h) >= 0);
    int bgft_loaded = (kernel_dynlib_handle(-1, "libSceBgft.sprx", &h) >= 0);
    char   tmp[sizeof(g_probe_json)];
    size_t n = 0, sz = sizeof(tmp);
    char  *out = tmp;
    int    first = 1;

    n += snprintf(out + n, sz - n,
                  "{\"appinstutil_loaded\":%s,\"bgft_loaded\":%s,\"symbols\":{",
                  ai_loaded ? "true" : "false", bgft_loaded ? "true" : "false");
    for (int i = 0; ai_syms[i] && n < sz - 80; i++) {
        intptr_t a = dynsym("libSceAppInstUtil.sprx", ai_syms[i]);
        n += snprintf(out + n, sz - n, "%s\"%s\":%s",
                      first ? "" : ",", ai_syms[i], a ? "true" : "false");
        first = 0;
    }
    for (int i = 0; bgft_syms[i] && n < sz - 80; i++) {
        intptr_t a = bgft_loaded ? dynsym("libSceBgft.sprx", bgft_syms[i]) : 0;
        n += snprintf(out + n, sz - n, "%s\"%s\":%s",
                      first ? "" : ",", bgft_syms[i], a ? "true" : "false");
        first = 0;
    }
    snprintf(out + n, sz - n, "}}");

    /* Publish atomically: the getter runs on an MHD worker thread and reads
       g_probe_json under the same lock, so it never sees a half-built buffer. */
    pthread_mutex_lock(&g_mtx);
    memcpy(g_probe_json, tmp, sizeof(g_probe_json));
    pthread_mutex_unlock(&g_mtx);
}

static void *
backend_init_thread(void *arg) {
    (void)arg;
    sysmod_load_fn sysmod_load;

    g_stage = 1;
    sysmod_load = (sysmod_load_fn)dynsym("libSceSysmodule.sprx",
                                         "sceSysmoduleLoadModuleInternal");
    if (!sysmod_load) { g_stage = -1; return NULL; }

    g_stage = 2;
    /* Dependencies first, then AppInstUtil. The sysmodule loader pulls in
       further deps and is the non-wedging path. */
    sysmod_load(SYSMOD_IPMI);
    sysmod_load(SYSMOD_USERSERVICE);
    sysmod_load(SYSMOD_SYSTEMSERVICE);
    g_err = sysmod_load(SYSMOD_APPINSTUTIL);
    if (g_err) { g_stage = -2; return NULL; }

    g_stage = 3;
    ai_initialize         = (ai_init_fn)dynsym("libSceAppInstUtil.sprx",
                                               "sceAppInstUtilInitialize");
    ai_install_pkg        = (ai_install_pkg_fn)dynsym("libSceAppInstUtil.sprx",
                                                      "sceAppInstUtilAppInstallPkg");
    ai_install_by_package = (ai_install_by_pkg_fn)dynsym("libSceAppInstUtil.sprx",
                                                         "sceAppInstUtilInstallByPackage");
    ai_title_from_pkg     = (ai_title_from_pkg_fn)dynsym("libSceAppInstUtil.sprx",
                                                         "sceAppInstUtilGetTitleIdFromPkg");

    /* Read-only feasibility probe — module is loaded, safe context. */
    fill_probe();

    if (!ai_initialize || !ai_install_pkg || !ai_install_by_package) {
        g_stage = -3;
        return NULL;
    }

    g_stage = 4;
    g_err = ai_initialize();
    if (g_err) { g_stage = -4; return NULL; }

    g_stage = 5;
    return NULL;
}

static void
backend_start(void) {
    pthread_mutex_lock(&g_mtx);
    if (g_stage == 0) {
        pthread_t      tid;
        pthread_attr_t attr;
        g_stage = 1;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, backend_init_thread, NULL);
        pthread_attr_destroy(&attr);
    }
    pthread_mutex_unlock(&g_mtx);
}

static const char *
stage_str(int s) {
    switch (s) {
    case 0:  return "idle";
    case 1:  return "resolving sysmodule loader";
    case 2:  return "loading AppInstUtil module";
    case 3:  return "resolving AppInstUtil symbols";
    case 4:  return "initializing service";
    case 5:  return "ready";
    case -1: return "sysmodule loader unavailable";
    case -2: return "AppInstUtil module load failed";
    case -3: return "AppInstUtil symbol resolution failed";
    case -4: return "AppInstUtil init failed";
    default: return "unknown";
    }
}

int
patchdl_install_backend_check(char *msg, size_t msg_sz) {
    int s;
    backend_start();
    s = g_stage;
    snprintf(msg, msg_sz, "%s (rc=0x%08x)", stage_str(s), (unsigned)g_err);
    return (s == 5) ? 0 : -1;
}

/* Public getter: trigger the backend (which fills the probe in its own thread)
   and return the cached result. Runs from the MHD worker thread, so it only
   reads the cached string — it never loads modules or resolves symbols here. */
int
patchdl_install_api_probe(char *out, size_t out_sz) {
    int ready;

    backend_start();
    pthread_mutex_lock(&g_mtx);
    ready = (g_probe_json[0] != '\0');
    if (ready)
        snprintf(out, out_sz, "%s", g_probe_json);
    pthread_mutex_unlock(&g_mtx);
    if (ready)
        return 0;

    snprintf(out, out_sz,
             "{\"pending\":true,\"stage\":\"%s\"}", stage_str(g_stage));
    return -1;
}

int
patchdl_install_local_pkg(const char *local_path, const char *expected_title_id,
                          const char *storage_title_id,
                          const char *target_content_id,
                          char *msg, size_t msg_sz) {
    char        sdk_path[1024];
    char        pkg_tid[48] = {0};
    struct stat st;
    int         rc;
    int         pkg_tid_mismatch = 0;
    const char *last_uri = "";

    if (!local_path || !local_path[0]) {
        snprintf(msg, msg_sz, "no package path");
        return -1;
    }
    if (stat(local_path, &st) != 0) {
        snprintf(msg, msg_sz, "package not downloaded");
        return -1;
    }

    backend_start();
    if (g_stage != 5) {
        snprintf(msg, msg_sz, "install backend not ready: %s", stage_str(g_stage));
        return -1;
    }

    /* AppInstallPkg runs in a sandbox that sees the user partition as
       /user/data, not /data. InstallByPackage is different: the shell/debug
       installer path takes the normal /data/... URI, so keep `local_path` for
       that API and use `sdk_path` only for AppInstallPkg / metadata probes. */
    if (!strncmp(local_path, "/data/", 6))
        snprintf(sdk_path, sizeof(sdk_path), "/user%s", local_path);
    else
        snprintf(sdk_path, sizeof(sdk_path), "%s", local_path);

    /* Diagnostic guard: Sony sometimes stores one patch byte stream under a
       master title id while the version.xml targets a regional title id. That
       is valid only when the caller supplies target metadata, so do not feed
       such packages to the raw AppInstallPkg path. */
    if (storage_title_id && storage_title_id[0] &&
        expected_title_id && expected_title_id[0] &&
        strncmp(storage_title_id, expected_title_id, 9) != 0) {
        pkg_tid_mismatch = 1;
        strncpy(pkg_tid, storage_title_id, sizeof(pkg_tid) - 1);
    }
    if (ai_title_from_pkg && expected_title_id && expected_title_id[0]) {
        int  is_app = 0;
        if (ai_title_from_pkg(sdk_path, pkg_tid, &is_app) == 0 && pkg_tid[0] &&
            strncmp(pkg_tid, expected_title_id, 9) != 0) {
            pkg_tid_mismatch = 1;
        }
    }
    if (pkg_tid_mismatch && (!target_content_id || !target_content_id[0])) {
        snprintf(msg, msg_sz,
                 "refused: package metadata is %.12s, target is %.12s",
                 pkg_tid, expected_title_id);
        return -1;
    }

    /* Preferred path: InstallByPackage accepts target metadata. Use it first,
       and use it exclusively when the downloaded bytes report a master/storage
       title id that differs from the target regional title id. */
    {
        char             file_uri[1100];
        char             http_loop_uri[1200] = {0};
        char             http_lan_uri[1200] = {0};
        const char      *uris[4];
        ai_meta_info_t   meta   = {0};
        ai_pkg_info_t    pkg    = {0};
        ai_playgo_info_t playgo = {0};
        int              rc2    = -1;
        const char      *title_dir;
        const char      *file_base;

        snprintf(file_uri, sizeof(file_uri), "file://%s", local_path);
        title_dir = strstr(local_path, "/data/patchdl/");
        file_base = strrchr(local_path, '/');
        if (title_dir && file_base && file_base > title_dir + strlen("/data/patchdl/")) {
            char title_id[32] = {0};
            const char *t = title_dir + strlen("/data/patchdl/");
            size_t tlen = (size_t)(file_base - t);
            if (tlen > 0 && tlen < sizeof(title_id)) {
                char ip[INET_ADDRSTRLEN] = {0};
                memcpy(title_id, t, tlen);
                snprintf(http_loop_uri, sizeof(http_loop_uri),
                         "http://127.0.0.1:%d/api/pkg/%s/%s",
                         PATCHDL_HTTP_PORT, title_id, file_base + 1);
                local_ip(ip, sizeof(ip));
                if (ip[0])
                    snprintf(http_lan_uri, sizeof(http_lan_uri),
                             "http://%s:%d/api/pkg/%s/%s",
                             ip, PATCHDL_HTTP_PORT, title_id, file_base + 1);
            }
        }
        uris[0]                 = local_path;
        uris[1]                 = file_uri;
        uris[2]                 = http_loop_uri[0] ? http_loop_uri : NULL;
        uris[3]                 = http_lan_uri[0] ? http_lan_uri : NULL;
        meta.ex_uri             = "";
        meta.playgo_scenario_id = "";
        meta.content_id         = target_content_id ? target_content_id : "";
        meta.content_name       = "PatchDL";
        meta.icon_url           = "";

        for (int i = 0; i < 4; i++) {
            if (!uris[i]) continue;
            memset(&pkg, 0, sizeof(pkg));
            memset(&playgo, 0, sizeof(playgo));
            meta.uri = uris[i];
            last_uri = uris[i];
            rc2 = ai_install_by_package(&meta, &pkg, &playgo);
            if (rc2 == 0) {
                snprintf(msg, msg_sz, "install started (InstallByPackage%s)",
                         pkg_tid_mismatch ? ", shared master bytes" : "");
                return 0;
            }
        }
        rc = rc2;
    }

    if (pkg_tid_mismatch) {
        snprintf(msg, msg_sz,
                 "install rejected (InstallByPackage=0x%08x, pkg %.12s, target %.12s, uri %.96s)",
                 (unsigned)rc, pkg_tid, expected_title_id ? expected_title_id : "",
                 last_uri);
        return rc ? rc : -1;
    }

    /* Last resort for normal same-title packages only. This path has no target
       metadata parameter, so it is intentionally skipped for shared-master
       region bytes. */
    {
        ai_pkg_info_t pkg = {0};
        int rc2 = ai_install_pkg(sdk_path, &pkg);
        if (rc2 == 0) {
            snprintf(msg, msg_sz, "install started (AppInstallPkg)");
            return 0;
        }
        snprintf(msg, msg_sz,
                 "install rejected (InstallByPackage=0x%08x, AppInstallPkg=0x%08x)",
                 (unsigned)rc, (unsigned)rc2);
        return rc2 ? rc2 : (rc ? rc : -1);
    }
}
