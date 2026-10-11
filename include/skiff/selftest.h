#ifndef SKIFF_SELFTEST_H
#define SKIFF_SELFTEST_H

/*
 * On-device self-test. The same checks run on the host (unit tests), in PPSSPPHeadless (CI) and on
 * a real PSP over PSPLINK (hardware tier), so a failure points at the environment rather than the
 * code. Each phase of the roadmap adds the checks for the layer it introduces.
 */

#include <stddef.h>

#include "skiff/error.h"
#include "skiff/storage_paths.h"

#define SKIFF_SELFTEST_LINE_MAX 160
#define SKIFF_SELFTEST_OK_MARKER "SKIFF SELFTEST OK"
#define SKIFF_SELFTEST_FAIL_MARKER "SKIFF SELFTEST FAIL"
/* Check EBOOTs (self-test, probes) also write their output to this file next to the EBOOT, so a
 * run started from the XMB, without PSPLINK, can be read back from the Memory Stick. */
#define SKIFF_SELFTEST_RESULT_FILE "result.txt"

typedef void (*skiff_selftest_log_fn)(void *ctx, const char *line);

typedef struct skiff_selftest_result {
    int passed;
    int failed;
} skiff_selftest_result;

/* Where the storage checks work: the device's storage and its roots. They create, list and remove
 * scratch folders under app: (SKIFF_SELFTEST_SCRATCH_FOLDER, SKIFF_SELFTEST_SAVES_FOLDER), so the
 * EBOOT's own folder must be writable. */
#define SKIFF_SELFTEST_SCRATCH_FOLDER SKIFF_STORAGE_ROOT_APP "/selftest-scratch"
/* The saves-unit check's own scratch folder under app:: synthetic save folders, a unit written
 * from them, and the staging folder it is extracted into, all removed afterwards. */
#define SKIFF_SELFTEST_SAVES_FOLDER SKIFF_STORAGE_ROOT_APP "/selftest-saves"

typedef struct skiff_selftest_env {
    skiff_storage *storage;
    const skiff_storage_roots *roots;
} skiff_selftest_env;

/* Runs every check, emitting one line per check and a final OK/FAIL marker line through log. The
 * storage checks fail when env, or its storage or roots, is NULL. */
skiff_selftest_result skiff_selftest_run(const skiff_selftest_env *env, skiff_selftest_log_fn log,
                                         void *ctx);

/* skiff_storage_sibling_path() (skiff/storage_paths.h) for SKIFF_SELFTEST_RESULT_FILE. */
skiff_err skiff_selftest_result_path(const char *program_path, char *out, size_t out_size);

#endif
