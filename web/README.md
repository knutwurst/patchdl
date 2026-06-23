# PatchDL Web UI

Standalone static UI for the planned PS5 `patchdl.elf` web server.

Open `index.html` directly for the mock UI, or serve this directory from the
ELF web server. The JavaScript first tries the real API and falls back to demo
data when the API is not available.

The embedded ELF server serves this UI on port `12880` by default.

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
  "source_policy": {
    "official": { "allow_check": true, "allow_download": true, "allow_install": true },
    "external": { "allow_check": true, "allow_download": true, "allow_install": true },
    "shadowmount": { "allow_check": true, "allow_download": true, "allow_install": false },
    "unknown": { "allow_check": true, "allow_download": false, "allow_install": false }
  },
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

Per-title source fields:

```json
{
  "title_id": "PPSA90001_00",
  "name": "Shadowmounted Test Title",
  "source_type": "shadowmount",
  "source_path": "/system_ex/app/PPSA90001_00",
  "mount_from": "/mnt/usb0/itemzflow/Shadowmounted Test Title",
  "enabled": true,
  "mode": "download_only"
}
```

Supported `source_type` values:

```text
official
external
shadowmount
unknown
```

The frontend treats `shadowmount` as download-only and `unknown` as blocked for
downloads and installs. Backend code should enforce the same policy even if a
client sends a forged request.
