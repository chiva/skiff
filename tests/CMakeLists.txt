include(FetchContent)

# Unity is pinned to a commit rather than a tag so the fetched tree is immutable.
FetchContent_Declare(
  unity
  GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
  GIT_TAG b6763fbd9cedfacaa89e2ad9fd00d615a234e355 # v2.7.0
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(unity)
# Enables TEST_PRINTF, which the tests use to narrate what they are checking.
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_PRINT_FORMATTED)
# Consume Unity's header as a system include so our -Wpedantic -Werror does not fail on its use of
# GNU extensions (e.g. clang's -Wgnu-zero-variadic-macro-arguments in UNITY_PRINT_EOL). Only our own
# code is held to the strict warning set.
get_target_property(unity_include_dirs unity INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(unity PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                                       "${unity_include_dirs}")

function(skiff_add_unit_test name)
  add_executable(${name} unit/${name}.c)
  target_link_libraries(${name} PRIVATE skiff_core unity)
  target_include_directories(${name} PRIVATE "${PROJECT_BINARY_DIR}/generated")
  skiff_set_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# A unit test that links Skiff's TLS stack, with the host's link-time contracts in place of the PSP's.
function(skiff_add_tls_unit_test name)
  skiff_add_unit_test(${name})
  target_link_libraries(${name} PRIVATE skiff_host_tls)
endfunction()

skiff_add_unit_test(test_error)
skiff_add_unit_test(test_version)
skiff_add_unit_test(test_selftest)
skiff_add_unit_test(test_entropy)
skiff_add_tls_unit_test(test_host_tls)
