#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PATCHDL_SOURCE_OFFICIAL    = 0,
    PATCHDL_SOURCE_EXTERNAL    = 1,
    PATCHDL_SOURCE_SHADOWMOUNT = 2,
    PATCHDL_SOURCE_UNKNOWN     = 3,
} patchdl_source_t;

typedef struct {
    char             title_id[32];
    char             content_id[64];
    char             name[128];
    char             installed_version[16];
    char             source_path[256];
    char             mount_from[256];
    char             version_file_uri[256]; /* from app.db, for version.xml */
    patchdl_source_t source_type;
    /* populated by background verxml fetch — guarded by websrv mutex */
    char             compatible_version[16];
    char             latest_version[16];
    char             latest_required_fw[16];
    char             patch_url[512];   /* delta_url of the compatible patch */
    char             patch_title_id[16]; /* title id embedded in patch_url */
    int              verxml_done;
} patchdl_title_t;

int         patchdl_scan(patchdl_title_t **titles_out, size_t *count_out);
void        patchdl_scan_free(patchdl_title_t *titles, size_t count);
const char *patchdl_source_str(patchdl_source_t src);

/* Diagnostic: malloc'd JSON dump of the mount table + scan-base directory
   listings. Caller frees. */
char       *patchdl_scan_debug_json(void);
