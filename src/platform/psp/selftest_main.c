#include <pspkernel.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "skiff/selftest.h"
#include "skiff/storage_paths.h"

#include "lifecycle.h"
#include "report.h"
#include "storage_psp.h"

#define BYTES_PER_MIB (1024ULL * 1024ULL)

static void log_line(void *ctx, const char *line) { skiff_psp_report_line(ctx, line); }

/* Hardware row S1: the free space of the device the EBOOT runs from, to compare with the XMB's
 * (Settings > System Settings > Memory Stick, or the PSP Go's System Storage). Information only: it
 * does not change the self-test's result. */
static void report_free_space(skiff_psp_report *report, const char *program_path) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    skiff_storage_roots roots;
    skiff_storage *storage = NULL;
    uint64_t free_bytes = 0;
    skiff_err err = skiff_storage_roots_from_program(program_path, &roots);
    if (err == SKIFF_OK) {
        err = skiff_psp_storage_create(&storage);
    }
    if (err == SKIFF_OK) {
        err = skiff_storage_free_space(storage, roots.app, &free_bytes);
    }
    const size_t device_length = strcspn(roots.app, ":");
    if (err == SKIFF_OK) {
        snprintf(line, sizeof line, "free space %.*s: %llu bytes (%llu MiB)", (int)device_length,
                 roots.app, (unsigned long long)free_bytes,
                 (unsigned long long)(free_bytes / BYTES_PER_MIB));
    } else {
        const skiff_psp_storage_failure failure = skiff_psp_storage_last_failure(storage);
        snprintf(line, sizeof line, "free space %.*s unknown: %s (%d), %s returned 0x%08X",
                 (int)device_length, roots.app, skiff_err_name(err), (int)err,
                 failure.call != NULL ? failure.call : "-", (unsigned)failure.sce_result);
    }
    skiff_storage_destroy(storage);
    skiff_psp_report_line(report, line);
}

int main(int argc, char *argv[]) {
    skiff_psp_report report;
    const char *program_path = argc > 0 ? argv[0] : NULL;

    skiff_psp_install_callbacks();
    skiff_psp_report_open(&report, program_path);
    skiff_selftest_result result = skiff_selftest_run(log_line, &report);
    report_free_space(&report, program_path);
    skiff_psp_report_close(&report);
    sceKernelExitGame();
    return result.failed == 0 ? 0 : 1;
}
