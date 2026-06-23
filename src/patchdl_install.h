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
   The PKG's own title id is read and must match, else the install is refused
   (prevents a cross-region/cross-title package being installed as a new
   phantom title). */
int patchdl_install_local_pkg(const char *local_path,
                              const char *expected_title_id,
                              char *msg, size_t msg_sz);

/* Verify the AppInstUtil backend can be loaded + resolved + initialized,
   WITHOUT performing any install. Returns 0 if ready. Safe to call. */
int patchdl_install_backend_check(char *msg, size_t msg_sz);
