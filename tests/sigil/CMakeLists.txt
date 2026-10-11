# The sigil cross-check's driver (tests/sigil/check.sh): argosy-sigil, fetched by fetch-sigil.sh,
# built as a static library with its own CMake files, and sigil_driver.c on top. A project of its own,
# outside Skiff's build: sigil is MPL-2.0 and never part of Skiff or its EBOOTs.
cmake_minimum_required(VERSION 3.16)
project(skiff_sigil_check LANGUAGES C)

set(SIGIL_SOURCE_DIR "" CACHE PATH "argosy-sigil's source, as fetch-sigil.sh unpacks it")
if(NOT EXISTS "${SIGIL_SOURCE_DIR}/include/sigil.h")
  message(FATAL_ERROR "SIGIL_SOURCE_DIR must point at argosy-sigil (tests/sigil/fetch-sigil.sh)")
endif()

set(SIGIL_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(SIGIL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${SIGIL_SOURCE_DIR}" sigil EXCLUDE_FROM_ALL)
# Third-party code: its warnings are sigil's to fix, and would only bury the check's output.
foreach(target sigil sigil_chdr sigil_zlib sigil_zstd sigil_lzma sigil_aes)
  if(TARGET ${target})
    target_compile_options(${target} PRIVATE -w)
  endif()
endforeach()

add_executable(sigil_driver sigil_driver.c)
target_link_libraries(sigil_driver PRIVATE sigil)
set_target_properties(sigil_driver PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(sigil_driver PRIVATE -Wall -Wextra -Werror)
