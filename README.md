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
(not a 9021 elfldr). `scripts/deploy_ps5.sh` uploads the version-named ELF and
launches it; the payload replaces any running instance.

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
download pool, reboot-safe resume, and on-device SHA-256 verification. A full
Dead Island 2 update (61.6 GB) downloaded and verified byte-perfect across
several reboots.

Install works for same-region patches, where Sony stores the patch bytes under
the installed game's own title id. PatchDL now mirrors etaHEN's native
DirectPKGInstaller call shape for that path: it passes an empty
`MetaInfo.content_id`, lets AppInstUtil bind the signed package metadata, keeps
the returned content id, and exposes `/api/installstatus` for installer progress
when `sceAppInstUtilGetInstallStatus` is exported.

Cross-region patches are a known limitation. Sony sometimes packages a regional
patch under a different (master) storage title and ships it as a debug-magic
container. PatchDL downloads such a patch and verifies it against Sony's hashes,
but refuses to install it from the standalone ELF because `InstallByPackage`
does not retarget signed package metadata on 11.60, and the homebrew alternative
(BGFT register) returns "not supported" outside the system process. Installing
that class of patch needs Sony's authenticated updater, which nanoDNS blocks.
Disc games need the disc inserted for their patch to apply, which is a normal
Sony requirement.
