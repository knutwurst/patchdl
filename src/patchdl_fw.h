#pragma once

#include <stdint.h>

typedef struct {
    char     str[0x1c]; /* e.g. "11.60" */
    uint32_t bin;       /* e.g. 0x11600000 */
} patchdl_fw_t;

int patchdl_fw_get(patchdl_fw_t *out);
