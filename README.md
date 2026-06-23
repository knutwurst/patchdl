# PatchDL

PatchDL is a planned standalone PS5 ELF payload for controlled official game
patch downloads.

The target design is deny-by-default:

- Sony domains stay blocked by nanoDNS for the normal PS5 system.
- PatchDL uses its own internal allowlist resolver for Sony CDN hosts.
- Games must be explicitly enabled before PatchDL checks or downloads updates.
- Patch selection is capped to the highest update compatible with the current
  firmware.

## Current Contents

```text
web/
  Static web UI prototype for the future embedded PatchDL web server.
```

Open `web/index.html` directly for the mock UI, or serve `web/` from a local
HTTP server.

