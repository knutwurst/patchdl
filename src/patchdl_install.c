#include "patchdl_install.h"

#include <ps5/kernel.h>

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

static intptr_t
dynsym(const char *module, const char *sym) {
    uint32_t h = 0;
    if (kernel_dynlib_handle(-1, module, &h) < 0)
        return 0;
    return kernel_dynlib_dlsym(-1, h, sym);
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

int
patchdl_install_local_pkg(const char *local_path, const char *expected_title_id,
                          char *msg, size_t msg_sz) {
    char        sdk_path[1024];
    struct stat st;
    int         rc;

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

    /* The install service runs in its own sandbox that sees the user
       partition as /user/data, not /data — remap so it can read the file. */
    if (!strncmp(local_path, "/data/", 6))
        snprintf(sdk_path, sizeof(sdk_path), "/user%s", local_path);
    else
        snprintf(sdk_path, sizeof(sdk_path), "%s", local_path);

    /* GUARD: read the PKG's own title id and refuse if it does not match the
       installed game. A cross-title/region package (e.g. a US PPSA03098 patch
       on an EU PPSA03099 install) would otherwise be registered as a separate
       phantom title instead of patching the game. */
    if (ai_title_from_pkg && expected_title_id && expected_title_id[0]) {
        char pkg_tid[48] = {0};
        int  is_app = 0;
        if (ai_title_from_pkg(sdk_path, pkg_tid, &is_app) == 0 && pkg_tid[0] &&
            strncmp(pkg_tid, expected_title_id, 9) != 0) {
            snprintf(msg, msg_sz,
                     "refused: package is for %.12s, installed game is %.12s "
                     "(cross-title/region)", pkg_tid, expected_title_id);
            return -1;
        }
    }

    /* Primary: direct package install. */
    {
        ai_pkg_info_t pkg = {0};
        rc = ai_install_pkg(sdk_path, &pkg);
        if (rc == 0) {
            snprintf(msg, msg_sz, "install started (AppInstallPkg)");
            return 0;
        }
    }

    /* Fallback: InstallByPackage with a file:// URI. */
    {
        char             file_uri[1100];
        ai_meta_info_t   meta   = {0};
        ai_pkg_info_t    pkg    = {0};
        ai_playgo_info_t playgo = {0};
        int              rc2;

        snprintf(file_uri, sizeof(file_uri), "file://%s", sdk_path);
        meta.uri                = file_uri;
        meta.ex_uri             = "";
        meta.playgo_scenario_id = "";
        meta.content_id         = "";
        meta.content_name       = "PatchDL";
        meta.icon_url           = "";

        rc2 = ai_install_by_package(&meta, &pkg, &playgo);
        if (rc2 == 0) {
            snprintf(msg, msg_sz, "install started (InstallByPackage)");
            return 0;
        }
        snprintf(msg, msg_sz,
                 "install rejected (AppInstallPkg=0x%08x, InstallByPackage=0x%08x)",
                 (unsigned)rc, (unsigned)rc2);
        return rc2;
    }
}
