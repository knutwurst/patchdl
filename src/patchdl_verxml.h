#pragma once

#include <stdint.h>

typedef struct {
    char compatible_version[16]; /* highest pkg with system_ver <= fw_bin, or "" */
    char latest_version[16];     /* highest pkg overall, or "" */
    char latest_required_fw[16]; /* fw str for latest pkg, e.g. "11.60", or "" */
    char compatible_url[512];    /* manifest_url if present, otherwise pkg URL */
    char compatible_title[16];   /* target title id from version.xml/manifest_url */
    char compatible_storage_title[16]; /* title id embedded in delta_url storage path */
} patchdl_verinfo_t;

int patchdl_verxml_query(const char *url, uint32_t fw_bin, patchdl_verinfo_t *out);
