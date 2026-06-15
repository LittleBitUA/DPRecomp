# deadlyprem - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(deadlyprem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(DEADLYPREM_SOURCES
    src/main.cpp
    src/deadlyprem_hooks.cpp
)

if(WIN32)
    add_executable(deadlyprem WIN32 ${DEADLYPREM_SOURCES})
else()
    add_executable(deadlyprem ${DEADLYPREM_SOURCES})
endif()

rexglue_setup_target(deadlyprem)
