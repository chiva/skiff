# Host build: unit tests for the portable layers, optionally under sanitizers or coverage.

option(SKIFF_BUILD_TESTS "Build the host unit tests" ON)
option(SKIFF_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SKIFF_COVERAGE "Build with gcov instrumentation" OFF)

# PUBLIC on skiff_core so every test executable linking it inherits the same instrumentation.
if(SKIFF_SANITIZE)
  set(SKIFF_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer
                          -fno-sanitize-recover=all)
  target_compile_options(skiff_core PUBLIC ${SKIFF_SANITIZER_FLAGS})
  target_link_options(skiff_core PUBLIC ${SKIFF_SANITIZER_FLAGS})
endif()

if(SKIFF_COVERAGE)
  target_compile_options(skiff_core PUBLIC --coverage -O0)
  target_link_options(skiff_core PUBLIC --coverage)
endif()

# What a host binary needs besides the libraries once it links Mbed TLS: the link-time contracts the
# PSP's skiff_psp_tls and skiff_psp_entropy supply on hardware (src/platform/host/tls_hooks.c).
add_library(skiff_host_tls OBJECT src/platform/host/tls_hooks.c)
target_link_libraries(skiff_host_tls PUBLIC CURL::libcurl MbedTLS::mbedtls)
skiff_set_warnings(skiff_host_tls)

# skiff_storage over POSIX file calls, standing in for the PSP's sceIo storage in host tests
# (src/platform/host/storage_posix.h).
add_library(skiff_host_storage OBJECT src/platform/host/storage_posix.c)
target_link_libraries(skiff_host_storage PUBLIC skiff_core)
target_include_directories(skiff_host_storage PUBLIC src/platform/host)
skiff_set_warnings(skiff_host_storage)

if(SKIFF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
