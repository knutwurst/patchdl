# PatchDL Web UI

Standalone static UI for the planned PS5 `patchdl.elf` web server.

Open `index.html` directly for the mock UI, or serve this directory from the
ELF web server. The JavaScript first tries the real API and falls back to demo
data when the API is not available.

## Expected API

```text
GET  /api/status
GET  /api/config
POST /api/config
GET  /api/titles
GET  /api/downloads
POST /api/titles/:title_id/check
POST /api/titles/:title_id/download
```

## Policy Model

The UI assumes deny-by-default behavior:

```json
{
  "default_policy": "deny",
  "download_dir": "/mnt/usb0/patches",
  "install_after_download": false,
  "delete_pkg_after_install": false,
  "cdn_allowlist": [
    "sgst.prod.dl.playstation.net",
    "gst.prod.dl.playstation.net",
    "gs2.ww.prod.dl.playstation.net"
  ]
}
```

Per-title modes:

```text
disabled
download_only
latest_compatible
pin
check_only
```
