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

# Test doubles shared by the unit and integration tests (tests/support/).
find_package(Threads REQUIRED)
add_library(skiff_test_support STATIC support/fake_storage.c support/fake_transport.c
                                      support/local_http_server.c support/temp_dir.c)
target_link_libraries(skiff_test_support PUBLIC skiff_core Threads::Threads)
target_include_directories(skiff_test_support PUBLIC support)
target_compile_definitions(skiff_test_support
                           PRIVATE SKIFF_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# glibc declares nftw() only for X/Open; only temp_dir.c gets it, so the rest keeps glibc's
# defaults (local_http_server.c's socket flags).
set_source_files_properties(support/temp_dir.c PROPERTIES COMPILE_DEFINITIONS _XOPEN_SOURCE=700)
skiff_set_warnings(skiff_test_support)

function(skiff_add_unit_test name)
  add_executable(${name} unit/${name}.c)
  target_link_libraries(${name} PRIVATE skiff_core skiff_test_support unity)
  target_include_directories(${name} PRIVATE "${PROJECT_BINARY_DIR}/generated")
  skiff_set_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# A unit test that links Skiff's network layer and TLS stack, with the host's link-time contracts in
# place of the PSP's.
function(skiff_add_tls_unit_test name)
  skiff_add_unit_test(${name})
  target_link_libraries(${name} PRIVATE skiff_net skiff_host_tls)
endfunction()

skiff_add_unit_test(test_error)
skiff_add_unit_test(test_version)
skiff_add_unit_test(test_i18n)
skiff_add_unit_test(test_ui)
# Reads its codes back with zbarimg (host image), through popen().
skiff_add_unit_test(test_ui_qr)
set_source_files_properties(unit/test_ui_qr.c PROPERTIES COMPILE_DEFINITIONS _XOPEN_SOURCE=700)
# The storage-list check runs on the POSIX storage over a temporary directory.
skiff_add_unit_test(test_selftest)
target_link_libraries(test_selftest PRIVATE skiff_host_storage)
skiff_add_unit_test(test_entropy)
skiff_add_unit_test(test_http)
skiff_add_unit_test(test_transport)
skiff_add_unit_test(test_fake_transport)
skiff_add_unit_test(test_romm)
skiff_add_unit_test(test_romm_installs)
skiff_add_unit_test(test_romm_pairing)
# Reaches the shared request helpers the pairing code uses (src/romm/romm_internal.h).
target_include_directories(test_romm_pairing PRIVATE "${PROJECT_SOURCE_DIR}/src/romm")
# Uploads the recorder's synthetic save units straight from tests/fixtures/romm/ through the POSIX
# storage.
skiff_add_unit_test(test_romm_saves)
target_link_libraries(test_romm_saves PRIVATE skiff_host_storage)
target_compile_definitions(test_romm_saves
                           PRIVATE SKIFF_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# Builds the PNGs it decodes in memory with libpng's writer: no picture files in the repository.
skiff_add_unit_test(test_cover)
skiff_add_unit_test(test_download_state)
# Tests that write files: the POSIX storage over a temporary directory, through the fake storage.
skiff_add_unit_test(test_storage_paths)
skiff_add_unit_test(test_storage)
target_link_libraries(test_storage PRIVATE skiff_host_storage)
skiff_add_unit_test(test_download)
target_link_libraries(test_download PRIVATE skiff_host_storage)
skiff_add_unit_test(test_disc_image)
target_link_libraries(test_disc_image PRIVATE skiff_host_storage)
skiff_add_unit_test(test_config)
target_link_libraries(test_config PRIVATE skiff_host_storage)
skiff_add_unit_test(test_install)
target_link_libraries(test_install PRIVATE skiff_host_storage)
# Save units: the MD5 and PARAM.SFO readers, then units over the POSIX storage, reading the
# synthetic units in fixtures/saves/ (made by fixtures/saves/make_units.py).
skiff_add_unit_test(test_md5)
skiff_add_unit_test(test_param_sfo)
skiff_add_unit_test(test_saves)
target_link_libraries(test_saves PRIVATE skiff_host_storage)
target_compile_definitions(test_saves PRIVATE SKIFF_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
skiff_add_unit_test(test_saves_refusals)
target_link_libraries(test_saves_refusals PRIVATE skiff_host_storage)
# Restores cut at every step and finished by the recovery, backups, the baseline and the decision
# rule, over the POSIX storage through the fake storage.
skiff_add_unit_test(test_saves_restore)
target_link_libraries(test_saves_restore PRIVATE skiff_host_storage)
# Calls the backups' pruning directly (src/saves/saves_internal.h).
target_include_directories(test_saves_restore PRIVATE "${PROJECT_SOURCE_DIR}/src/saves")
skiff_add_unit_test(test_saves_sync)
target_link_libraries(test_saves_sync PRIVATE skiff_host_storage)
skiff_add_unit_test(test_cover_cache)
target_link_libraries(test_cover_cache PRIVATE skiff_host_storage)
skiff_add_unit_test(test_log)
target_link_libraries(test_log PRIVATE skiff_host_storage)
skiff_add_unit_test(test_jobs)
target_link_libraries(test_jobs PRIVATE skiff_host_storage)
# Reaches the app's state (src/app/app_internal.h) to inject the worker's events.
skiff_add_unit_test(test_app)
target_link_libraries(test_app PRIVATE skiff_host_storage)
target_include_directories(test_app PRIVATE "${PROJECT_SOURCE_DIR}/src/app")
skiff_add_tls_unit_test(test_host_tls)
skiff_add_tls_unit_test(test_net_errors)
skiff_add_tls_unit_test(test_curl_transport)

# The hardware probes' plain-C helpers (tests/hardware/probe_support.c), shared with their EBOOTs.
skiff_add_unit_test(test_probe_support)
target_sources(test_probe_support PRIVATE hardware/probe_support.c)
target_include_directories(test_probe_support PRIVATE hardware)

# Skiff's side of the sigil cross-check, not under ctest: scripts/dev.sh sigil-check
# (tests/sigil/check.sh). Built with every host build so it always compiles.
add_executable(skiff_unit_tool sigil/skiff_unit_tool.c)
target_link_libraries(skiff_unit_tool PRIVATE skiff_core skiff_host_storage)
skiff_set_warnings(skiff_unit_tool)

# Run against the integration RomM, not under ctest: scripts/dev.sh romm-test
# (tests/integration/transport-test.sh). Built with every host build so they always compile.
add_executable(test_transport_romm integration/test_transport_romm.c)
target_link_libraries(test_transport_romm PRIVATE skiff_net skiff_host_tls unity)
skiff_set_warnings(test_transport_romm)
add_executable(test_download_romm integration/test_download_romm.c)
target_link_libraries(test_download_romm PRIVATE skiff_net skiff_host_tls skiff_host_storage
                                                 skiff_test_support unity)
skiff_set_warnings(test_download_romm)
add_executable(test_client_romm integration/test_client_romm.c)
target_link_libraries(test_client_romm PRIVATE skiff_net skiff_host_tls unity)
skiff_set_warnings(test_client_romm)
add_executable(test_pairing_romm integration/test_pairing_romm.c)
target_link_libraries(test_pairing_romm PRIVATE skiff_net skiff_host_tls unity)
skiff_set_warnings(test_pairing_romm)
add_executable(test_saves_romm integration/test_saves_romm.c)
target_link_libraries(test_saves_romm PRIVATE skiff_net skiff_host_tls skiff_host_storage
                                              skiff_test_support unity)
target_compile_definitions(test_saves_romm
                           PRIVATE SKIFF_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
skiff_set_warnings(test_saves_romm)
