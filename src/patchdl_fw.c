#include "patchdl_fw.h"

#include <ps5/kernel.h>
#include <stdio.h>
#include <string.h>

/* Layout per ps5-payload-dev/shsrv (verified on fw 11.60). The leading
   `unknown1` field is why a naive {str,bin} struct reads garbage. */
typedef struct {
    unsigned long unknown1;
    char          str_version[0x1c];
    unsigned int  bin_version;
    unsigned long unknown2;
} sce_version_t;

/* Resolved from libkernel_sys at runtime on PS5 */
extern int sceKernelGetProsperoSystemSwVersion(sce_version_t *ver);

/* bin_version packs the firmware as hex digits read as decimal:
   0x11600000 -> "11.60". */
static void
bin_to_str(uint32_t bin, char *out, size_t sz) {
    snprintf(out, sz, "%x.%02x", (bin >> 24) & 0xff, (bin >> 16) & 0xff);
}

int
patchdl_fw_get(patchdl_fw_t *out) {
    sce_version_t v;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    memset(&v, 0, sizeof(v));

    if (sceKernelGetProsperoSystemSwVersion(&v) == 0 && v.bin_version) {
        out->bin = v.bin_version;
        bin_to_str(out->bin, out->str, sizeof(out->str));
        return 0;
    }

    /* Fallback: kernel_get_fw_version reads the live FW build from the
       kernel when the SCE call is unavailable. */
    out->bin = kernel_get_fw_version();
    if (out->bin)
        bin_to_str(out->bin, out->str, sizeof(out->str));
    return 0;
}
