#pragma once

#include <stddef.h>

typedef struct patchdl_asset {
  const char* path;
  const void* data;
  size_t size;
  const char* mime;
  struct patchdl_asset* next;
} patchdl_asset_t;

void patchdl_asset_register(const char* path, const void* data, size_t size,
                            const char* mime);
const patchdl_asset_t* patchdl_asset_find(const char* path);
