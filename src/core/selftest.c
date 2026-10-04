#include "skiff/selftest.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "skiff/version.h"

typedef const char *(*selftest_check_fn)(void);

typedef struct selftest_check {
    const char *name;
    selftest_check_fn run;
} selftest_check;

/*
 * Checks target what differs between the host, PPSSPP and a real PSP: the C library (newlib on the
 * PSP), the memory actually available to the app, the clock, and byte order. Pure logic is covered
 * by the unit tests and does not belong here.
 */

/* Roughly what one download needs at once: TLS buffers, write buffer, JSON page, UI textures. */
#define SKIFF_SELFTEST_HEAP_PROBE_BYTES ((size_t)8 * 1024 * 1024)
/* Busy-wait bound while waiting for clock() to tick; about a second on a PSP. */
#define SKIFF_SELFTEST_CLOCK_SPIN_LIMIT 50000000L

/* Each check returns NULL on success or a short reason on failure. */

static const char *check_version_matches_components(void) {
    char expected[SKIFF_SELFTEST_LINE_MAX];
    snprintf(expected, sizeof expected, "%d.%d.%d", skiff_version_major(), skiff_version_minor(),
             skiff_version_patch());
    return strcmp(expected, skiff_version_string()) == 0
               ? NULL
               : "version string disagrees with components";
}

/* newlib can be built without long long or floating-point printf support; we rely on both. */
static const char *check_libc_formatting(void) {
    char formatted[SKIFF_SELFTEST_LINE_MAX];
    snprintf(formatted, sizeof formatted, "%zu|%lld|%.2f|%s", (size_t)4096, -9000000000LL, 3.14159,
             "ok");
    return strcmp(formatted, "4096|-9000000000|3.14|ok") == 0
               ? NULL
               : "snprintf lacks size_t, long long or float support";
}

static const char *check_heap_headroom(void) {
    unsigned char *probe = malloc(SKIFF_SELFTEST_HEAP_PROBE_BYTES);
    if (probe == NULL) {
        return "could not allocate the 8 MiB a download needs";
    }
    /* Touch both ends so the allocation is real, not just reserved address space. */
    probe[0] = 0xA5;
    probe[SKIFF_SELFTEST_HEAP_PROBE_BYTES - 1] = 0x5A;
    const int intact = probe[0] == 0xA5 && probe[SKIFF_SELFTEST_HEAP_PROBE_BYTES - 1] == 0x5A;
    free(probe);
    return intact ? NULL : "heap memory did not hold a written value";
}

/* Timeouts, download speed and entropy timing all depend on a working monotonic tick. */
static const char *check_clock_advances(void) {
    const clock_t start = clock();
    if (start == (clock_t)-1) {
        return "clock() is unavailable";
    }
    for (volatile long spins = 0; spins < SKIFF_SELFTEST_CLOCK_SPIN_LIMIT; spins++) {
        if (clock() != start) {
            return NULL;
        }
    }
    return "clock() did not advance";
}

/* PBP, ISO and PARAM.SFO headers are little-endian, and their parsers assume a matching CPU. */
static const char *check_little_endian(void) {
    const uint32_t marker = 0x01020304U;
    unsigned char first_byte;
    memcpy(&first_byte, &marker, sizeof first_byte);
    return first_byte == 0x04 ? NULL : "CPU is not little-endian";
}

/* One check per line: the order here is the order of the output lines. */
// clang-format off
static const selftest_check CHECKS[] = {
    {"version", check_version_matches_components},
    {"libc-formatting", check_libc_formatting},
    {"heap-headroom", check_heap_headroom},
    {"clock", check_clock_advances},
    {"little-endian", check_little_endian},
};
// clang-format on

static void emit(skiff_selftest_log_fn log, void *ctx, const char *line) {
    if (log != NULL) {
        log(ctx, line);
    }
}

skiff_selftest_result skiff_selftest_run(skiff_selftest_log_fn log, void *ctx) {
    skiff_selftest_result result = {0, 0};
    char line[SKIFF_SELFTEST_LINE_MAX];

    snprintf(line, sizeof line, "skiff %s selftest", skiff_version_string());
    emit(log, ctx, line);

    for (size_t i = 0; i < sizeof CHECKS / sizeof CHECKS[0]; i++) {
        const char *failure = CHECKS[i].run();
        if (failure == NULL) {
            result.passed++;
            snprintf(line, sizeof line, "PASS %s", CHECKS[i].name);
        } else {
            result.failed++;
            snprintf(line, sizeof line, "FAIL %s: %s", CHECKS[i].name, failure);
        }
        emit(log, ctx, line);
    }

    snprintf(line, sizeof line, "%s %d/%d",
             result.failed == 0 ? SKIFF_SELFTEST_OK_MARKER : SKIFF_SELFTEST_FAIL_MARKER,
             result.passed, result.passed + result.failed);
    emit(log, ctx, line);
    return result;
}

skiff_err skiff_selftest_result_path(const char *program_path, char *out, size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (program_path == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const char *last_separator = strrchr(program_path, '/');
    if (last_separator == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    /* Keeps the trailing '/', so "ms0:/EBOOT.PBP" becomes "ms0:/result.txt". */
    const size_t directory_length = (size_t)(last_separator - program_path) + 1;
    const size_t needed = directory_length + strlen(SKIFF_SELFTEST_RESULT_FILE) + 1;
    if (needed > out_size) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(out, program_path, directory_length);
    memcpy(out + directory_length, SKIFF_SELFTEST_RESULT_FILE, sizeof SKIFF_SELFTEST_RESULT_FILE);
    return SKIFF_OK;
}
