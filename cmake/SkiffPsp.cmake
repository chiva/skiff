# PSP targets. Configured through pspdev's toolchain file (see CMakePresets.json), which defines
# PSP and the create_pbp_file() helper that packs the ELF into an EBOOT.PBP.

# The toolchain adds the SDK with plain -I, so its headers would fail our -Wpedantic -Werror. GCC
# lets -isystem override -I for the same directory, which silences warnings from SDK code only.
# Passed as raw flags because CMake drops include_directories(SYSTEM) entries it already treats as
# implicit toolchain directories.
set(SKIFF_PSP_SYSTEM_INCLUDES "SHELL:-isystem ${PSPDEV}/psp/include"
                            "SHELL:-isystem ${PSPDEV}/psp/sdk/include")

set(SKIFF_PSP_LIBRARIES pspdebug pspdisplay pspge pspctrl)
set(SKIFF_PBP_TITLE "Skiff")
# PARAM.SFO wants "XX.YY"; the patch level does not fit and is visible in-app instead.
set(SKIFF_PBP_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
# PARAM.SFO MEMSIZE=1 unlocks the extra 32 MB on PSP-2000/3000/Go/Street. create_pbp_file() defaults
# to 2 (limited memory, for Vita compatibility), which would waste half the RAM of those models.
# The PSP-1000 has no extra memory and ignores the flag.
set(SKIFF_PBP_MEMSIZE_FULL 1)

# Shared by every PSP executable: module metadata and the HOME-menu lifecycle. OBJECT, not STATIC:
# nothing references module_info.c by symbol, so an archive member would be dropped at link time.
add_library(skiff_psp OBJECT src/platform/psp/module_info.c src/platform/psp/lifecycle.c)
target_compile_options(skiff_psp PUBLIC ${SKIFF_PSP_SYSTEM_INCLUDES})
target_link_libraries(skiff_psp PUBLIC ${SKIFF_PSP_LIBRARIES})
skiff_set_warnings(skiff_psp)

function(skiff_add_psp_app target title main_source)
  add_executable(${target} ${main_source})
  target_link_libraries(${target} PRIVATE skiff_core skiff_psp)
  skiff_set_warnings(${target})
  create_pbp_file(
    TARGET ${target}
    TITLE "${title}"
    VERSION "${SKIFF_PBP_VERSION}"
    MEMSIZE ${SKIFF_PBP_MEMSIZE_FULL}
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/pbp/${target}")
endfunction()

skiff_add_psp_app(skiff "${SKIFF_PBP_TITLE}" src/platform/psp/app_main.c)

# Headless self-test: prints check results to stdout and exits. Run by PPSSPPHeadless in CI and by
# PSPLINK on real hardware.
skiff_add_psp_app(skiff_selftest "${SKIFF_PBP_TITLE} self-test" src/platform/psp/selftest_main.c)

# Security probe: documents the weak SDK getentropy() that Skiff's TLS stack is built to avoid.
# PSP-only, since it depends on the SDK's entropy implementation; the host libc is unaffected.
skiff_add_psp_app(skiff_entropy_probe "${SKIFF_PBP_TITLE} entropy probe" tests/security/entropy_probe.c)
target_include_directories(skiff_entropy_probe PRIVATE src/platform/psp)

# TLS stack from the Skiff toolchain image (docker/toolchain.Dockerfile). Imported targets put their
# headers on the system include path, so our strict warnings do not apply to them.
find_package(MbedTLS 4.1 CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)

# What an EBOOT needs besides the libraries once it links Mbed TLS: the link-time contracts listed in
# docs/development/toolchain.md, except the entropy hook, which each EBOOT supplies.
add_library(skiff_psp_tls OBJECT src/platform/psp/mbedtls_time.c)
target_compile_options(skiff_psp_tls PRIVATE ${SKIFF_PSP_SYSTEM_INCLUDES})
target_link_libraries(skiff_psp_tls PUBLIC CURL::libcurl MbedTLS::mbedtls)
skiff_set_warnings(skiff_psp_tls)

# TLS toolchain probe: proves the image's libcurl uses Mbed TLS 4.1 and that TLS randomness comes
# only from mbedtls_platform_get_entropy() (see tests/security/tls_probe.c).
skiff_add_psp_app(skiff_tls_probe "${SKIFF_PBP_TITLE} TLS probe" tests/security/tls_probe.c)
target_link_libraries(skiff_tls_probe PRIVATE skiff_psp_tls)
target_include_directories(skiff_tls_probe PRIVATE src/platform/psp)
