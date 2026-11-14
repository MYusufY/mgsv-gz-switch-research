#!/bin/sh
# Host build of the recompiled game.
#
#   REXSDK_PREFIX=/path/to/rexglue/mac-arm64 ./build.sh
#
# Output: out/build/mac-arm64/mgsv_gz
# Log:    out/build/mac-arm64/build.log   (tail -f it to watch progress)
set -e
cd "$(dirname "$0")"

BUILD=out/build/mac-arm64
JOBS=${JOBS:-10}

cmake -S . -B "$BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    ${REXSDK_PREFIX:+-DCMAKE_PREFIX_PATH="$REXSDK_PREFIX"}

cmake --build "$BUILD" -j "$JOBS" 2>&1 | tee "$BUILD/build.log"
