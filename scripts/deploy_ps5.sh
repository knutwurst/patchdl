#!/bin/sh
# Deploy patchdl to the PS5 via the Payload Manager HTTP API (port 8084).
# The uploaded filename carries the version so it is identifiable in the
# Payload Manager UI. patchdl self-kills any running instance, so this is
# an idempotent redeploy.
#
# Usage: scripts/deploy_ps5.sh [PS5_HOST]
#   PS5_HOST defaults to $PS5_HOST or ps5-slim.fritz.box
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HOST=${1:-${PS5_HOST:-ps5-slim.fritz.box}}
PM_PORT=${PM_PORT:-8084}
HTTP_PORT=${PATCHDL_HTTP_PORT:-12880}

VERSION=$(sed -n 's/.*PATCHDL_VERSION[^"]*"\([^"]*\)".*/\1/p' "$ROOT_DIR/src/patchdl_version.h")
[ -n "$VERSION" ] || { echo "could not read PATCHDL_VERSION" >&2; exit 1; }

SRC_ELF="$ROOT_DIR/patchdl-ps5.elf"
[ -f "$SRC_ELF" ] || { echo "build first: $SRC_ELF missing" >&2; exit 1; }

UP_NAME="patchdl-ps5-v${VERSION}.elf"
TMP_ELF="$ROOT_DIR/$UP_NAME"
cp "$SRC_ELF" "$TMP_ELF"

echo "Uploading $UP_NAME to $HOST:$PM_PORT ..."
curl -fsS -m120 -X POST "http://$HOST:$PM_PORT/manage:upload?filename=$UP_NAME" \
  -H 'Content-Type: application/octet-stream' --data-binary @"$TMP_ELF" >/dev/null
rm -f "$TMP_ELF"

PAYLOAD_PATH=$(curl -fsS -m15 "http://$HOST:$PM_PORT/list_payloads" | \
  UP_NAME="$UP_NAME" python3 -c '
import os, sys, json
want = os.environ["UP_NAME"]
obj = json.load(sys.stdin)
paths = obj.get("payloads", obj) if isinstance(obj, dict) else obj
for p in paths:
    if isinstance(p, str) and p.endswith(want):
        print(p); break
')
[ -n "$PAYLOAD_PATH" ] || { echo "uploaded payload not found in list_payloads" >&2; exit 1; }

echo "Launching $PAYLOAD_PATH ..."
curl -fsS -m20 "http://$HOST:$PM_PORT/loadpayload:$PAYLOAD_PATH" >/dev/null

printf "Waiting for web server on :%s " "$HTTP_PORT"
i=0
while [ "$i" -lt 20 ]; do
  if curl -fsS -m4 "http://$HOST:$HTTP_PORT/api/status" >/dev/null 2>&1; then
    echo " up."
    curl -fsS -m6 "http://$HOST:$HTTP_PORT/api/status"; echo
    echo "UI: http://$HOST:$HTTP_PORT/"
    exit 0
  fi
  printf "."
  sleep 2
  i=$((i + 1))
done
echo " timeout — server did not answer." >&2
exit 1
