#!/bin/bash
# Assembles the SD card folder for the Switch build:
#   nx/out/sdcard/switch/mgsv-nx/
#     mgsv_gz.nro
#     game_root/            (your extracted disc: default.xex, Fox/, Tpp/, *.qar, ...)
#
# Copy nx/out/sdcard/switch/mgsv-nx to sdmc:/switch/ on the console (or run with DEST=/Volumes/<SD card>/switch).
# Your game files are never redistributed: this only copies them from ../extracted to your own SD card.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NRO=${NRO:-$ROOT/nx/out/nx/mgsv_gz.nro}
GAME=${GAME:-$ROOT/extracted}
DEST=${DEST:-$ROOT/nx/out/sdcard/switch}
OUT=$DEST/mgsv-nx

[ -f "$NRO" ] || { echo "no NRO at $NRO (build nx/out/nx first)" >&2; exit 1; }
[ -f "$GAME/default.xex" ] || { echo "no default.xex in $GAME" >&2; exit 1; }

mkdir -p "$OUT/game_root"
cp "$NRO" "$OUT/mgsv_gz.nro"
# The native renderer's shader package (made from the game's own shaders by the shader dump + XenosRecomp + DXC).
SHADERS=${SHADERS:-$ROOT/third_party/sa2-nx/out/shaders}
if [ -f "$SHADERS/sa2_shaders.mesp" ]; then
    cp "$SHADERS/sa2_shaders.mesp" "$SHADERS/sa2_shaders.mesp.idx" "$OUT/"
else
    echo "warning: no shader package in $SHADERS (the native renderer will draw nothing)" >&2
fi
# The disc's own files only: leave out the analysis by-products of tools/ (decrypted image, manifest, logs).
rsync -a --delete \
    --exclude '.DS_Store' --exclude 'default.image.bin' --exclude 'manifest.tsv' --exclude 'extract.log' \
    "$GAME/" "$OUT/game_root/"
du -sh "$OUT"
echo "Copy $OUT to sdmc:/switch/ and launch mgsv_gz.nro with a title override (hold R while starting a game)."
