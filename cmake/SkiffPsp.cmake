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
# XMB artwork, rendered from the SVG masters in assets/brand/ by `scripts/dev.sh icons`.
set(SKIFF_PBP_ICON "${PROJECT_SOURCE_DIR}/assets/psp/ICON0.PNG")
set(SKIFF_PBP_BACKGROUND "${PROJECT_SOURCE_DIR}/assets/psp/PIC1.PNG")

# Shared by every PSP executable: module metadata and the HOME-menu lifecycle. OBJECT, not STATIC:
# nothing references module_info.c by symbol, so an archive member would be dropped at link time.
add_library(skiff_psp OBJECT src/platform/psp/module_info.c src/platform/psp/lifecycle.c)
target_compile_options(skiff_psp PUBLIC ${SKIFF_PSP_SYSTEM_INCLUDES})
target_link_libraries(skiff_psp PUBLIC ${SKIFF_PSP_LIBRARIES})
skiff_set_warnings(skiff_psp)

# Optional ICON and BACKGROUND name the XMB artwork; without them the XMB shows its default icon.
function(skiff_add_psp_app target title main_source)
  cmake_parse_arguments(PARSE_ARGV 3 SKIFF_APP "" "ICON;BACKGROUND" "")
  add_executable(${target} ${main_source})
  target_link_libraries(${target} PRIVATE skiff_core skiff_psp)
  skiff_set_warnings(${target})
  # create_pbp_file() packs the artwork in a POST_BUILD step, which tracks no file dependencies.
  # LINK_DEPENDS relinks the target when the artwork changes, so the EBOOT is repacked.
  set(artwork)
  if(SKIFF_APP_ICON)
    list(APPEND artwork ICON_PATH "${SKIFF_APP_ICON}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${SKIFF_APP_ICON}")
  endif()
  if(SKIFF_APP_BACKGROUND)
    list(APPEND artwork BACKGROUND_PATH "${SKIFF_APP_BACKGROUND}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${SKIFF_APP_BACKGROUND}")
  endif()
  create_pbp_file(
    TARGET ${target}
    TITLE "${title}"
    VERSION "${SKIFF_PBP_VERSION}"
    MEMSIZE ${SKIFF_PBP_MEMSIZE_FULL}
    ${artwork}
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/pbp/${target}")
endfunction()

# Only the app carries the artwork: the check EBOOTs keep the default icon, so testers can tell
# them apart from the app in the XMB.
skiff_add_psp_app(skiff "${SKIFF_PBP_TITLE}" src/platform/psp/app_main.c
  ICON "${SKIFF_PBP_ICON}" BACKGROUND "${SKIFF_PBP_BACKGROUND}")

# Output shared by the check EBOOTs below: stdout, debug screen and result.txt next to the EBOOT.
# Not linked into the app.
add_library(skiff_psp_check OBJECT src/platform/psp/report.c)
target_compile_options(skiff_psp_check PRIVATE ${SKIFF_PSP_SYSTEM_INCLUDES})
target_link_libraries(skiff_psp_check PUBLIC skiff_core)
target_include_directories(skiff_psp_check PUBLIC src/platform/psp)
skiff_set_warnings(skiff_psp_check)

# Headless self-test: prints check results and exits. Run by PPSSPPHeadless in CI, and on real
# hardware from the XMB (result.txt) or over PSPLINK.
skiff_add_psp_app(skiff_selftest "${SKIFF_PBP_TITLE} self-test" src/platform/psp/selftest_main.c)
target_link_libraries(skiff_selftest PRIVATE skiff_psp_check)

# Imports from ARK custom firmware's SystemCtrlForUser library, hand-written as import stubs
# (src/platform/psp/ark_sysctrl.S) because pspsdk ships none.
enable_language(ASM)
add_library(skiff_psp_ark OBJECT src/platform/psp/ark_sysctrl.S)
target_compile_options(skiff_psp_ark PRIVATE ${SKIFF_PSP_SYSTEM_INCLUDES})
target_include_directories(skiff_psp_ark PUBLIC src/platform/psp)

# What an EBOOT needs besides the libraries once it links Mbed TLS: the link-time contracts listed in
# docs/development/toolchain.md, except the entropy hook, which each EBOOT supplies.
add_library(skiff_psp_tls OBJECT src/platform/psp/mbedtls_time.c)
target_compile_options(skiff_psp_tls PRIVATE ${SKIFF_PSP_SYSTEM_INCLUDES})
target_link_libraries(skiff_psp_tls PUBLIC CURL::libcurl MbedTLS::mbedtls)
skiff_set_warnings(skiff_psp_tls)

# TLS toolchain probe: proves the image's libcurl uses Mbed TLS 4.1 and that TLS randomness comes
# only from mbedtls_platform_get_entropy() (see tests/security/tls_probe.c).
skiff_add_psp_app(skiff_tls_probe "${SKIFF_PBP_TITLE} TLS probe" tests/security/tls_probe.c)
target_link_libraries(skiff_tls_probe PRIVATE skiff_psp_tls skiff_psp_check)

# Skiff's TLS entropy hook: KIRK through ARK, behind skiff_entropy_fill()'s health test. Every EBOOT
# that uses TLS for real links it; the TLS probe instead supplies a hook that refuses. Object
# libraries pass their objects only to targets that link them directly, so such an EBOOT also links
# skiff_psp_ark and skiff_psp_tls itself.
add_library(skiff_psp_entropy OBJECT src/platform/psp/kirk_entropy.c)
target_compile_options(skiff_psp_entropy PRIVATE ${SKIFF_PSP_SYSTEM_INCLUDES})
target_link_libraries(skiff_psp_entropy PUBLIC skiff_core skiff_psp_ark skiff_psp_tls)
skiff_set_warnings(skiff_psp_entropy)

# KIRK probe (Phase 1 hardware spike): measures ARK's sctrlKernelRand() on a real PSP, against the
# toolchain's default getentropy(), then runs Skiff's TLS stack seeded by skiff_psp_entropy.
# Hardware only, since PPSSPP has no ARK; not run in CI.
skiff_add_psp_app(skiff_kirk_probe "${SKIFF_PBP_TITLE} KIRK probe" tests/security/kirk_probe.c)
target_link_libraries(skiff_kirk_probe PRIVATE skiff_psp_check skiff_psp_ark skiff_psp_tls
                                               skiff_psp_entropy)

# The PSP's network stack (src/platform/psp/net_psp.c): CPU clock, modules, access-point connection,
# teardown. psputility, pspnet_inet and pspnet_resolver are left out: psp-gcc already links them
# after everything else, and listing a stub library twice splits its import stubs, which
# psp-fixup-imports rejects ("stubs out of order"). psppower is not among them, so it is listed
# here (and only here: its users get it through this library).
add_library(skiff_psp_net OBJECT src/platform/psp/net_psp.c)
target_compile_options(skiff_psp_net PRIVATE ${SKIFF_PSP_SYSTEM_INCLUDES})
target_link_libraries(skiff_psp_net PUBLIC skiff_core pspnet_apctl pspnet pspwlan psppower)
target_include_directories(skiff_psp_net PUBLIC src/platform/psp)
skiff_set_warnings(skiff_psp_net)

# Helpers shared by the hardware probes (tests/hardware/): the plain-C part, also unit-tested on the
# host, and the PSP part (check lines, memory, configuration file, network stack, TLS session). Like
# every object library, its users also link the object libraries it builds on.
add_library(skiff_probe_support OBJECT tests/hardware/probe_support.c tests/hardware/probe_psp.c)
target_compile_options(skiff_probe_support PRIVATE ${SKIFF_PSP_SYSTEM_INCLUDES})
target_link_libraries(skiff_probe_support PUBLIC skiff_core skiff_psp_check skiff_psp_net
                                                 skiff_psp_tls)
target_include_directories(skiff_probe_support PUBLIC tests/hardware)
skiff_set_warnings(skiff_probe_support)

# Network probe (Phase 1 hardware spike): joins Wi-Fi through a saved profile and runs HTTPS and mTLS
# requests against the test RomM (tests/integration), raw and through Skiff's transport (skiff_net).
# Without ARK (PPSSPP, CI) it checks that TLS refuses while the network modules still load and
# unload.
skiff_add_psp_app(skiff_net_probe "${SKIFF_PBP_TITLE} network probe" tests/hardware/net_probe.c)
target_link_libraries(skiff_net_probe PRIVATE skiff_net skiff_probe_support skiff_psp_check
                                              skiff_psp_net skiff_psp_ark skiff_psp_tls
                                              skiff_psp_entropy)

# Benchmark (Phase 1 hardware spike, W6): download speed by curl and socket buffer, TLS version and
# cipher, plain HTTP and CPU clock, with the CPU's busy share; hash and cipher speed; Memory Stick
# speed; first-request latency after joining (see tests/hardware/bench.c). zlib's CRC-32 is one of
# the candidates for the integrity check.
skiff_add_psp_app(skiff_bench "${SKIFF_PBP_TITLE} benchmark" tests/hardware/bench.c)
target_link_libraries(skiff_bench PRIVATE skiff_net skiff_probe_support skiff_psp_check skiff_psp_net
                                          skiff_psp_ark skiff_psp_tls skiff_psp_entropy ZLIB::ZLIB)

# UI stack prototype (Phase 1 hardware spike): GU + intraFont with the firmware fonts, the on-screen
# keyboard and the network picker (see tests/prototype/ui_proto.c). intraFont comes from pspdev's
# packages; it draws through GU, so it goes before pspgu.
skiff_add_psp_app(skiff_ui_proto "${SKIFF_PBP_TITLE} UI prototype" tests/prototype/ui_proto.c)
target_link_libraries(skiff_ui_proto PRIVATE skiff_psp_check skiff_psp_net intrafont pspgu)
