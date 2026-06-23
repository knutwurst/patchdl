#pragma once

#include <stddef.h>

typedef struct {
    char title_id[16];
    char title_name[128];
    char content_id[64];
    char content_version[16];
    char version_file_uri[256]; /* Sony version.xml URL (UUID assigned) */
} patchdl_appinfo_t;

/* Read the PS5 app database (tbl_contentinfo) for installed-title metadata.
   Returns 0 on success with a malloc'd array (caller frees with
   patchdl_appdb_free). Requires the root-vnode swap + elevated authid to be
   in effect (the caller arranges this). */
int  patchdl_appdb_load(patchdl_appinfo_t **out, size_t *count);
void patchdl_appdb_free(patchdl_appinfo_t *arr);

const patchdl_appinfo_t *patchdl_appdb_find(const patchdl_appinfo_t *arr,
                                            size_t count, const char *title_id);
