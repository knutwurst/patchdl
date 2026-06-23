# PatchDL

PatchDL is a planned standalone PS5 ELF payload for controlled official game
patch downloads.

The target design is deny-by-default:

- Sony domains stay blocked by nanoDNS for the normal PS5 system.
- PatchDL uses its own internal allowlist resolver for Sony CDN hosts.
- Games must be explicitly enabled before PatchDL checks or downloads updates.
- Patch selection is capped to the highest update compatible with the current
  firmware.
- Install actions are source-aware: shadowmounted and unknown titles are never
  installed by PatchDL.

## Source Classification

PatchDL treats the title source as part of the safety policy:

```text
official     check/download/install allowed
external     check/download/install allowed when detected as a real install
shadowmount  check/download allowed, install blocked
unknown      check allowed, download/install blocked
```

The future ELF scanner should classify titles by app metadata plus mount table
inspection. Shadowmounts should be detected through `statfs()` / `getfsstat()`
and `nullfs` mount origins instead of trusting title IDs alone.

## Current Contents

```text
web/
  Static web UI prototype for the future embedded PatchDL web server.
```

Open `web/index.html` directly for the mock UI, or serve `web/` from a local
HTTP server.
