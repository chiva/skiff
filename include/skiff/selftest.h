#ifndef SKIFF_SELFTEST_H
#define SKIFF_SELFTEST_H

/*
 * On-device self-test. The same checks run on the host (unit tests), in PPSSPPHeadless (CI) and on
 * a real PSP over PSPLINK (hardware tier), so a failure points at the environment rather than the
 * code. Each phase of the roadmap adds the checks for the layer it introduces.
 */

#include <stddef.h>

#include "skiff/error.h"

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

/* Runs every check, emitting one line per check and a final OK/FAIL marker line through log. */
skiff_selftest_result skiff_selftest_run(skiff_selftest_log_fn log, void *ctx);

/*
 * Writes "<directory of program_path>/<file_name>" into out. program_path is argv[0] as the PSP
 * passes it, e.g. "ms0:/PSP/GAME/SkiffSelftest/EBOOT.PBP". Returns SKIFF_ERR_INVALID_ARG for a NULL
 * argument or a path without a directory, SKIFF_ERR_BUFFER_TOO_SMALL if the result does not fit; on
 * any error out is left empty (when out_size allows).
 */
skiff_err skiff_selftest_sibling_path(const char *program_path, const char *file_name, char *out,
                                      size_t out_size);

/* skiff_selftest_sibling_path() for SKIFF_SELFTEST_RESULT_FILE. */
skiff_err skiff_selftest_result_path(const char *program_path, char *out, size_t out_size);

#endif
