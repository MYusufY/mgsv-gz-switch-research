# Porting MGSV: Ground Zeroes to the Nintendo Switch

Feasibility assessment and roadmap, based on the actual disc image in this
workspace. Numbers below come from the files in `extracted/`, not from
estimates.

---

## 1. Bottom line

A **source port is impossible**: Fox Engine is proprietary, the source has never
leaked or been licensed, and no decompilation project exists. Nothing in this
workspace changes that.

A **native binary port is possible in principle, and there is now a proven
route**: static recompilation of the Xbox 360 PowerPC executable to C++, built
for ARM64, with the console's Direct3D 9 reimplemented on deko3d.

That route is not speculative any more:

- [ReXGlue](https://github.com/rexglue/rexglue-sdk) turns XEX binaries into
  portable C++ (it is the successor of XenonRecomp / rexdex and reuses Xenia's
  loader, kernel and GPU research).
- [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) provides the Switch platform
  layer (memory, SD filesystem, threads, audio).
- **Silent Hill: Homecoming NX** shipped on 2026-10-30 as an `.nro`: a Konami
  Xbox 360 title statically recompiled to ARM64 with a native deko3d renderer,
  30 fps at 720p handheld. It is the closest existing precedent.

But "possible" is not "small". Ground Zeroes is a 2014 open-world engine with
67,764 functions, a six-thread lock-free job system and runtime shader
compilation. Expect a **multi-month project with substantial performance risk**,
not a weekend build.

Status right now: the executable is extracted, decrypted, analysed, and
successfully translated by ReXGlue (659 generated C++ headers, 345 MB). The
next milestone is booting the recompiled code on a desktop host.

---

## 2. What the disc actually is

| Property | Value |
| --- | --- |
| Format | Xbox 360 XGD3 (XDVDFS game partition + UDF video partition) |
| Image size | 7,838,695,424 bytes (7.30 GiB) |
| Filesystem entries | 945 (781 files) |
| Payload | 1.51 GiB of game files |
| Executable | `default.xex`, 21,610,496 bytes, title ID `4B4E085C`, media ID `085097D1`, built 2014-01-06 |
| Protection | XEX2, AES-128-CBC "normal" encryption, "basic" compression |
| Image | `0x1570000` bytes at `0x82000000`, entry `0x82E12CC0` |

### Assets

| Type | Count | Size | Notes |
| --- | ---: | ---: | --- |
| `.fpk` + `.fpkd` | 139 + 139 | 142 MB | Fox Engine archives; `.fpkd` is the per-archive dictionary |
| `.pftxs` | 63 | 70 MB | streamed textures |
| `.fsm` | 59 | 342 MB | demo / cutscene streams (largest: 123 MB) |
| `.sbp` | 56 | 303 MB | sound banks |
| `.wem` | 55 | 199 MB | Wwise audio |
| `.lua` | 196 | 4.4 MB | gameplay and **shader** definitions |
| `.qar` | 2 | 444 MB | `texture.qar` alone is 443 MB |
| `.fsop` | 4 | 0.3 MB | compiled shader operation tables |
| `.json`, `.rdf`, `.ffnt`, … | 50 | 1 MB | config, fonts, UI |

Asset files are themselves Fox-compressed (magic `0F F5 12 ED`), but that does
not matter for the port: the recompiled engine keeps its own decompression code,
so the files can be shipped to the Switch exactly as they are on the disc.

One important detail: `/shaders/xbox360/*.lua` are **shader sources**, and the
`.fsop` files are shader operation tables. Fox Engine compiles Xenos shaders at
runtime. A port therefore has to translate shaders on the fly (or pre-cache
them), which is the single largest source of stutter and of GPU bugs.

---

## 3. The executable

`tools/xex_extract.py` derives the session key from the security header and
decrypts/decompresses the image; `tools/analyze_image.py` inspects the result.

| Metric | Value |
| --- | ---: |
| `.text` | 18,884,100 bytes (`0x1205604`) |
| Functions (`.pdata` entries) | **67,764** |
| Instruction slots | **4,724,097** |
| Integer | 70.6 % |
| Branch | 18.3 % |
| **VMX128 / AltiVec** | **8.7 %** (409,364) |
| Scalar FP | 1.4 % |
| Padding / undecodable | 1.1 % |
| Kernel imports | 498 (`xboxkrnl.exe` 278, `xam.xex` 220) |
| Static libraries | 21, incl. `D3D9`, `XAudio2`, `XMP`, `XGRAPHICS`, `XBC` |

The instruction mix is good news for translation: 8.7 % vector code is
manageable, and there is no exotic self-modifying code. The bad news is scale —
67k functions is 3–5× a typical 2008-era UE3 title, and the vector code is
Xenon's **VMX128** dialect (128 registers, non-standard encodings), not plain
AltiVec.

### Recompilation result

ReXGlue v0.10.0, `codegen` on the decrypted XEX:

- analysis took 50 s and reported **6 unresolved branch targets** at only three
  addresses (declared in `port/overrides.toml`);
- code generation took 56 s and emitted **659 headers / 345 MB** of C++;
- remaining warnings: 3 unresolved conditional branches into `.pdata` gaps
  (declared in `overrides.toml`), ~21 unsupported `float16_4` pack instructions
  in one VMX128 routine, and 3 functions whose *generated C++* exceeds the 1 MB
  per-file cap (an 88 KB guest function becomes ~1.3 MB of C++; not an error).

That is a remarkably clean translation for a binary this size.

---

## 4. Platform gap

| Subsystem | Xbox 360 | Switch (handheld) | Work required |
| --- | --- | --- | --- |
| CPU | 3 × PowerPC @ 3.2 GHz, 6 threads, VMX128, in-order | 4 × Cortex-A57 @ 1.02 GHz (3 usable), ARM64 + NEON | ReXGlue translates PPC → C++; thread/fiber model from the runtime |
| GPU | Xenos @ 500 MHz, 48 shaders, 10 MB eDRAM, D3D9-class + memexport | Maxwell, 256 CUDA cores @ 307–768 MHz, unified 4 GB LPDDR4 | **D3D9 → deko3d** (Renut / plume), Xenos shader → GLSL → deko3d (XenosRecomp + uam) |
| RAM | 512 MB unified + 10 MB eDRAM | 4 GB shared (~3.2 GB for apps) | Runtime large-address-space support; guest layout assumptions |
| Audio | XAudio2 + XMA hardware decoder | audren, software decode | FFmpeg XMA decode → audren |
| Input | XInput | HID | Mapping layer, rumble |
| Storage | DVD (slow seeks) | microSD | `game://` → `sdmc:/switch/.../game_root` |
| OS services | `xboxkrnl.exe` + `xam.xex` | libnx / Atmosphère | 498 imports: many trivial, some significant (XAM, XMP, XNotify) |

### Why this game is harder than the existing ports

1. **Threads.** Fox Engine runs a job system across all six hardware threads
   with lock-free queues and spin-waits. The Switch gives three cores. Any
   spinning or priority assumption that worked on Xenon can turn into a
   deadlock or a 3× slowdown.
2. **Scale.** 67,764 functions and 4.72 M instructions means far more code to
   validate, and far more chances for one mistranslated instruction to corrupt
   state hours into the game.
3. **Runtime shaders.** The engine compiles shaders from Lua at runtime, so the
   port cannot ship a fixed shader set. It needs a translation cache
   (`deko3d_shaders.bin`-style) or players will see multi-second hitches on
   every new effect.
4. **Renderer complexity.** Deferred rendering, many render targets, heavy
   post-processing and Xenos-specific paths have to survive translation to a
   tile-based mobile GPU with a fraction of the eDRAM bandwidth pattern.
5. **CPU headroom.** The 360 build targets 30 fps while using six 3.2 GHz
   threads. Three 1.02 GHz A57s are roughly an order of magnitude short on paper;
   only the fact that much of that CPU time is VMX/GPU-adjacent work makes this
   plausible at all. This is the biggest unknown.

---

## 5. Routes considered

| Route | Verdict |
| --- | --- |
| Port Fox Engine source | Impossible — no source exists publicly. |
| **Static recompilation → ARM64 + deko3d** | **Chosen.** Only route that yields a real `.nro` running the original game code. |
| Run a 360 emulator on Switch | No viable Xenon emulator exists for ARM64 Switch; JIT translation of 3.2 GHz PPC on 3 A57s is far out of reach. |
| Stream from a PC (Moonlight/Sunshine) | Works today and needs no porting, but is not native. Useful as a stopgap. |
| Recreate the game on another engine from assets | Years of work, legally worse, and would not be the same game. |

---

## 6. Roadmap

Phase 0 is done; Phase 1 is where the workspace currently is.

| Phase | Goal | Exit criterion |
| --- | --- | --- |
| **0. Extraction & analysis** ✅ | Disc, XEX, image, tooling, codegen | ReXGlue accepts the XEX and emits C++ |
| **1. Host boot** | Build and run the recompiled game on macOS/Windows | Reaches `main`, initialises the engine, gets past asset loading |
| **2. Kernel & XAM** | Fill 498 imports; file I/O on `game://`; threads | Engine reaches its render loop without unimplemented-call aborts |
| **3. Renderer** | D3D9 device/state/textures/shaders on deko3d | First frame of the main menu |
| **4. Audio & input** | XMA → audren, XInput → HID | Menu music, controllable cursor |
| **5. Gameplay** | Level load, streaming, save data | Playable prologue |
| **6. Performance** | Codegen flags (`cr_as_local`, `dform_disp_split`, …), shader cache, resolution scaling | 30 fps at 720p handheld |
| **7. Packaging** | `.nro`, title-mode memory, logs, config | Installable homebrew release |

Phases 1–3 are the make-or-break part. Phase 6 is where most of the calendar
time will go if the game boots.

### Immediate next steps

1. Finish the host build (`cmake --build out/build/mac-arm64`) and run it.
2. Fix the three oversized "functions" (they are jump tables inside `.text`).
3. Add the three unresolved conditional branch targets to `overrides.toml`.
4. Trace the first 100 kernel imports the engine actually calls and stub them.
5. Stand up the deko3d backend from the nfsmw-nx / Renut lineage.

---

## 7. Legal

- Reverse engineering a disc you own for personal interoperability is the
  usual basis for this work; distribution is a different matter.
- **Do not distribute** the ISO, the extracted assets, or the generated C++
  (`port/generated/`). The generated code is a derivative of Konami's binary.
- A release must ship only original code — runtime patches, renderer, glue,
  packaging — and require the user to supply their own Xbox 360 disc.
- ReXGlue is BSD-3-Clause; nfsmw-nx and the Silent Hill port are GPL-3.0. Code
  taken from them carries those obligations.
