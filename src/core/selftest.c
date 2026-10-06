#include "skiff/selftest.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "skiff/config.h"
#include "skiff/download.h"
#include "skiff/http.h"
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
/* A resumed download past 4 GiB: offsets that need all 64 bits on a 32-bit CPU. */
#define SKIFF_SELFTEST_RANGE_HEADER "Content-Range: bytes 4294967296-6442450943/6442450944\r\n"
#define SKIFF_SELFTEST_RANGE_START 4294967296ULL
#define SKIFF_SELFTEST_RANGE_END 6442450943ULL
#define SKIFF_SELFTEST_RANGE_TOTAL 6442450944ULL
/* A .resume file for a download cut just short of the FAT32 limit: values a 32-bit long cannot
 * hold. */
#define SKIFF_SELFTEST_STATE_SIZE 4294967295ULL
#define SKIFF_SELFTEST_STATE_OFFSET 4294967290ULL
#define SKIFF_SELFTEST_STATE_CRC32 0xCBF43926U
#define SKIFF_SELFTEST_STATE_ETAG "\"6ac37763-1000\""
/* config.ini as Notepad saves it: a byte-order mark, CRLF line endings, mixed-case names. */
#define SKIFF_SELFTEST_CONFIG_TEXT                                                                 \
    "\xEF\xBB\xBF[Server]\r\nURL = https://romm.lan:8443\r\n[mtls]\r\ncert_file = psp.crt\r\n"     \
    "key_file = psp.key\r\n[headers]\r\nCF-Access-Client-Id = "                                    \
    "id.access\r\n[skiff]\r\nversion=1\r\n"
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

/* Resuming reads Content-Range with 64-bit arithmetic, which the PSP's 32-bit MIPS CPU does in
 * software (libgcc): check it gives the host's answer. */
static const char *check_http_range_parsing(void) {
    skiff_http_response response;
    skiff_http_response_reset(&response);
    skiff_http_response_parse_header(&response, SKIFF_SELFTEST_RANGE_HEADER,
                                     sizeof SKIFF_SELFTEST_RANGE_HEADER - 1);
    return response.has_content_range && response.range_start == SKIFF_SELFTEST_RANGE_START &&
                   response.range_end == SKIFF_SELFTEST_RANGE_END &&
                   response.range_total == SKIFF_SELFTEST_RANGE_TOTAL
               ? NULL
               : "Content-Range above 4 GiB parsed wrongly";
}

/* The .resume file is written and read with newlib's printf and strtoull on the PSP, and its check
 * line with zlib's CRC-32: a 64-bit offset must survive the round trip. */
static const char *check_download_state(void) {
    skiff_download_state state;
    memset(&state, 0, sizeof state);
    state.size = SKIFF_SELFTEST_STATE_SIZE;
    state.offset = SKIFF_SELFTEST_STATE_OFFSET;
    state.crc32 = SKIFF_SELFTEST_STATE_CRC32;
    state.has_expected_crc32 = 1;
    state.expected_crc32 = SKIFF_SELFTEST_STATE_CRC32;
    snprintf(state.etag, sizeof state.etag, "%s", SKIFF_SELFTEST_STATE_ETAG);
    char text[SKIFF_DOWNLOAD_STATE_MAX];
    size_t length = 0;
    skiff_download_state parsed;
    if (skiff_download_state_format(&state, text, sizeof text, &length) != SKIFF_OK ||
        skiff_download_state_parse(text, length, &parsed) != SKIFF_OK) {
        return "a .resume file did not survive a round trip";
    }
    return parsed.size == state.size && parsed.offset == state.offset &&
                   parsed.crc32 == state.crc32 && parsed.expected_crc32 == state.expected_crc32 &&
                   strcmp(parsed.etag, state.etag) == 0
               ? NULL
               : "a .resume file came back with other values";
}

/* config.ini is read with newlib's string functions and strtoul on the PSP, and edited in place:
 * a Windows-saved file must parse, and an edit must parse back. */
static const char *check_config_parsing(void) {
    static const char text[] = SKIFF_SELFTEST_CONFIG_TEXT;
    static skiff_config config;
    static char edited[SKIFF_CONFIG_TEXT_MAX + 1];
    size_t edited_length = 0;
    if (skiff_config_parse(text, sizeof text - 1, &config, NULL) != SKIFF_OK ||
        strcmp(config.server_url, "https://romm.lan:8443") != 0 ||
        strcmp(config.key_file, "psp.key") != 0 || config.header_count != 1 ||
        strcmp(config.headers[0].value, "id.access") != 0) {
        return "a Windows-saved config.ini parsed wrongly";
    }
    if (skiff_config_set(text, sizeof text - 1, "auth", "token", "rmm_selftest", edited,
                         sizeof edited, &edited_length) != SKIFF_OK ||
        skiff_config_parse(edited, edited_length, &config, NULL) != SKIFF_OK ||
        strcmp(config.token, "rmm_selftest") != 0 ||
        strcmp(config.server_url, "https://romm.lan:8443") != 0) {
        return "an edited config.ini did not parse back";
    }
    return NULL;
}

/* One check per line: the order here is the order of the output lines. */
// clang-format off
static const selftest_check CHECKS[] = {
    {"version", check_version_matches_components},
    {"libc-formatting", check_libc_formatting},
    {"heap-headroom", check_heap_headroom},
    {"clock", check_clock_advances},
    {"little-endian", check_little_endian},
    {"http-range", check_http_range_parsing},
    {"download-state", check_download_state},
    {"config-parse", check_config_parsing},
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

skiff_err skiff_selftest_sibling_path(const char *program_path, const char *file_name, char *out,
                                      size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (program_path == NULL || file_name == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const char *last_separator = strrchr(program_path, '/');
    if (last_separator == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    /* Keeps the trailing '/', so "ms0:/EBOOT.PBP" becomes "ms0:/result.txt". */
    const size_t directory_length = (size_t)(last_separator - program_path) + 1;
    const size_t name_length = strlen(file_name);
    if (directory_length + name_length + 1 > out_size) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(out, program_path, directory_length);
    memcpy(out + directory_length, file_name, name_length + 1);
    return SKIFF_OK;
}

skiff_err skiff_selftest_result_path(const char *program_path, char *out, size_t out_size) {
    return skiff_selftest_sibling_path(program_path, SKIFF_SELFTEST_RESULT_FILE, out, out_size);
}
