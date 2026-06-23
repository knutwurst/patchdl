# PatchDL

A standalone PlayStation 5 ELF payload that downloads and installs official game
patches on your terms. It serves its own web UI and runs without etaHEN.

PatchDL is built for setups where nanoDNS blocks Sony's servers for the whole
console. It resolves the Sony patch CDN on its own path, so the rest of the
system stays offline and only the patches you pick get fetched.

by Knutwurst

## What it does

- Scans installed titles and classifies each one: genuine install,
  ShadowMountPlus mount, preinstall, or unknown.
- Reads the title name, installed version, and the Sony `version.xml` URL from
  the PS5 app database.
- Fetches each title's `version.xml` from Sony's CDN past nanoDNS (a raw DNS
  query to 1.1.1.1) and verifies TLS against the pinned SCEI DNAS root.
- Picks the newest patch compatible with the current firmware
  (`system_ver <= firmware`), so an update never forces a firmware upgrade.
- Downloads the patch package and installs it through Sony's AppInstUtil
  service.

## Safety model

Deny-by-default. A patch is installed only for a genuine install, and only when
the package's title id matches the installed game:

| Source                | Check | Download | Install |
|-----------------------|-------|----------|---------|
| official              | yes   | yes      | yes     |
| shadowmount           | yes   | yes      | no      |
| preinstall / unknown  | yes   | no       | no      |

Two independent guards stop the wrong package being installed: the patch's title
id (read from its download URL) must match the game, and just before install the
real title id is read back from the package
(`sceAppInstUtilGetTitleIdFromPkg`) and checked again. A cross-region or
cross-title package is refused instead of installed as a phantom title.

## Build

Requires `ps5-payload-dev/sdk`. The network and install features also need the
prebuilt libcurl + OpenSSL from `ps5-payload-dev/pacbrew-repo` placed in the SDK
sysroot (`target/user/homebrew`); `scripts/build_ps5.sh` enables them
automatically when present. libmicrohttpd is vendored under `vendor/etahen`, and
SQLite is vendored under `vendor/sqlite`.

```sh
scripts/build_ps5.sh        # produces patchdl-ps5.elf
```

## Deploy

This console uses the BD-JB autoloader with itsPLK's Payload Manager on port
8084 (not a 9021 elfldr). `scripts/deploy_ps5.sh` uploads the ELF named with its
version and launches it; the payload replaces any running instance itself.

```sh
PS5_HOST=<console-ip> scripts/deploy_ps5.sh
```

On start it shows an on-screen notification with the URL. Open the web UI at:

```text
http://<console-ip>:12880/
```

## Status

0.0.1, early. Title scan, version resolution, firmware-compatibility filtering,
download, and install work and have been verified on firmware 11.60. Open items:
the web UI marks a title "Installing…" but reads progress from the PS5's own
notifications rather than a percentage; config persistence and a download queue
are not built yet; disc-based games need the disc inserted for their patch to
apply (a normal Sony requirement).
