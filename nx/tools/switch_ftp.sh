#!/bin/bash
# Sends the build to the Switch over Wi-Fi (FTP) and fetches its logs back.
#
#   SWITCH_IP=192.168.1.42 nx/tools/switch_ftp.sh push    # nx/out/sdcard/switch/mgsv-nx -> sdmc:/switch/mgsv-nx
#   SWITCH_IP=192.168.1.42 nx/tools/switch_ftp.sh logs    # sdmc:/switch/mgsv-nx/logs   -> nx/out/console-logs
#
# The Switch must be running an FTP server (ftpd, or Sphaira's built-in one) on the same network; it shows its
# IP address and port on screen. SWITCH_PORT defaults to 5000 (ftpd's default). Login is anonymous unless
# FTP_USER / FTP_PASS are set.
#
# "push" only sends files that are new or changed, so after the first full copy (~1.6 GB, several minutes over
# Wi-Fi) a rebuilt NRO is the only thing that goes over. It refreshes the SD folder from the latest build first.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
: "${SWITCH_IP:?set SWITCH_IP to the address shown by the FTP app on the Switch}"
PORT=${SWITCH_PORT:-5000}
LOCAL=$ROOT/nx/out/sdcard/switch/mgsv-nx
REMOTE=/switch/mgsv-nx
LOGIN=""
if [ -n "${FTP_USER:-}" ]; then LOGIN="user $FTP_USER ${FTP_PASS:-};"; fi
SETTINGS="set ftp:passive-mode on; set net:timeout 20; set net:max-retries 3; set net:reconnect-interval-base 5; set ftp:sync-mode off;"

case "${1:-push}" in
  push)
    "$ROOT/nx/tools/package_sd.sh" > /dev/null
    echo "Uploading $LOCAL -> ftp://$SWITCH_IP:$PORT$REMOTE (only new or changed files)"
    lftp -p "$PORT" -e "$SETTINGS $LOGIN mkdir -p -f $REMOTE; mirror --reverse --only-newer --no-perms --verbose=1 --exclude-glob .DS_Store --exclude-glob ._* '$LOCAL' '$REMOTE'; bye" "$SWITCH_IP"
    echo "Done. Launch mgsv_gz.nro from the Homebrew Menu (hold R while starting a game)."
    ;;
  logs)
    OUT=$ROOT/nx/out/console-logs
    mkdir -p "$OUT"
    lftp -p "$PORT" -e "$SETTINGS $LOGIN mirror --verbose=1 '$REMOTE/logs' '$OUT'; bye" "$SWITCH_IP"
    echo "Logs in $OUT"
    ;;
  *)
    echo "usage: SWITCH_IP=<ip> $0 push|logs" >&2
    exit 2
    ;;
esac
