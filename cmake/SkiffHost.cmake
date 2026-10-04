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

if(SKIFF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
