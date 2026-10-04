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

function(skiff_add_unit_test name)
  add_executable(${name} unit/${name}.c)
  target_link_libraries(${name} PRIVATE skiff_core unity)
  target_include_directories(${name} PRIVATE "${PROJECT_BINARY_DIR}/generated")
  skiff_set_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

skiff_add_unit_test(test_error)
skiff_add_unit_test(test_version)
skiff_add_unit_test(test_selftest)
