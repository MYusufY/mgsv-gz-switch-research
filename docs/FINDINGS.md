# Findings

What broke the native renderer on Fox Engine, how each cause was found, and what is still open. Written for
anyone continuing this or porting another Xbox 360 game with a similar renderer.

## Fixed

1. **Pixel position in shaders without interpolators (XenosRecomp).** A pixel shader with no declared
   interpolators had every register `rN` initialised from `iTexCoordN`, ignoring the container's vPos register
   (shader header `fieldC`: bit 1 = uses vPos, bits 8–15 = register). Fox pairs Direct3D's position-only
   screen-quad vertex shader with pixel shaders that read the pixel position from `r0`. Its depth-restore pass
   therefore sampled texel (0,0) and wrote the far plane everywhere, no z-fail stencil light volume ever marked a
   pixel, and all deferred lighting was black.
2. **Colour-grading LUT read back as a 3D texture.** Fox renders its grading LUT into a 32×512 target, resolves
   it, and samples that memory as a tiled 16×16×16 volume. The native renderer keeps resolves in host images, so
   the volume was empty and grading blacked the frame. Fixed by rebuilding the volume on the GPU (3D tiled offset →
   analytic inverse of the 2D tiling: `mgsv_resolved_to_volume.comp`).
3. **Gamma render targets (`k_8_8_8_8_GAMMA`) stored linear.** Fox reads these back raw (per-light colour tiles,
   half-resolution diffuse accumulation), so cutscenes were about 2× too dark. Fixed with an sRGB attachment view
   for gamma passes (`ImageNative::view_srgb`).
4. **Direct3D vertex-shader variants.** D3D rewrites a vertex shader's fetch instructions per vertex declaration
   (format, stride, full/mini fetch, destination register). The game-side dump only has templates, so ~190
   draws per frame were skipped and some produced stretched triangles. Fix: dump the microcode the ring actually
   loads (`MGSV_RING_DUMP_DIR`) and build library entries with `nx/tools/repair_vs_variants.py` (template header,
   element table remapped by destination register, ring microcode).
5. **Junk "reconstructed" shaders.** Containers rebuilt from shader objects (`MGSV_UNMATCHED_DIR`) were often read
   after their microcode memory was reused; 56 of them had control flow pointing outside their code. The library
   now packs only containers whose control flow is valid.
6. **Partial last vertex.** A vertex stream may end partway through its last vertex's stride; rounding down to
   whole vertices dropped it, and its triangles collapsed to the origin.
7. **Depth sampled as `k_8_8_8_8`.** Fox reads its depth resolve as colour bytes; converted on the GPU
   (`mgsv_depth_resolved_to_rgba8.comp`).
8. **Smaller items:** shader objects below 0x40000000 were rejected; `getWeights2D` overload for DXC;
   `k_16*_EXPAND` texture formats; blend op "reverse subtract"; vertex fetches from buffers in game shaders
   (`banjoVfetch`, pixel-shader buffers in slots 2–3).

## Open

- Some shots contain large flat polygons (cause not found; not exp_adjust, not indexed fetches).
- Lens-flare streaks show up as banded blue stripes.
- Colour noise on some surfaces. On macOS, MoltenVK limits the 2D descriptor heap to 1024 entries, and shrinking
  the heap further makes the noise much worse, so part of it is Mac-specific; the Switch gets 4096 entries but
  still shows glitches.
- Two shaders read the pixel position from `r1` while their header says `r0`: the real param-gen register is
  likely set per draw (`SQ_CONTEXT_MISC`), which the translator does not model.
- Performance: ~2 fps on the Switch; one pass group (Direct3D clear shaders) took ~45 % of GPU time.

## Diagnostics left in the code (environment variables, off by default)

- `MGSV_PRESENT_ADDRESS` / `MGSV_PRESENT_SIZE` / `MGSV_PRESENT_FORMAT` — present a resolved texture instead of the
  front buffer (the view swaps R and B for 8888 targets).
- `MGSV_PROTECT=<addr>:<pitch>`, `MGSV_PROTECT_NTH=<addr>:<n>` — keep a resolve so an aliased target can be viewed.
- `MGSV_TRACE_PS_CONSTANTS=1` with `SA2_TRACE_ALL_DRAWS=1 --banjo_trace_frame_s=<s>` — full frame trace with
  constants, fetch words, stencil and viewport state, and render-pass bindings.
- `MGSV_RING_DUMP_DIR`, `MGSV_UNMATCHED_DIR` — dump unidentified shader microcode / reconstructed containers.
- `MGSV_GAMMA_LINEAR=1` — old gamma-target behaviour, for comparison.
- `SA2_SHADER_LIBRARY=<file>` — load another shader package (for single-shader debug variants).
