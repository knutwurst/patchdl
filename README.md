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
- Downloads the installable package from Sony's manifest pieces and installs it
  through Sony's AppInstUtil service.

## Safety model

Deny-by-default. A patch is installed only for a genuine install, and only when
the patch metadata targets the installed game:

| Source                | Check | Download | Install |
|-----------------------|-------|----------|---------|
| official              | yes   | yes      | yes     |
| shadowmount           | yes   | yes      | no      |
| preinstall / unknown  | yes   | no       | no      |

Two independent guards stop the wrong target being installed: the patch target
id (read from `version.xml` / `manifest_url`) must match the installed game, and
the install call receives the installed game's content id from app.db. Sony may
store the actual patch bytes under a regional/master title id that differs from
the target; that storage id is accepted only when `version.xml` targets the
installed title. A true target-title mismatch is refused instead of installed as
a phantom title.

For PS5 titles, `delta_url` often points to a small `*-DP.pkg` helper package.
That bootstrap can make the system fetch the full patch, but it follows the
package's storage/master title id and can create a duplicate/ghost title for
cross-region updates. PatchDL therefore prefers the Sony `manifest_url`,
downloads every listed `pieces[]` entry in order, and concatenates them into one
local `.pkg` before handing it to AppInstUtil. The `delta_url` title id is kept
only as the storage/master-id diagnostic.

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

0.0.3, early. Title scan, source classification, version resolution,
firmware-compatibility filtering, target/storage-id handling, and the local
AppInstUtil HTTP stream have been verified on firmware 11.60. PatchDL now
downloads PS5 update manifests as merged piece packages under `/data/patchdl`;
large retail updates can be tens of GB. The download queue shows live progress,
and each download can be cancelled (the partial file is deleted) or a finished
package deleted again, from the queue or the title card. Manifest pieces are
verified in offset order and against their declared size while merging. Open
items: a full large-title manifest download/install still needs an end-to-end
run, the web UI marks a title "Installing…" but reads progress from the PS5's
own notifications rather than a percentage, and disc-based games need the disc
inserted for their patch to apply (a normal Sony requirement).
Settings (global policy and the per-game toggle) persist to
`/data/patchdl/config.json` and survive a restart.
