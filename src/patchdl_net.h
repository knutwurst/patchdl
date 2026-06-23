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

/* Progress callback. Return non-zero to ABORT the in-flight download (used to
   cancel large patch downloads); return 0 to continue. */
typedef int (*patchdl_download_progress_cb)(void *ctx,
                                            long long downloaded,
                                            long long total);

/* Stream a URL to a file on disk (for large PKG downloads). Returns 0 on
   success and writes the byte count to *bytes_out. */
int patchdl_http_download(const char *url, const char *dest_path,
                          long long *bytes_out);
int patchdl_http_download_progress(const char *url, const char *dest_path,
                                   long long *bytes_out,
                                   patchdl_download_progress_cb cb, void *ctx);

/* Download a Sony JSON package manifest by concatenating every entry in
   "pieces" into one installable PKG. */
int patchdl_http_download_manifest(const char *manifest_url, const char *dest_path,
                                   long long *bytes_out);
int patchdl_http_download_manifest_progress(const char *manifest_url,
                                            const char *dest_path,
                                            long long *bytes_out,
                                            patchdl_download_progress_cb cb,
                                            void *ctx);

/* Diagnostic: run the GET pipeline for `url` and write a JSON report
   (dns result/ip, curl code, http status, bytes) into `out_json`. */
void patchdl_net_diag(const char *url, char *out_json, size_t sz);
