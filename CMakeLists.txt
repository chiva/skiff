cmake_minimum_required(VERSION 3.25)

project(
  skiff
  VERSION 0.0.0 # x-release-please-version
  DESCRIPTION "A RomM client for the PSP"
  HOMEPAGE_URL "https://github.com/chiva/skiff"
  LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# pspsdk headers rely on GNU extensions (inline asm, statement expressions).
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SkiffWarnings)

option(SKIFF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

configure_file(src/core/version_config.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version_config.h"
               @ONLY)

# Portable logic: no PSP system calls, so it builds and is unit-tested on the host too.
add_library(skiff_core STATIC src/core/error.c src/core/selftest.c src/core/version.c
                              src/net/entropy.c)
target_include_directories(
  skiff_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
skiff_set_warnings(skiff_core)

if(PSP)
  include(SkiffPsp)
else()
  include(SkiffHost)
endif()
