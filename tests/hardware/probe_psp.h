#ifndef SKIFF_PROBE_PSP_H
#define SKIFF_PROBE_PSP_H

/*
 * PSP-side helpers shared by the hardware probes (network probe, benchmark): check lines, memory
 * snapshots, the probe's configuration file, the network stack's load and teardown, and the TLS
 * session libcurl negotiated. Output goes through report.h.
 */

#include <curl/curl.h>
#include <pspkerneltypes.h>
#include <stddef.h>

#include "skiff/error.h"

#include "net_psp.h"
#include "probe_support.h"
#include "report.h"

enum {
    SKIFF_PROBE_PATH_MAX = 256,
    SKIFF_PROBE_TLS_NAME_MAX = 64,
    /* Request and run-log lines carry labels, curl's message and TLS names: longer than a check. */
    SKIFF_PROBE_LONG_LINE_MAX = 512,
};

typedef struct skiff_probe_memory {
    size_t heap_used;
    SceSize system_free;
    SceSize system_largest;
} skiff_probe_memory;

skiff_probe_memory skiff_probe_memory_now(void);

void skiff_probe_report_memory(skiff_psp_report *report, const char *label,
                               const skiff_probe_memory *memory);

/* "ok   <text>" or "FAIL <text>". */
void skiff_probe_report_check(skiff_psp_report *report, int ok, const char *text);

void skiff_probe_report_net_failure(skiff_psp_report *report, const skiff_psp_net *net,
                                    skiff_err err);

/* The full path of file_name next to the EBOOT (program_path is argv[0]); 0 if it does not fit. */
int skiff_probe_sibling(const char *program_path, const char *file_name, char *out);

/* skiff_probe_sibling(), and the file exists and is readable; reports a FAIL line if not. */
int skiff_probe_existing_sibling(skiff_psp_report *report, const char *program_path,
                                 const char *file_name, char *out);

/*
 * Reads file_name next to the EBOOT into config (starting from the defaults). Reports a FAIL line
 * when the file is missing (how to create it) or a value is unusable; returns 0 then.
 */
int skiff_probe_load_config(skiff_psp_report *report, const char *program_path,
                            const char *file_name, skiff_probe_config *config);

/* Loads the network modules, reporting free memory before and after; 0 (and a FAIL line) if not. */
int skiff_probe_load_network(skiff_psp_report *report, skiff_psp_net *net);

/* Disconnects, then unloads only if the disconnect is proven: never under a live connection. */
int skiff_probe_tear_down(skiff_psp_report *report, skiff_psp_net *net, long long timeout_us);

/*
 * The TLS version and cipher suite of curl's current transfer (Mbed TLS names), or empty strings.
 * libcurl exposes the session only while the transfer runs, so call it from a write callback.
 */
void skiff_probe_read_tls_session(CURL *curl, char *version, char *cipher);

#endif
