#pragma once

#include <stddef.h>

/* Install / refresh the PatchDL home-screen tile.
 *
 * Approach (from itsPLK's ps5-payload-manager/app_installer.c, which adapted
 * John Tornblom's ftpsrv work): write param.json + icon0.png into
 *   /user/app/<TITLE_ID>/sce_sys/
 * then call sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0)
 * which registers the directory as an app. The tile's deeplinkUri opens
 * Sony's WebKit browser at http://127.0.0.1:12880/, i.e. PatchDL's own UI
 * — only useful while the ELF is running.
 *
 * No PKG, no code signing, no debug-magic. Files only — Sony's installer
 * registers the directory as an app.
 *
 * stat-guard: the asset bytes are diffed against the on-disk copy first;
 * if nothing changed the install API isn't called at all (avoids a costly
 * re-register every payload start, and avoids the dangerous CancelInstall
 * code path). Returns 0 on success or when nothing needed doing.
 */
int patchdl_tile_install_if_needed(char *msg, size_t msg_sz);
