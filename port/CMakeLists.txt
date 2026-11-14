# mgsv_gz - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(mgsv_gz LANGUAGES CXX)

if(APPLE)
    # src/present_probe.mm is Objective-C++ (Metal/CAMetalLayer diagnostics).
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# generated/default/mgsv_gz_register.cpp is post-processed by
# tools/split_register.py right after codegen: ReXGlue emits one function with
# ~68 k SetFunction calls, which trips Apple clang's quadratic Localizer pass
# (>12 min per build). Split into 2 k-call chunks it compiles in seconds, so no
# special-casing is needed here.
#
# Re-run it if you regenerate:
#   python3 ../tools/split_register.py generated/default/mgsv_gz_register.cpp


# Sources
set(MGSVGZ_SOURCES
    src/main.cpp
)

if(APPLE)
    list(APPEND MGSVGZ_SOURCES src/present_probe.mm)
endif()

if(WIN32)
    add_executable(mgsv_gz WIN32 ${MGSVGZ_SOURCES})
else()
    add_executable(mgsv_gz ${MGSVGZ_SOURCES})
endif()

rexglue_setup_target(mgsv_gz)

if(APPLE)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    target_link_libraries(mgsv_gz PRIVATE
        ${APPKIT_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK})
    set_source_files_properties(src/present_probe.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
endif()
