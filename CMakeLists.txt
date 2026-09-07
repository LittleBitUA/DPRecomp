# deadlyprem - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(deadlyprem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CPU baseline (2026-09-06), copied from the DownpourRecomp release block, which
# in turn mirrors skate3recomp: the COMPATIBLE build is the default, AVX2 is an
# explicit opt-in. Downpour issue #11: a -march=x86-64-v3 build failed with
# 0xc0000142 on pre-Haswell / pre-Zen CPUs (AVX2 is a hard requirement there).
# Note the SDK helper (rexglue_apply_target_settings) always adds -msse4.1 to
# the recompiled target, so the effective floor is SSE4.1 (Penryn 2007+,
# Bulldozer 2011+); the SDK itself is built with -march=x86-64 -mssse3.
# Must come BEFORE include(generated/rexglue.cmake) so add_compile_options
# covers everything it pulls in.
option(DP_ENABLE_X86_64_V3
    "Build x86_64 binaries for x86-64-v3 CPUs (AVX2/BMI2/FMA)"
    OFF)
if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    if(DP_ENABLE_X86_64_V3)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            add_compile_options(-march=x86-64-v3)
        elseif(MSVC)
            add_compile_options(/arch:AVX2)
        endif()
    else()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            add_compile_options(-march=x86-64 -mtune=generic -mssse3)
        endif()
    endif()
endif()

include(generated/rexglue.cmake)

# Sources
set(DEADLYPREM_SOURCES
    src/main.cpp
    src/deadlyprem_hooks.cpp
    src/deadlyprem_camera_hook.cpp
    src/deadlyprem_anim_event_hook.cpp
    src/deadlyprem_iso_installer.cpp
    src/deadlyprem_acquire_wizard.cpp
)

# DP1 2026-09-06: embed the game icon on Windows (see src/deadlyprem.rc).
if(WIN32)
    list(APPEND DEADLYPREM_SOURCES src/deadlyprem.rc)
endif()

if(WIN32)
    add_executable(deadlyprem WIN32 ${DEADLYPREM_SOURCES})
else()
    add_executable(deadlyprem ${DEADLYPREM_SOURCES})
endif()

rexglue_setup_target(deadlyprem GPU_PLUGINS xenos)
