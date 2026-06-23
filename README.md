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
Makefile
  PS5 payload build using ps5-payload-sdk and libmicrohttpd.

src/
  Embedded web server entrypoint and API stubs.

web/
  Static web UI prototype for the future embedded PatchDL web server.
```

Open `web/index.html` directly for the mock UI, or serve `web/` from a local
HTTP server.

## Embedded Web Server

PatchDL is intended to run one self-contained ELF. It does not need a second
`websrv.elf` process. The payload starts its own libmicrohttpd server and serves
the UI from compiled-in assets.

Default port:

```text
http://PS5_IP:12880/
```

The port can be changed at build time:

```sh
make PATCHDL_HTTP_PORT=12881
```

or at runtime by passing a port as the first argument, if the loader supports
argv:

```sh
patchdl-ps5.elf 12881
```
