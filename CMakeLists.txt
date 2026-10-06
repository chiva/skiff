cmake_minimum_required(VERSION 3.25)

project(
  skiff
  VERSION 0.1.0 # x-release-please-version
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

# zlib's CRC-32, for downloads (src/jobs/): pspdev packages zlib for the PSP, the host image ships
# zlib1g-dev.
find_package(ZLIB REQUIRED)

# cJSON for RomM's responses (src/romm/): pspdev packages 1.7.16 for the PSP, and the host image
# builds the same version (docker/host.Dockerfile). Neither installs a CMake package, so it is found
# by header and library; the imported target keeps its header off our strict warnings.
find_path(SKIFF_CJSON_INCLUDE_DIR cjson/cJSON.h REQUIRED)
find_library(SKIFF_CJSON_LIBRARY cjson REQUIRED)
add_library(skiff_cjson UNKNOWN IMPORTED)
set_target_properties(
  skiff_cjson PROPERTIES IMPORTED_LOCATION "${SKIFF_CJSON_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES
                                                                    "${SKIFF_CJSON_INCLUDE_DIR}")
if(PSP)
  # newlib's libm, which cJSON's number printing needs (pkg-config: Libs.private).
  set_property(TARGET skiff_cjson PROPERTY INTERFACE_LINK_LIBRARIES m)
endif()

# Portable logic: no PSP system calls, so it builds and is unit-tested on the host too.
add_library(
  skiff_core STATIC
  src/config/config.c
  src/core/error.c
  src/core/selftest.c
  src/core/version.c
  src/jobs/download.c
  src/jobs/download_state.c
  src/log/log.c
  src/net/entropy.c
  src/net/http.c
  src/net/transport.c
  src/romm/romm.c
  src/storage/storage.c)
target_include_directories(
  skiff_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(skiff_core PUBLIC ZLIB::ZLIB skiff_cjson)
skiff_set_warnings(skiff_core)

# Skiff's TLS stack, built with the same profile for both targets (docker/toolchain/build-tls.sh):
# the toolchain image installs it into the PSP SDK, the host image into /opt/skiff-tls. Imported
# targets put their headers on the system include path, so our strict warnings do not apply to them.
find_package(MbedTLS 4.1 CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)

# The real transport, libcurl over Mbed TLS (skiff/curl_transport.h), for the PSP and host tests. On
# the PSP an EBOOT linking it also links skiff_psp_tls and skiff_psp_entropy; on the host,
# skiff_host_tls.
add_library(skiff_net STATIC src/net/curl_transport.c src/net/net_errors.c)
target_link_libraries(skiff_net PUBLIC skiff_core CURL::libcurl MbedTLS::mbedtls)
skiff_set_warnings(skiff_net)

if(PSP)
  include(SkiffPsp)
else()
  include(SkiffHost)
endif()
