#!/usr/bin/env python3
"""Probe build: finds the Direct3D functions that receive Xbox 360 shader containers (CreateVertexShader /
CreatePixelShader and friends) by looking at their pointer arguments at run time.

  python3 nx/tools/d3d_container_probe.py [--lo 0x82E00000] [--hi 0x82E50000]
      -> nx/out/census/mgsv_d3d_container_probe.cpp

Build the PC test build with -DMGSV_EXTRA_SOURCES=<that file>, run it with MGSV_PROBE_FILE=<path>. For the first 2000
calls of every function in the range, r3 and r4 are checked: a committed, word-aligned guest address whose words read
like a shader container header (ShaderContainer in XenosRecomp/shader.h: flags, header size, microcode size, ...,
constant table / definition table / shader offsets inside the header). Every 5 s the functions with hits are written
with the argument, hit count, the most recent address and its first words.
"""
import argparse
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(ROOT, 'nx', 'generated', 'default')
OUT = os.path.join(ROOT, 'nx', 'out', 'census')
DEF = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{$', re.M)

BODY = r'''
namespace {
std::mutex g_page_mutex;
std::unordered_map<uint32_t, bool> g_page_ok;  // 64 KB guest pages: readable?

bool Readable(const uint8_t* base, uint32_t address) {
  const uint32_t page = address >> 16;
  std::lock_guard lock(g_page_mutex);
  auto it = g_page_ok.find(page);
  if (it != g_page_ok.end()) return it->second;
  size_t length = 0x10000;
  rex::memory::PageAccess access = rex::memory::PageAccess::kNoAccess;
  const bool ok = rex::memory::QueryProtect(const_cast<uint8_t*>(base) + (size_t(page) << 16), length, access) &&
                  access != rex::memory::PageAccess::kNoAccess && length >= 0x10000;
  g_page_ok.emplace(page, ok);
  return ok;
}

uint32_t Word(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

struct Hit {
  std::atomic<uint64_t> count{0};
  std::atomic<uint32_t> last{0};
};
Hit g_hits[kCount][2];

bool LooksLikeContainer(const uint8_t* base, uint32_t a) {
  if ((a & 3) || a < 0x40000000u || a >= 0xC0000000u) return false;
  if (!Readable(base, a) || !Readable(base, a + 0x24)) return false;
  const uint32_t header = Word(base, a + 4), microcode = Word(base, a + 8);
  const uint32_t constants = Word(base, a + 16), definitions = Word(base, a + 20), shader = Word(base, a + 24);
  if (header < 0x24 || header > 0x100000 || microcode < 4 || microcode > 0x100000 || (microcode & 3)) return false;
  if (constants > header || definitions > header || shader > header || shader < 0x24) return false;
  return true;
}

void Probe(unsigned i, const uint8_t* base, uint32_t r3, uint32_t r4) {
  if (LooksLikeContainer(base, r3)) {
    g_hits[i][0].count.fetch_add(1, std::memory_order_relaxed);
    g_hits[i][0].last.store(r3, std::memory_order_relaxed);
  }
  if (LooksLikeContainer(base, r4)) {
    g_hits[i][1].count.fetch_add(1, std::memory_order_relaxed);
    g_hits[i][1].last.store(r4, std::memory_order_relaxed);
  }
}

const uint8_t* g_base = nullptr;

struct ProbeReporter {
  ProbeReporter() {
    const char* path = std::getenv("MGSV_PROBE_FILE");
    if (!path || !*path) return;
    std::thread([path] {
      for (unsigned interval = 0;; ++interval) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        FILE* f = std::fopen(path, "a");
        if (!f) continue;
        std::fprintf(f, "==== interval %u\n", interval);
        for (unsigned i = 0; i < kCount; ++i) {
          for (int arg = 0; arg < 2; ++arg) {
            const uint64_t n = g_hits[i][arg].count.load(std::memory_order_relaxed);
            if (!n) continue;
            const uint32_t a = g_hits[i][arg].last.load(std::memory_order_relaxed);
            std::fprintf(f, "sub_%08X r%d hits %llu calls %llu last %08X", kAddr[i], arg + 3, (unsigned long long)n,
                         (unsigned long long)g_calls[i].load(std::memory_order_relaxed), a);
            if (g_base) {
              std::fprintf(f, " words");
              for (uint32_t w = 0; w < 9; ++w) std::fprintf(f, " %08X", Word(g_base, a + 4 * w));
            }
            std::fprintf(f, "\n");
          }
        }
        std::fclose(f);
      }
    }).detach();
  }
} g_probe_reporter;
}  // namespace
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--lo', type=lambda s: int(s, 0), default=0x82E00000)
    ap.add_argument('--hi', type=lambda s: int(s, 0), default=0x82E50000)
    a = ap.parse_args()
    funcs = set()
    for path in glob.glob(os.path.join(GEN, '*_recomp.*.cpp')):
        for m in DEF.finditer(open(path, errors='replace').read()):
            addr = int(m.group(1), 16)
            if a.lo <= addr < a.hi:
                funcs.add(addr)
    funcs = sorted(funcs)
    os.makedirs(OUT, exist_ok=True)
    out = os.path.join(OUT, 'mgsv_d3d_container_probe.cpp')
    with open(out, 'w') as f:
        f.write('// Generated by nx/tools/d3d_container_probe.py - shader container probe (PC test build only).\n')
        f.write('#include <atomic>\n#include <chrono>\n#include <cstdint>\n#include <cstdio>\n#include <cstdlib>\n')
        f.write('#include <cstring>\n#include <mutex>\n#include <thread>\n#include <unordered_map>\n\n')
        f.write('#include <rex/hook.h>\n#include <rex/memory/utils.h>\n#include <rex/ppc.h>\n\n')
        f.write(f'static constexpr unsigned kCount = {len(funcs)};\n')
        f.write('static constexpr uint32_t kAddr[kCount] = {\n')
        for i in range(0, len(funcs), 8):
            f.write('  ' + ', '.join(f'0x{x:08X}u' for x in funcs[i:i + 8]) + ',\n')
        f.write('};\nstatic std::atomic<uint64_t> g_calls[kCount];\n')
        f.write(BODY)
        for i, addr in enumerate(funcs):
            f.write(f'REX_EXTERN(__imp__sub_{addr:08X});\n')
            f.write(f'REX_HOOK_RAW(sub_{addr:08X}) {{ if (g_calls[{i}].fetch_add(1, std::memory_order_relaxed) < 2000) '
                    f'{{ g_base = base; Probe({i}, base, ctx.r3.u32, ctx.r4.u32); }} __imp__sub_{addr:08X}(ctx, base); }}\n')
    print(f'{len(funcs)} functions in {a.lo:08X}..{a.hi:08X} -> {out}')


if __name__ == '__main__':
    main()
