# Metal Gear Solid V: Ground Zeroes (Xbox 360) - static recompilation host application, macOS test build and
# Nintendo Switch NRO.
#
# Built on the ReXGlue v0.10.0 fork with the Horizon (Switch) layer from sa2-nx (third_party/sa2-nx/sdk; its Switch
# layer comes from nfsmw-nx). The game code is not in this repository: generate it from your own default.xex first:
#   REX_MAX_JUMP_TABLE_ENTRIES=1024 ../third_party/sa2-nx/sdk/out/mac-arm64/rexglue codegen mgsv_gz_manifest.toml
#
# Stage 1 (this file): the SDK's emulated Xenos GPU (gpu_plugin = xenos) running on NVK, to bring the game up on the
# console. A native renderer (sa2-nx's src/me or Renut on deko3d) replaces it later for speed.
#
# Nintendo Switch:
#   cmake -S nx -B nx/out/nx -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=third_party/sa2-nx/tools/switch/cmake/switch-devkitA64.cmake \
#     -DREXSDK_DIR=$PWD/third_party/sa2-nx/sdk \
#     -DREXGLUE_SWITCH_NVK_SDK=$PWD/third_party/mesa-switch/mesa-macos-install/opt/devkitpro/portlibs/switch
#   cmake --build nx/out/nx --target mgsv_gz            -> nx/out/nx/mgsv_gz.nro

cmake_minimum_required(VERSION 3.25)
project(mgsv_gz LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sa2-nx/sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    message(FATAL_ERROR "nx/generated/rexglue.cmake is missing: run the code generator first")
endif()
include(generated/rexglue.cmake)
# The SDK's PC helpers copy MoltenVK_icd.json from ${REXGLUE_ROOT}/cmake, a variable only set inside the SDK's own
# directory scope when it is built from source: give it to this scope too.
if(NOT REXGLUE_ROOT)
    set(REXGLUE_ROOT "${REXSDK_DIR}")
endif()

set(MGSV_SOURCES
    src/main.cpp
    # Scripted controller for test runs and test-run video recordings (sc5p2-nx's, off by default).
    src/sc5p2_test_input.cpp
    src/sc5p2_capture.cpp
    # The native renderer (sa2-nx's src/me: masseffect-nx's PM4-ring Vulkan renderer as adapted by banjo-nx, sa2-nx
    # and sc5p2-nx), with Ground Zeroes' hooks: sa2_renderer_native = true replaces the emulated Xenos GPU.
    src/me/me_native_system.cpp
    src/me/me_ring_wait.cpp
    src/me/me_renderdoc.cpp
    src/me/native/sa2_native_draws.cpp
    src/me/native/sa2_deferred_recording.cpp
    src/me/native/sa2_native_targets.cpp
    src/me/native/sa2_native_shaders.cpp
    src/me/native/sa2_native_textures_pool.cpp
    src/me/native/sa2_shader_library.cpp
    src/me/native/sa2_native_glue.cpp
    src/me/sa2_render_mode.cpp
    src/me/mgsv_me_hooks.cpp
    src/me/sc5p2_api_stub.cpp
    # sa2-nx's switchable enhancements (keys the renderer reads) and the start-up self-check of the AArch64-only forms.
    src/cpu/sa2_enhancements.cpp
    src/cpu/sa2_forms_selfcheck.cpp
    src/cpu/sa2_gpu_boost.cpp
    src/cpu/sa2_codegen_checks.cpp
)
if(NOT REXGLUE_PLATFORM_SWITCH)
    # Switch profiler counters the native renderer calls (no-ops elsewhere) and the PC sampling profiler.
    list(APPEND MGSV_SOURCES src/me/me_pc_stubs.cpp src/profiler/sa2_cpu_profile_pc.cpp
                             src/profiler/sa2_thread_cycles_pc.cpp)
endif()
set_source_files_properties(src/cpu/sa2_enhancements.cpp PROPERTIES
    COMPILE_DEFINITIONS "SA2_BUILD_FLAVOR=\"optimised codegen (step A)\"")

add_executable(mgsv_gz ${MGSV_SOURCES})

# Diagnostic builds: extra sources outside the tree (for example the Direct3D census hooks written by
# nx/tools/d3d_census.py). Empty in normal builds.
set(MGSV_EXTRA_SOURCES "" CACHE STRING "Extra source files compiled into the app (diagnostic builds)")
if(MGSV_EXTRA_SOURCES)
    target_sources(mgsv_gz PRIVATE ${MGSV_EXTRA_SOURCES})
endif()

# The emulated Xenos GPU (ReXGlue's Vulkan backend, on NVK on the Switch).
rexglue_setup_target(mgsv_gz GPU_PLUGINS xenos)

if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(mgsv_gz PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# The renderer's fast draw path calls into the patched NVK through weak references (nvk_switch_*) that are null
# without it; Apple's linker needs to be told they may stay undefined.
if(APPLE)
    target_link_options(mgsv_gz PRIVATE "LINKER:-U,_nvk_switch_draw" "LINKER:-U,_nvk_switch_dibujo"
                                        "LINKER:-U,_nvk_switch_set4" "LINKER:-U,_vk_switch_preload_pipeline")
endif()

# SPIR-V initializers of the native renderer's EDRAM transfer / utility shaders (src/me/native/shaders), compiled on
# the host with glslangValidator (SPIR-V is platform independent: the Switch build uses the same host tool).
find_program(_mgsv_host_glslang glslangValidator REQUIRED)
set(SA2_GLSLANG_VALIDATOR "${_mgsv_host_glslang}" CACHE FILEPATH "Host glslangValidator")
set(SA2_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
set(SA2_EDRAM_SHADER_INCLUDES)
foreach(_shader sa2_edram_7e3_to_rgba8 sa2_edram_rgba8_to_7e3 sa2_edram_16f_to_16f me_resolve_exp_bias me_edram_depth_to_rgba8 me_edram_depth_to_16f me_edram_import me_edram_color_to_depth me_edram_depth_to_depth me_edram_depth_to_stencil me_edram_color_to_stencil me_edram_depth_to_depth_msaa2 me_edram_depth_msaa2_to_depth_1x me_edram_depth_to_stencil_msaa2 me_edram_depth_msaa2_to_stencil_1x me_edram_raw64_to_rgba8 me_edram_rgba8_to_raw64 me_edram_raw64_to_16f me_edram_16f_to_raw64 me_edram_raw64_to_raw64 me_edram_depth_to_raw64 me_edram_raw64_to_depth me_depth_resolve_guestspace me_depth_resolve_guestspace_msaa2 me_edram_stencil_to_buffer me_edram_stencil_msaa2_to_buffer me_edram_color_stencil_to_buffer me_edram_color_to_color me_edram_r64_to_r64 sa2_resolve_copy sa2_depth_resolve_ms mgsv_depth_resolved_to_rgba8 mgsv_resolved_to_volume)
    set(_extension comp)
    if(_shader STREQUAL "me_edram_import")
        set(_extension vert)
    elseif(_shader MATCHES "^me_edram_(color_to_depth|depth_to_depth|depth_to_stencil|color_to_stencil|depth_to_depth_msaa2|depth_msaa2_to_depth_1x|depth_to_stencil_msaa2|depth_msaa2_to_stencil_1x|raw64_to_depth|color_to_color|r64_to_r64)$" OR _shader STREQUAL "sa2_resolve_copy" OR
           _shader STREQUAL "sa2_depth_resolve_ms")
        set(_extension frag)
    endif()
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/src/me/native/shaders/${_shader}.${_extension}")
    set(_output "${SA2_GENERATED_SHADER_DIR}/${_shader}.inc")
    set(_target_flags)
    if(_shader MATCHES "^me_edram_(stencil_to_buffer|color_stencil_to_buffer|stencil_msaa2_to_buffer|depth_to_depth_msaa2|depth_msaa2_to_depth_1x|depth_to_stencil_msaa2|depth_msaa2_to_stencil_1x)$" OR _shader STREQUAL "me_depth_resolve_guestspace_msaa2")
        set(_target_flags --target-env vulkan1.2)
        set(_output "${SA2_GENERATED_SHADER_DIR}/${_shader}_generated.inc")
    endif()
    add_custom_command(OUTPUT "${_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SA2_GENERATED_SHADER_DIR}"
        COMMAND "${SA2_GLSLANG_VALIDATOR}" -V ${_target_flags} -x -o "${_output}" "${_source}"
        DEPENDS "${_source}" VERBATIM)
    list(APPEND SA2_EDRAM_SHADER_INCLUDES "${_output}")
endforeach()
add_custom_target(mgsv_edram_shaders DEPENDS ${SA2_EDRAM_SHADER_INCLUDES})
add_dependencies(mgsv_gz mgsv_edram_shaders)
target_include_directories(mgsv_gz PRIVATE
    "${SA2_GENERATED_SHADER_DIR}"
    src
    src/me
    src/me/native
    src/me/native/shaders
    "${REXSDK_DIR}/src/core"
    # Precompiled SPIR-V of the SDK presenter (vulkan_spirv/guest_output_*): the renderer draws into its output.
    "${REXSDK_DIR}/src/ui/shaders")

# CPU-only SDK sources the native renderer reuses (draw-extent estimation, shader microcode interpretation).
target_sources(mgsv_gz PRIVATE
    "${REXSDK_DIR}/src/graphics/register_file.cpp"
    "${REXSDK_DIR}/src/graphics/format/ucode.cpp"
    "${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp")

# The renderer includes xxhash.h and renderdoc_app.h directly; with the SDK built from source they are private to it.
foreach(_dep xxhash renderdoc)
    if(TARGET ${_dep})
        target_include_directories(mgsv_gz PRIVATE $<TARGET_PROPERTY:${_dep},INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
endforeach()

# As in the reference ports: no FMA contraction (results would drift from the Xbox 360) and no strict aliasing (unsafe
# for recompiled code) on every target that holds generated code; no errno check after every sqrt.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target mgsv_gz mgsv_gz_recomp)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -ffp-contract=off -fno-strict-aliasing)
        endif()
    endforeach()
    if(TARGET mgsv_gz_recomp)
        target_compile_options(mgsv_gz_recomp PRIVATE -fno-math-errno)
    endif()
endif()

if(REXGLUE_PLATFORM_SWITCH)
    # mgsv_gz_register.cpp is one huge generated function of SetFunction calls that runs once at startup: -O0
    # compiles it in seconds (GCC -O3 takes many minutes on it) and costs only microseconds at startup.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/mgsv_gz_register.cpp" APPEND PROPERTY
        COMPILE_OPTIONS -O0)
    # LTO off for the bring-up build: with 67k guest functions it would take a very long time. Turn it on once the
    # game runs (the reference ports gain ~10-20 % from LTO + PGO).
    option(MGSV_SWITCH_LTO "Link-time optimisation of the game code and the app (Switch)" OFF)
    set(MGSV_SWITCH_LTO_JOBS "8" CACHE STRING "Parallel LTRANS jobs of the Switch LTO link")
    target_compile_definitions(mgsv_gz PRIVATE XXH_FORCE_MEMORY_ACCESS=0)
    if(MGSV_SWITCH_LTO)
        foreach(target mgsv_gz_recomp mgsv_gz)
            target_compile_options(${target} PRIVATE -flto -fno-fat-lto-objects -fipa-pta)
        endforeach()
        target_link_options(mgsv_gz PRIVATE -flto=${MGSV_SWITCH_LTO_JOBS} -flto-partition=balanced -fipa-pta
                                            -ffp-contract=off -fno-strict-aliasing)
    endif()
    target_compile_options(mgsv_gz_recomp PRIVATE -fomit-frame-pointer)
    # Line tables for the app's own code, so console crash addresses map to source lines.
    target_compile_options(mgsv_gz PRIVATE -g1 -gdwarf-4)
    target_link_options(mgsv_gz PRIVATE -g1 -gdwarf-4)

    # Fixed settings compiled into the NRO; the player's mgsv.toml is written next to the NRO when missing.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/mgsv_switch_defaults.toml" MGSV_BUILTIN_CONFIG)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/mgsv_switch.toml" MGSV_PLAYER_CONFIG)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/mgsv_switch_defaults.toml" "${CMAKE_CURRENT_SOURCE_DIR}/mgsv_switch.toml")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/mgsv_builtin_config.h.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/builtin_config/mgsv_builtin_config.h" @ONLY)
    target_include_directories(mgsv_gz PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/builtin_config")
    target_compile_definitions(mgsv_gz PRIVATE MGSV_BUILTIN_CONFIG=1)

    rexglue_switch_add_nro(mgsv_gz
        NAME "MGSV Ground Zeroes"
        AUTHOR "Unofficial port"
        VERSION "0.1.0")
endif()
