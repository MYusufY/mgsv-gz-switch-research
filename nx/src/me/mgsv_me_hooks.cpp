// Metal Gear Solid V: Ground Zeroes hooks for the native renderer (src/me: sa2-nx's renderer, from masseffect-nx
// through banjo-nx, as used by sc5p2-nx). Same role as sc5p2-nx's sc5p2_me_hooks.cpp, with Ground Zeroes' addresses.
//
// How they were found (2026-11-11), without a reference game to match against (sa2-nx's tools/d3d_match.py needs one):
//   - The XDK Direct3D block is 0x82E0xxxx-0x82E4xxxx: VdSwap (sub_82E28A30), VdInitializeRingBuffer (sub_82E309A0)
//     and VdRetrainEDRAM (sub_82E3D7D8) are called from there.
//   - Whole-program call census on the PC build (nx/tools/d3d_census.py) at the install-prompt menu, 60 swaps/s:
//     sub_82E41230 runs 59 times a swap = the 59 resolves a frame the emulated GPU reported (D3DDevice_Resolve; its
//     own draw is sub_82E40FD0). sub_82E2BA38 (56 a swap) flushes the device's dirty state and writes a DRAW_INDX
//     packet: the main draw.
//   - Draw-time shader load: sub_82E441F8 (r3 = device) reads the bound vertex shader at device+13072 and the pixel
//     shader at device+13068 - the same offsets as SA2's XDK (me_native_system.h: "SA2, 2012: +13072 / +13068").
//     Its callers are the draw entry points: sub_82E2B578, sub_82E2BA38, sub_82E2BE50, sub_82E2C2C8, sub_82E2C768,
//     sub_82E340F0, sub_82E34990 (game draws, called from Fox Engine's renderer at 0x832F2F50-0x832F4CA0 and from
//     sub_82E27EC0), and sub_82E40FD0 (only inside the resolve: Direct3D's own shaders, so no draw record).
//     None of them calls another, so one draw makes one record.
//   - Shader containers (nx/tools/d3d_container_probe.py): sub_82E26C30 receives every container in r3 - Fox Engine
//     calls it (from sub_832FA550) for each shader it creates, ~1200 at boot, as do Direct3D's own CreateVertexShader
//     (sub_82E27930) / CreatePixelShader (sub_82E27708). It reads the header and sizes the shader object (872 bytes for
//     a vertex shader, 40 for a pixel shader). Containers start with 0x102A11xx; flags bit 0 = vertex shader
//     (XenosRecomp shader_recompiler.cpp).
//
// Every hook calls the recompiled original unchanged; no PPC register is touched.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "me/me_native_system.h"

REXCVAR_DEFINE_BOOL(mgsv_dump_shaders, false, "MGSV",
                    "Write every shader container the game creates to shaders/ next to the executable "
                    "(vs_<hash>.bin / ps_<hash>.bin), for the shader package");
REXCVAR_DEFINE_STRING(mgsv_dump_shaders_dir, "", "MGSV",
                      "mgsv_dump_shaders: folder to write to instead of shaders/ next to the executable");

namespace {

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

constexpr uint32_t kDeviceVertexShader = 13072;  // 0x3310
constexpr uint32_t kDevicePixelShader = 13068;   // 0x330C

const bool g_device_layout_set = [] {
  me::native::SetDeviceShaderOffsets(kDeviceVertexShader, kDevicePixelShader);
  return true;
}();

inline bool PlausibleDevice(uint32_t device) { return device >= 0x40000000 && device < 0x7F000000; }

std::mutex g_dump_mutex;
std::unordered_set<uint64_t> g_dumped;

void DumpShaderContainer(const uint8_t* base, uint32_t container) {
  if (!REXCVAR_GET(mgsv_dump_shaders) || !container) {
    return;
  }
  const uint32_t flags = Load32(base, container);
  const uint32_t header = Load32(base, container + 4);
  const uint32_t ucode = Load32(base, container + 8);
  if ((flags & 0xFFFF0000u) != 0x102A0000u || header < 12 || header > (1u << 20) || !ucode || ucode > (1u << 20)) {
    REXLOG_WARN("[shaders] implausible container at {:08X} (flags {:08X}, header {}, microcode {}), skipped",
                container, flags, header, ucode);
    return;
  }
  const bool vertex = (flags & 1) != 0;
  const uint8_t* blob = base + container;
  const size_t size = size_t(header) + ucode;
  const uint64_t hash = XXH3_64bits(blob, size);
  {
    std::lock_guard lock(g_dump_mutex);
    if (!g_dumped.insert(hash).second) {
      return;
    }
  }
  const std::string dir = REXCVAR_GET(mgsv_dump_shaders_dir);
  const auto folder = dir.empty() ? rex::filesystem::GetExecutableFolder() / "shaders" : std::filesystem::path(dir);
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const auto path = folder / fmt::format("{}_{:016x}.bin", vertex ? "vs" : "ps", hash);
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(blob), std::streamsize(size));
}

inline void NoteDraw(const uint8_t* base, const PPCContext& ctx) {
  me::native::NoteDrawCall(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}

}  // namespace

// Draw-time shader load (the shader part of Direct3D's state flush, called from every draw entry point).
REX_EXTERN(__imp__sub_82E441F8);
REX_HOOK_RAW(sub_82E441F8) {
  const uint32_t device = ctx.r3.u32;
  if (PlausibleDevice(device)) {
    const uint32_t vs = Load32(base, device + kDeviceVertexShader), ps = Load32(base, device + kDevicePixelShader);
    if (!me::native::ShaderObjectsFinished(vs, ps)) {
      me::native::NoteShaderObject(base, vs, true);
      me::native::NoteShaderObject(base, ps, false);
    }
  }
  __imp__sub_82E441F8(ctx, base);
}

// Draw entry points (r3 = device): one record per call, read before the original writes its packets.
#define MGSV_DRAW_HOOK(ADDR)            \
  REX_EXTERN(__imp__sub_##ADDR);        \
  REX_HOOK_RAW(sub_##ADDR) {            \
    NoteDraw(base, ctx);                \
    __imp__sub_##ADDR(ctx, base);       \
  }

MGSV_DRAW_HOOK(82E2B578)
MGSV_DRAW_HOOK(82E2BA38)
MGSV_DRAW_HOOK(82E2BE50)
MGSV_DRAW_HOOK(82E2C2C8)
MGSV_DRAW_HOOK(82E2C768)
MGSV_DRAW_HOOK(82E340F0)
MGSV_DRAW_HOOK(82E34990)

// Every shader container the game (and Direct3D) creates passes through here first (r3 = container).
REX_EXTERN(__imp__sub_82E26C30);
REX_HOOK_RAW(sub_82E26C30) {
  DumpShaderContainer(base, ctx.r3.u32);
  __imp__sub_82E26C30(ctx, base);
}
