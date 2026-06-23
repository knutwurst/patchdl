#pragma once

#include <stddef.h>

/*
 * Install a local PKG (a downloaded patch) via Sony's AppInstUtil service.
 * `local_path` is the on-disk path under /data/patchdl/... The install runs
 * asynchronously in the system installer queue once started; this returns 0
 * when the install was accepted, non-zero (the SCE error code) otherwise, and
 * writes a short human-readable status into `msg`.
 *
 * This MODIFIES the installed game. The caller must gate it to genuine
 * (official) titles and obtain explicit user intent before calling.
 */
/* `expected_title_id` is the title id of the installed game the patch is for.
   `storage_title_id` is the title id embedded in the delta_url storage path.
   `target_content_id` is the installed game's content id from app.db; when
   present it is passed to InstallByPackage so Sony's installer has the target
   metadata even for region-shared/master-storage patch bytes. */
int patchdl_install_local_pkg(const char *local_path,
                              const char *expected_title_id,
                              const char *storage_title_id,
                              const char *target_content_id,
                              char *msg, size_t msg_sz);

/* Verify the AppInstUtil backend can be loaded + resolved + initialized,
   WITHOUT performing any install. Returns 0 if ready. Safe to call. */
int patchdl_install_backend_check(char *msg, size_t msg_sz);

/* Read-only feasibility probe: resolve (dlsym, never call) a list of candidate
   AppInstUtil/Bgft patch-install symbols and report which exist on this
   firmware. Writes a JSON object into `out`. No install, no side effects. */
int patchdl_install_api_probe(char *out, size_t out_sz);
