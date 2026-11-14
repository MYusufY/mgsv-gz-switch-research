#!/bin/bash
# Builds the Switch NVK driver (mesa-switch @1a8c1a66d6f + nfsmw-nx's mesa-switch-nfsmw.patch) on macOS / Apple Silicon.
# macOS port of third_party/nfsmw-nx/mesa/linux/build.sh (same Meson options, but Vulkan only: the OpenGL/gallium
# half needs devkitPro's libdrm_nouveau headers and ReXGlue never uses it), using Homebrew's LLVM 23 +
# SPIRV-LLVM-Translator 23 for the host tools, rustup's stable Rust, and devkitPro in /opt/devkitpro.
#
#   nx/tools/build_nvk_macos.sh            -> third_party/mesa-switch/mesa-macos-install/opt/devkitpro/portlibs/switch
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MESA=${MESA:-$ROOT/third_party/mesa-switch}
LINUXDIR=$ROOT/third_party/nfsmw-nx/mesa/linux
MESON=${MESON:-$ROOT/third_party/mesa-venv/bin/meson}
BREW=$(brew --prefix)
LLVM=$BREW/opt/llvm
INSTALL=$MESA/mesa-macos-install
SDK=$INSTALL/opt/devkitpro/portlibs/switch

export PATH="$BREW/opt/bison/bin:$BREW/opt/rustup/bin:$HOME/.cargo/bin:$ROOT/third_party/mesa-venv/bin:$PATH"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu
export MESA_SWITCH_RUSTC=${MESA_SWITCH_RUSTC:-$(command -v rustc)}
export MESA_SWITCH_BINDGEN=${MESA_SWITCH_BINDGEN:-$(command -v bindgen)}
# python.org's Python has no CA bundle until "Install Certificates.command" runs; Meson downloads Mesa's Rust crates
# (subprojects/*.wrap) with it, so point it at certifi's bundle from the venv.
export SSL_CERT_FILE=${SSL_CERT_FILE:-$("$ROOT/third_party/mesa-venv/bin/python" -m certifi)}
export LIBCLANG_PATH=$LLVM/lib              # bindgen loads Homebrew's libclang ...
export SOURCE_DATE_EPOCH=$(git -C "$MESA" show -s --format=%ct HEAD)
export ZERO_AR_DATE=1 LC_ALL=C TZ=UTC
cd "$MESA"

if [ ! -x builddir-native-host/src/compiler/clc/mesa_clc ] || [ ! -x builddir-native-host/src/compiler/spirv/vtn_bindgen2 ]; then
    echo "== host tools (LLVM $("$LLVM/bin/llvm-config" --version))"
    printf "[binaries]\nllvm-config = '%s'\n" "$LLVM/bin/llvm-config" > macos-host-native.ini
    PKG_CONFIG_PATH="$BREW/opt/spirv-llvm-translator/lib/pkgconfig:$BREW/opt/spirv-tools/lib/pkgconfig:$BREW/lib/pkgconfig" \
    "$MESON" setup builddir-native-host --wipe --native-file macos-host-native.ini \
        --buildtype=release \
        -Dvulkan-drivers= -Dgallium-drivers= -Dshader-cache=disabled -Dplatforms= \
        -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
        -Dtools=[] -Dllvm=enabled -Dshared-llvm=enabled \
        -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dinstall-mesa-clc=true
    ninja -C builddir-native-host src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2
fi

echo "== driver for the Switch"
# ... and the clang whose resource dir the bindgen wrapper passes must be the same LLVM.
export PATH="$LLVM/bin:$PATH"
# src/nouveau/vulkan/nvk_archive_merge.py merges the static libraries with `ar -M` (an MRI script), a GNU ar feature
# Apple's ar lacks: put devkitA64's GNU ar first on PATH as `ar`.
mkdir -p "$MESA/macos-gnu-ar"
ln -sf "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar" "$MESA/macos-gnu-ar/ar"
export PATH="$MESA/macos-gnu-ar:$PATH"
# nfsmw-nx's cross file, plus Mesa's own nouveau.h / nv_device_info.h folders on the include path: the Linux/Docker
# builds copy those two headers into /opt/devkitpro/portlibs/switch/include (build-switch.sh), which is root-owned
# here. @DIRNAME@ is resolved to the Linux folder because the copy lives elsewhere.
CROSS=$MESA/switch_cross_file_macos.txt
EXTRA_INC="'-I$MESA/src/gallium/winsys/nouveau/drm', '-I$MESA/src/nouveau/headers', '-I$MESA/include/drm-uapi', "
sed -e "s|@DIRNAME@|$LINUXDIR|g" \
    -e "s|'-I/opt/devkitpro/portlibs/switch/include', |&$EXTRA_INC|" \
    "$LINUXDIR/switch_cross_file.txt" > "$CROSS"
grep -q "winsys/nouveau/drm" "$CROSS" || { echo "cross file include patch did not apply" >&2; exit 1; }
setup_mode=()
[ -f builddir-switch-macos/meson-private/coredata.dat ] && setup_mode+=(--wipe)
"$MESON" setup builddir-switch-macos ${setup_mode[@]+"${setup_mode[@]}"} \
    --cross-file "$CROSS" \
    --native-file "$LINUXDIR/switch_native_tools.txt" \
    --default-library=static --prefix=/opt/devkitpro/portlibs/switch --libdir=lib \
    --buildtype=release -Doptimization=2 -Db_lto=false -Db_ndebug=true \
    -Dvulkan-drivers=nouveau -Dgallium-drivers= -Dgallium-rusticl=false \
    -Dplatforms=switch -Dglx=disabled -Degl=disabled \
    -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dvideo-codecs= \
    -Dshader-cache=enabled -Dxmlconfig=enabled -Dexpat=enabled -Dtools=[] \
    -Dllvm=disabled -Dshared-glapi=disabled -Dshared-llvm=disabled \
    -Dmesa-clc=system -Dprecomp-compiler=system -Dcpp_rtti=false -Dbuild-tests=false
ninja -C builddir-switch-macos

echo "== install"
rm -rf -- "$INSTALL"
"$MESON" install -C builddir-switch-macos --destdir "$INSTALL" > /dev/null
for pc in "$SDK"/lib/pkgconfig/*.pc; do
    sed -i '' 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$pc"
done
[ -f "$SDK/lib/libvulkan.a" ] || { echo "no lib/libvulkan.a in $SDK" >&2; exit 1; }
echo "SDK for -DREXGLUE_SWITCH_NVK_SDK:"
echo "  $SDK"
