cmake_minimum_required(VERSION 3.25)

project(
  skiff
  VERSION 0.2.0 # x-release-please-version
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

# Nayuki's QR Code generator for the pairing address (src/ui/qr.c), built into both images from the
# same pinned source (docker/qrcodegen/build-qrcodegen.sh) and found like cJSON.
find_path(SKIFF_QRCODEGEN_INCLUDE_DIR qrcodegen/qrcodegen.h REQUIRED)
find_library(SKIFF_QRCODEGEN_LIBRARY qrcodegen REQUIRED)
add_library(skiff_qrcodegen UNKNOWN IMPORTED)
set_target_properties(
  skiff_qrcodegen PROPERTIES IMPORTED_LOCATION "${SKIFF_QRCODEGEN_LIBRARY}"
                             INTERFACE_INCLUDE_DIRECTORIES "${SKIFF_QRCODEGEN_INCLUDE_DIR}")

# libpng for covers (src/cover/): pspdev packages 1.6.53 for the PSP, and the host image builds the
# same version with the same default options (docker/host.Dockerfile). Found like cJSON.
find_path(SKIFF_PNG_INCLUDE_DIR png.h REQUIRED)
find_library(SKIFF_PNG_LIBRARY png16 REQUIRED)
add_library(skiff_png UNKNOWN IMPORTED)
set_target_properties(
  skiff_png PROPERTIES IMPORTED_LOCATION "${SKIFF_PNG_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES
                                                                "${SKIFF_PNG_INCLUDE_DIR}")
# libpng inflates with zlib and uses libm (pkg-config: Libs.private).
set_property(TARGET skiff_png PROPERTY INTERFACE_LINK_LIBRARIES ZLIB::ZLIB m)

# Portable logic: no PSP system calls, so it builds and is unit-tested on the host too.
add_library(
  skiff_core STATIC
  src/app/app.c
  src/app/app_call.c
  src/app/app_connect.c
  src/app/app_library.c
  src/app/app_queue.c
  src/app/app_settings.c
  src/app/app_view.c
  src/config/config.c
  src/core/error.c
  src/core/selftest.c
  src/core/version.c
  src/cover/decode.c
  src/i18n/i18n.c
  src/install/install.c
  src/install/manifest.c
  src/jobs/download.c
  src/jobs/download_state.c
  src/jobs/queue.c
  src/jobs/runner.c
  src/log/log.c
  src/net/entropy.c
  src/net/http.c
  src/net/transport.c
  src/romm/pairing.c
  src/romm/romm.c
  src/storage/paths.c
  src/storage/storage.c
  src/storage/whole_file.c
  src/ui/qr.c
  src/ui/ui.c)
target_include_directories(
  skiff_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(skiff_core PUBLIC skiff_png ZLIB::ZLIB skiff_cjson skiff_qrcodegen)
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
