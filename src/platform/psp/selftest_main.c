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
/* PPSSPPHeadless runs an EBOOT from its folder mounted as a disc, where folders cannot be made
 * (sceIoMkdir answers 0 and creates nothing): the storage checks then use the emulated Memory
 * Stick. A real PSP runs the self-test from ms0: or ef0:. */
#define DISC_DEVICE "umd0:"
#define DISC_FALLBACK_DEVICE "ms0:"
#define DISC_FALLBACK_APP "ms0:/PSP"

static void log_line(void *ctx, const char *line) { skiff_psp_report_line(ctx, line); }

/* Hardware row S1: the free space of the device the EBOOT runs from, to compare with the XMB's
 * (Settings > System Settings > Memory Stick, or the PSP Go's System Storage). Information only: it
 * does not change the self-test's result. err is why storage or roots could not be made. */
static void report_free_space(skiff_psp_report *report, skiff_storage *storage,
                              const skiff_storage_roots *roots, skiff_err err) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    uint64_t free_bytes = 0;
    if (err == SKIFF_OK) {
        err = skiff_storage_free_space(storage, roots->app, &free_bytes);
    }
    const size_t device_length = strcspn(roots->app, ":");
    if (err == SKIFF_OK) {
        snprintf(line, sizeof line, "free space %.*s: %llu bytes (%llu MiB)", (int)device_length,
                 roots->app, (unsigned long long)free_bytes,
                 (unsigned long long)(free_bytes / BYTES_PER_MIB));
    } else {
        const skiff_psp_storage_failure failure = skiff_psp_storage_last_failure(storage);
        snprintf(line, sizeof line, "free space %.*s unknown: %s (%d), %s returned 0x%08X",
                 (int)device_length, roots->app, skiff_err_name(err), (int)err,
                 failure.call != NULL ? failure.call : "-", (unsigned)failure.sce_result);
    }
    skiff_psp_report_line(report, line);
}

/* app: and the other roots for the self-test's storage checks (see DISC_DEVICE). */
static skiff_err selftest_roots(const char *program_path, skiff_storage_roots *out) {
    if (program_path != NULL && strncmp(program_path, DISC_DEVICE, strlen(DISC_DEVICE)) == 0) {
        return skiff_storage_roots_init(DISC_FALLBACK_DEVICE, DISC_FALLBACK_APP, out);
    }
    return skiff_storage_roots_from_program(program_path, out);
}

/* After a failed check, the last sceIo call the storage saw fail, for a hardware run's result.txt.
 */
static void report_storage_failure(skiff_psp_report *report, const skiff_storage *storage) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    const skiff_psp_storage_failure failure = skiff_psp_storage_last_failure(storage);
    snprintf(line, sizeof line, "last failed storage call: %s returned 0x%08X",
             failure.call != NULL ? failure.call : "-", (unsigned)failure.sce_result);
    skiff_psp_report_line(report, line);
}

int main(int argc, char *argv[]) {
    skiff_psp_report report;
    const char *program_path = argc > 0 ? argv[0] : NULL;

    skiff_psp_install_callbacks();
    skiff_psp_report_open(&report, program_path);
    skiff_storage_roots roots;
    skiff_storage *storage = NULL;
    skiff_err err = selftest_roots(program_path, &roots);
    if (err == SKIFF_OK) {
        err = skiff_psp_storage_create(&storage);
    }
    const skiff_selftest_env env = {storage, err == SKIFF_OK ? &roots : NULL};
    skiff_selftest_result result = skiff_selftest_run(&env, log_line, &report);
    if (result.failed > 0) {
        report_storage_failure(&report, storage);
    }
    report_free_space(&report, storage, &roots, err);
    skiff_storage_destroy(storage);
    skiff_psp_report_close(&report);
    sceKernelExitGame();
    return result.failed == 0 ? 0 : 1;
}
