#include "probe_psp.h"

#include <malloc.h>
#include <mbedtls/ssl.h>
#include <psppower.h>
#include <pspsysmem.h>
#include <stdio.h>

#include "skiff/selftest.h"

enum { BYTES_PER_KB = 1024 };

skiff_probe_memory skiff_probe_memory_now(void) {
    const struct mallinfo heap = mallinfo();
    skiff_probe_memory memory = {(size_t)heap.uordblks, sceKernelTotalFreeMemSize(),
                                 sceKernelMaxFreeMemSize()};
    return memory;
}

void skiff_probe_report_memory(skiff_psp_report *report, const char *label,
                               const skiff_probe_memory *memory) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "memory %s: heap used %u KB, system free %u KB (largest %u KB)",
             label, (unsigned)(memory->heap_used / BYTES_PER_KB),
             (unsigned)(memory->system_free / BYTES_PER_KB),
             (unsigned)(memory->system_largest / BYTES_PER_KB));
    skiff_psp_report_line(report, line);
}

void skiff_probe_report_check(skiff_psp_report *report, int ok, const char *text) {
    char line[SKIFF_PROBE_LONG_LINE_MAX];
    snprintf(line, sizeof line, "%s %s", ok ? "ok  " : "FAIL", text);
    skiff_psp_report_line(report, line);
}

void skiff_probe_report_net_failure(skiff_psp_report *report, const skiff_psp_net *net,
                                    skiff_err err) {
    char line[SKIFF_PROBE_LONG_LINE_MAX];
    snprintf(line, sizeof line,
             "FAIL %s (%d): %s returned 0x%08X, access point state %d (furthest %d, firmware "
             "error 0x%08X)",
             skiff_err_name(err), (int)err, net->failed_call != NULL ? net->failed_call : "-",
             (unsigned)net->sce_result, net->apctl_state, net->apctl_furthest_state,
             (unsigned)net->apctl_error);
    skiff_psp_report_line(report, line);
}

int skiff_probe_sibling(const char *program_path, const char *file_name, char *out) {
    return skiff_selftest_sibling_path(program_path, file_name, out, SKIFF_PROBE_PATH_MAX) ==
           SKIFF_OK;
}

int skiff_probe_existing_sibling(skiff_psp_report *report, const char *program_path,
                                 const char *file_name, char *out) {
    FILE *file = skiff_probe_sibling(program_path, file_name, out) ? fopen(out, "r") : NULL;
    if (file == NULL) {
        char text[SKIFF_SELFTEST_LINE_MAX];
        snprintf(text, sizeof text,
                 "setup: %s missing next to the EBOOT (scripts/memstick.sh "
                 "install copies it)",
                 file_name);
        skiff_probe_report_check(report, 0, text);
        return 0;
    }
    fclose(file);
    return 1;
}

int skiff_probe_load_config(skiff_psp_report *report, const char *program_path,
                            const char *file_name, skiff_probe_config *config) {
    char path[SKIFF_PROBE_PATH_MAX];
    skiff_probe_config_defaults(config);
    FILE *file = skiff_probe_sibling(program_path, file_name, path) ? fopen(path, "r") : NULL;
    if (file == NULL) {
        char text[SKIFF_SELFTEST_LINE_MAX];
        snprintf(text, sizeof text,
                 "%s not found: run scripts/dev.sh romm-lan, then scripts/memstick.sh install",
                 file_name);
        skiff_probe_report_check(report, 0, text);
        return 0;
    }
    skiff_probe_config_read(file, config);
    fclose(file);
    if (config->invalid_values > 0) {
        char text[SKIFF_SELFTEST_LINE_MAX];
        snprintf(text, sizeof text, "%s: %d line(s) with an unusable value", file_name,
                 config->invalid_values);
        skiff_probe_report_check(report, 0, text);
        return 0;
    }
    return 1;
}

/* The clock the network modules loaded at; a FAIL line when it is not the one asked for. */
static int report_session_clock(skiff_psp_report *report, const skiff_psp_net *net, int cpu_mhz) {
    char text[SKIFF_SELFTEST_LINE_MAX];
    if (cpu_mhz == SKIFF_PSP_NET_CPU_MHZ_UNCHANGED) {
        snprintf(text, sizeof text, "clock while online: %d/%d MHz (left as it was)", net->cpu_mhz,
                 net->bus_mhz);
        skiff_probe_report_check(report, 1, text);
        return 1;
    }
    const int ok = net->cpu_mhz == cpu_mhz;
    snprintf(text, sizeof text, "clock while online: %d/%d MHz (asked for %d, was %d/%d)%s",
             net->cpu_mhz, net->bus_mhz, cpu_mhz, net->cpu_mhz_before, net->bus_mhz_before,
             ok ? "" : ": the firmware kept its clock");
    skiff_probe_report_check(report, ok, text);
    return ok;
}

int skiff_probe_load_network(skiff_psp_report *report, skiff_psp_net *net, int cpu_mhz) {
    const skiff_probe_memory before = skiff_probe_memory_now();
    skiff_probe_report_memory(report, "before the network modules", &before);
    const skiff_err err = skiff_psp_net_load(net, cpu_mhz);
    if (err != SKIFF_OK) {
        skiff_probe_report_net_failure(report, net, err);
        return 0;
    }
    const skiff_probe_memory after = skiff_probe_memory_now();
    skiff_probe_report_memory(report, "after the network modules load", &after);
    return report_session_clock(report, net, cpu_mhz);
}

int skiff_probe_unload_network(skiff_psp_report *report, skiff_psp_net *net) {
    const skiff_err err = skiff_psp_net_unload(net);
    if (err != SKIFF_OK) {
        skiff_probe_report_net_failure(report, net, err);
        return 0;
    }
    const skiff_probe_memory after = skiff_probe_memory_now();
    skiff_probe_report_memory(report, "after the network modules unload", &after);
    const int cpu_mhz = scePowerGetCpuClockFrequency();
    const int bus_mhz = scePowerGetBusClockFrequency();
    const int restored = cpu_mhz == net->cpu_mhz_before && bus_mhz == net->bus_mhz_before;
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "clock after unloading: %d/%d MHz (%d/%d before loading)", cpu_mhz,
             bus_mhz, net->cpu_mhz_before, net->bus_mhz_before);
    skiff_probe_report_check(report, restored, text);
    return restored;
}

int skiff_probe_tear_down(skiff_psp_report *report, skiff_psp_net *net, long long timeout_us) {
    const skiff_err err = skiff_psp_net_disconnect(net, timeout_us);
    if (err != SKIFF_OK) {
        skiff_probe_report_net_failure(report, net, err);
        skiff_probe_report_check(report, 0,
                                 "network: not disconnected, leaving the modules loaded");
        return 0;
    }
    if (!skiff_probe_unload_network(report, net)) {
        return 0;
    }
    skiff_probe_report_check(report, 1, "network: disconnected and modules unloaded");
    return 1;
}

void skiff_probe_read_tls_session(CURL *curl, char *version, char *cipher) {
    const struct curl_tlssessioninfo *session = NULL;
    version[0] = '\0';
    cipher[0] = '\0';
    if (curl_easy_getinfo(curl, CURLINFO_TLS_SSL_PTR, &session) != CURLE_OK || session == NULL ||
        session->backend != CURLSSLBACKEND_MBEDTLS || session->internals == NULL) {
        return;
    }
    const mbedtls_ssl_context *ssl = session->internals;
    snprintf(version, SKIFF_PROBE_TLS_NAME_MAX, "%s", mbedtls_ssl_get_version(ssl));
    snprintf(cipher, SKIFF_PROBE_TLS_NAME_MAX, "%s", mbedtls_ssl_get_ciphersuite(ssl));
}
