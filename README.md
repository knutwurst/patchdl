# PatchDL

A standalone PlayStation 5 ELF payload that downloads and installs official game
patches on your terms. It serves its own dark-mode web UI and runs without
etaHEN.

PatchDL is built for setups where nanoDNS blocks Sony's servers for the whole
console. It resolves the Sony patch CDN on its own DNS path, so the rest of the
system stays offline and only the patches you pick get fetched.

by Knutwurst

## What it does

- Scans installed titles and classifies each: genuine install, ShadowMountPlus
  mount, preinstall, or unknown.
- Reads the title name, installed version, and Sony `version.xml` URL from the
  PS5 app database.
- Fetches each title's `version.xml` past nanoDNS (a raw DNS query to 1.1.1.1)
  and verifies TLS against the pinned SCEI DNAS root.
- Picks the newest patch compatible with the current firmware
  (`system_ver <= firmware`), so an update never forces a firmware upgrade.
- Downloads the patch from Sony's manifest pieces into one local `.pkg`, then
  installs it through Sony's AppInstUtil service.

## Downloading

PatchDL pulls each patch over a pool of connections instead of one stream, which
lifts the ~7 MB/s per-connection ceiling on the Sony CDN. Set the connection
count (1 to 16) in Settings with the stepper; the change applies live, with no
payload restart. One patch downloads at a time and further requests queue.

Downloads survive interruptions:

- **Resume across a reboot.** PatchDL records progress per manifest piece in a
  sidecar beside the `.pkg`, written after every completed piece, so a reboot or
  relaunch continues where it stopped.
- **Pause, Resume, Cancel.** Pause keeps the partial file, Resume continues it,
  Cancel deletes it. All three work at any point in a download.
- **Verification.** Turn on "Verify downloaded pieces (SHA-256)" to check each
  piece against its manifest hash while downloading. After a download you can
  also verify the assembled package on-device against Sony's per-piece hashes
  (`GET /api/pkgverify/<title_id>`).

Patches download to `/data/patchdl` on the internal SSD. Large retail updates
run tens of GB.

## Web UI

The Games view groups titles into filter chips:

- **Updatable** — has an installable update; selected by default.
- **Updating** — queued, actively downloading, paused with a partial on disk,
  or installing.
- **Up to date**, **Needs FW**, **Can't update** — the rest.
- **All** — flat list at the right end of the strip.

**Update all** in the top bar queues a download for every game with an
installable update in one click. Shadowmounts are skipped (they pass the
download policy but not the install policy), so a sweep doesn't burn tens of
GB on bytes AppInstUtil would refuse — pick those up by hand when the disc is
ready.

## Safety model

Deny-by-default. A patch installs only for a genuine install, and only when the
patch metadata targets the installed game:

| Source                | Check | Download | Install |
|-----------------------|-------|----------|---------|
| official              | yes   | yes      | yes     |
| shadowmount           | yes   | yes      | no      |
| preinstall / unknown  | yes   | no       | no      |

Two guards stop the wrong target being installed: the patch target id (from
`version.xml` / `manifest_url`) must match the installed game, and the install
call receives the installed game's content id from app.db. PatchDL refuses a
true target-title mismatch rather than installing a phantom title.

All writes stay under `/data/patchdl`. PatchDL never writes to the system
partition and never touches firmware.

## Settings

Settings persist to `/data/patchdl/config.json` and survive a restart:

- Default policy (allow or deny) and a per-game enable toggle.
- Install after download, as a global default with a per-game override.
- Delete the PKG after a successful install.
- Verify downloaded pieces (SHA-256).
- Parallel download connections (1 to 16), applied live.

## Build

Requires `ps5-payload-dev/sdk`. The network and install features need the
prebuilt libcurl + OpenSSL from `ps5-payload-dev/pacbrew-repo` in the SDK sysroot
(`target/user/homebrew`); `scripts/build_ps5.sh` enables them when present.
libmicrohttpd is vendored under `vendor/etahen`, SQLite under `vendor/sqlite`.

```sh
scripts/build_ps5.sh        # produces patchdl-ps5.elf
```

## Deploy

This console uses the BD-JB autoloader with itsPLK's Payload Manager on port 8084
(not a 9021 elfldr). `scripts/deploy_ps5.sh` uploads the version-named ELF
(`patchdl_<version>.elf`, so Payload Manager picks the version out of the
filename) and launches it; the payload replaces any running instance.

```sh
PS5_HOST=<console-ip> scripts/deploy_ps5.sh
```

On start it shows an on-screen notification with the URL. Open the web UI at:

```text
http://<console-ip>:12880/
```

## Status

Verified on firmware 11.60: title scan, source classification, version
resolution past the nanoDNS block, firmware-compatibility filtering, the parallel
download pool, reboot-safe resume, and on-device SHA-256 verification.

Install works through Sony's AppInstUtil. `sceAppInstUtilInstallByPackage` is
unavailable from the homebrew payload context (rejected with `0x80B21163`
outside the system process), so PatchDL routes through
`sceAppInstUtilAppInstallPkg` — the simpler API that reads the PKG's embedded
`content_id` and binds the install to the right title slot. Verified
end-to-end: Dead Island 2 (`PPSA03099`) updated from 01.000.001 to 01.000.011
on 11.60, including the cross-region shared-storage case where Sony serves the
patch under a master title id.

`/api/installstatus` reports installer progress once
`sceAppInstUtilGetInstallStatus` finishes the queued task.

Disc games still need the disc inserted for their patch to apply, which is a
normal Sony requirement; that's why shadowmounts are download-only by policy.
