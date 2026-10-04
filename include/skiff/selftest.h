#ifndef SKIFF_SELFTEST_H
#define SKIFF_SELFTEST_H

/*
 * On-device self-test. The same checks run on the host (unit tests), in PPSSPPHeadless (CI) and on
 * a real PSP over PSPLINK (hardware tier), so a failure points at the environment rather than the
 * code. Each phase of the roadmap adds the checks for the layer it introduces.
 */

#define SKIFF_SELFTEST_LINE_MAX 160
#define SKIFF_SELFTEST_OK_MARKER "SKIFF SELFTEST OK"
#define SKIFF_SELFTEST_FAIL_MARKER "SKIFF SELFTEST FAIL"

typedef void (*skiff_selftest_log_fn)(void *ctx, const char *line);

typedef struct skiff_selftest_result {
    int passed;
    int failed;
} skiff_selftest_result;

/* Runs every check, emitting one line per check and a final OK/FAIL marker line through log. */
skiff_selftest_result skiff_selftest_run(skiff_selftest_log_fn log, void *ctx);

#endif
