#!/usr/bin/env bash
# Launch the recompiled build against the extracted game root and capture a log.
#
#   ./run.sh              # normal run, xenos (Vulkan/MoltenVK) GPU plugin
#   ./run.sh --gpu_plugin=   # runtime only, no GPU emulation
#
# Extra arguments are passed through to the executable.
set -u

cd "$(dirname "$0")"

BIN=${BIN:-out/build/mac-arm64/mgsv_gz}
GAME_ROOT=${GAME_ROOT:-../extracted}
LOG=${LOG:-out/run.log}
# Durable copy of the SDK install prefix (the original lived in a temp dir that macOS purges).
REXSDK_PREFIX=${REXSDK_PREFIX:-$(cd .. && pwd)/sdk/prefix-mac-arm64}

if [ ! -x "$BIN" ]; then
    echo "error: $BIN not built (run ./build.sh)" >&2
    exit 1
fi

mkdir -p "$(dirname "$LOG")"

# Make the SDK dylibs (librexgpu-xenos, MoltenVK) findable without install_name_tool.
export DYLD_LIBRARY_PATH="$REXSDK_PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

# The runtime loads the GPU plugin as "rexgpu-<name>" from the executable's own
# directory, so symlink it next to the binary.
for lib in librexgpu-xenos.dylib libMoltenVK.dylib; do
    if [ ! -e "$(dirname "$BIN")/$lib" ]; then
        ln -sf "$REXSDK_PREFIX/lib/$lib" "$(dirname "$BIN")/$lib"
    fi
done

ARGS=(
    "--game_data_root=$GAME_ROOT"
    --gpu_plugin=xenos
    --mnk_mode=true
    --log_level=debug
)
ARGS+=("$@")

echo "binary : $BIN"
echo "game   : $GAME_ROOT"
echo "log    : $LOG"
echo

exec "$BIN" "${ARGS[@]}" 2>&1 | tee "$LOG"
