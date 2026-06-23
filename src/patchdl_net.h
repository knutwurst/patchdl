#pragma once

#include <stddef.h>

typedef struct {
    char  *data;
    size_t size;
    size_t cap;
} patchdl_buf_t;

patchdl_buf_t *patchdl_buf_new(void);
void           patchdl_buf_free(patchdl_buf_t *b);

int patchdl_http_get(const char *url, patchdl_buf_t *out);

/* Stream a URL to a file on disk (for large PKG downloads). Returns 0 on
   success and writes the byte count to *bytes_out. */
int patchdl_http_download(const char *url, const char *dest_path,
                          long long *bytes_out);

/* Diagnostic: run the GET pipeline for `url` and write a JSON report
   (dns result/ip, curl code, http status, bytes) into `out_json`. */
void patchdl_net_diag(const char *url, char *out_json, size_t sz);
