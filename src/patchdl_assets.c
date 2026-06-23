#include "patchdl_assets.h"

#include <stdlib.h>
#include <string.h>

static patchdl_asset_t* asset_head;

void
patchdl_asset_register(const char* path, const void* data, size_t size,
                       const char* mime) {
  patchdl_asset_t* asset = calloc(1, sizeof(*asset));

  if(!asset) {
    return;
  }

  asset->path = path;
  asset->data = data;
  asset->size = size;
  asset->mime = mime;
  asset->next = asset_head;
  asset_head = asset;
}

const patchdl_asset_t*
patchdl_asset_find(const char* path) {
  const patchdl_asset_t* asset = asset_head;

  if(!path || !strcmp(path, "/")) {
    path = "/index.html";
  }

  while(asset) {
    if(!strcmp(asset->path, path)) {
      return asset;
    }
    asset = asset->next;
  }

  return 0;
}
