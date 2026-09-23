# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Hallendorff
cmake_minimum_required(VERSION 3.16)
project(straddle LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimized build for single-config generators (Makefiles, Ninja).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(STRADDLE_START 0 CACHE STRING
    "Bytes of padding before the loop (asm mode). Moves the whole loop. ARM64: multiple of 4.")
option(STRADDLE_FORCE_CXX
    "Use the C++ loop even where the assembly version is available" OFF)
set(STRADDLE_ALIGN_LOOPS "" CACHE STRING
    "GCC/Clang, C++ mode only: pass -falign-loops=N (for example 64)")

add_executable(straddle straddle.cpp)
target_compile_definitions(straddle PRIVATE STRADDLE_START=${STRADDLE_START})

if(STRADDLE_FORCE_CXX)
  target_compile_definitions(straddle PRIVATE STRADDLE_FORCE_CXX=1)
endif()

if(MSVC)
  target_compile_options(straddle PRIVATE /O2 /W3)
  # Function addresses must point at the code itself, not incremental-link stubs.
  target_link_options(straddle PRIVATE /INCREMENTAL:NO)
else()
  target_compile_options(straddle PRIVATE -O2 -Wall -Wextra)
  if(STRADDLE_ALIGN_LOOPS)
    target_compile_options(straddle PRIVATE -falign-loops=${STRADDLE_ALIGN_LOOPS})
  endif()
endif()
