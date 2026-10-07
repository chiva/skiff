#include "skiff/selftest.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "skiff/config.h"
#include "skiff/download.h"
#include "skiff/http.h"
#include "skiff/i18n.h"
#include "skiff/install.h"
#include "skiff/jobs.h"
#include "skiff/log.h"
#include "skiff/romm.h"
#include "skiff/storage_paths.h"
#include "skiff/ui.h"
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
/* 2^31 seconds after 1970, in milliseconds: past what a 32-bit time_t holds. */
#define SKIFF_SELFTEST_LOG_MS 2147483648123LL
#define SKIFF_SELFTEST_LOG_TIMESTAMP "2038-01-19 03:14:08.123Z"
/* A RomM page whose numbers need more than 32 bits, and a CRC-32 with a leading zero. */
#define SKIFF_SELFTEST_ROMM_PAGE                                                                   \
    "{\"items\":[{\"id\":1099511627776,\"platform_id\":1,\"name\":\"Caf\xC3\xA9\",\"fs_name\":"    \
    "\"a.iso\","                                                                                   \
    "\"fs_size_bytes\":4294967295,\"crc_hash\":\"0a1b2c3d\"}],\"total\":1,\"offset\":0}"
#define SKIFF_SELFTEST_ROMM_ID 1099511627776ULL
#define SKIFF_SELFTEST_ROMM_SIZE 4294967295ULL
#define SKIFF_SELFTEST_ROMM_CRC32 0x0a1b2c3dU
/* A RomM name with accents and characters FAT refuses, and how it must come out. */
#define SKIFF_SELFTEST_ROMM_NAME "Pok\xC3\xA9mon: Edici\xC3\xB3n/Plata?.iso"
#define SKIFF_SELFTEST_SAFE_NAME "Pok\xC3\xA9mon_ Edici\xC3\xB3n_Plata_.iso"
/* A four-byte character, repeated past the name limit so the cut has to back up to its start. */
#define SKIFF_SELFTEST_WIDE_CHARACTER "\xF0\x9F\x8E\xAE"
#define SKIFF_SELFTEST_WIDE_COUNT 40
#define SKIFF_SELFTEST_WIDE_EXTENSION ".iso"
/* 123 bytes of stem room hold 30 whole four-byte characters. */
#define SKIFF_SELFTEST_WIDE_KEPT_BYTES 124U
/* The Spanish line for a full Memory Stick: accents through the catalogue, the code through
 * snprintf. */
#define SKIFF_SELFTEST_SPANISH_ERROR_LINE "No hay suficiente espacio libre en el Memory Stick [301]"
/* A Spanish title cut to 9 one-unit characters: six of them and the ellipsis, the sixth a two-byte
 * character. */
#define SKIFF_SELFTEST_FIT_TEXT "Edici\xC3\xB3n especial"
#define SKIFF_SELFTEST_FIT_WIDTH 9.0f
#define SKIFF_SELFTEST_FIT_EXPECTED "Edici\xC3\xB3..."
/* 1.5 GiB, with the Spanish decimal comma: 64-bit arithmetic and %llu on newlib. */
#define SKIFF_SELFTEST_UI_BYTES 1610612736ULL
#define SKIFF_SELFTEST_UI_BYTES_TEXT "1,5 GB"
#define SKIFF_SELFTEST_TEXT_MAX 96
/* A queue file of one job, and a ROM id of 15 digits: past 32 bits, and the most the file keeps
 * exactly. */
#define SKIFF_SELFTEST_JOBS_TEXT_MAX 2048
#define SKIFF_SELFTEST_JOBS_ROM_ID 999999999999999ULL
/* An install recorded at 2026-10-01 00:00:00.123 UTC, in ms: above 2^32, below 2^53. */
#define SKIFF_SELFTEST_MANIFEST_MS 1790812800123LL
#define SKIFF_SELFTEST_MANIFEST_TEXT_MAX 512
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
                         sizeof edited, &edited_length, NULL) != SKIFF_OK ||
        skiff_config_parse(edited, edited_length, &config, NULL) != SKIFF_OK ||
        strcmp(config.token, "rmm_selftest") != 0 ||
        strcmp(config.server_url, "https://romm.lan:8443") != 0) {
        return "an edited config.ini did not parse back";
    }
    return NULL;
}

/* Log lines carry a date computed with 64-bit division, which the PSP's 32-bit MIPS CPU does in
 * software (libgcc), without newlib's time functions. */
static const char *check_log_timestamp(void) {
    char timestamp[SKIFF_LOG_TIMESTAMP_MAX];
    return skiff_log_format_timestamp(SKIFF_SELFTEST_LOG_MS, timestamp, sizeof timestamp) ==
                       SKIFF_OK &&
                   strcmp(timestamp, SKIFF_SELFTEST_LOG_TIMESTAMP) == 0
               ? NULL
               : "a log timestamp past 2038 came out wrong";
}

/* RomM's JSON is parsed by cJSON with newlib's strtod on the PSP, and its numbers turned into
 * 64-bit integers in software (libgcc): ids and sizes past 32 bits must come out exact. */
static const char *check_romm_json(void) {
    static const char page_text[] = SKIFF_SELFTEST_ROMM_PAGE;
    static skiff_romm_rom_page page;
    if (skiff_romm_parse_rom_page(page_text, sizeof page_text - 1, &page) != SKIFF_OK ||
        page.count != 1) {
        return "a RomM page did not parse";
    }
    return page.items[0].id == SKIFF_SELFTEST_ROMM_ID &&
                   page.items[0].size == SKIFF_SELFTEST_ROMM_SIZE && page.items[0].has_crc32 &&
                   page.items[0].crc32 == SKIFF_SELFTEST_ROMM_CRC32 &&
                   strcmp(page.items[0].name, "Caf\xC3\xA9") == 0
               ? NULL
               : "a RomM page came back with other values";
}

/* Names from RomM are cleaned byte by byte, and the PSP's char is signed: UTF-8 must still be
 * recognised, and a long name cut between characters, not inside one. */
static const char *check_safe_names(void) {
    char out[SKIFF_STORAGE_NAME_MAX];
    if (skiff_storage_safe_name(SKIFF_SELFTEST_ROMM_NAME, out, sizeof out) != SKIFF_OK ||
        strcmp(out, SKIFF_SELFTEST_SAFE_NAME) != 0) {
        return "a RomM name with accents came out wrong";
    }
    char wide[SKIFF_STORAGE_NAME_INPUT_MAX];
    size_t used = 0;
    for (int i = 0; i < SKIFF_SELFTEST_WIDE_COUNT; i++) {
        memcpy(wide + used, SKIFF_SELFTEST_WIDE_CHARACTER,
               sizeof SKIFF_SELFTEST_WIDE_CHARACTER - 1);
        used += sizeof SKIFF_SELFTEST_WIDE_CHARACTER - 1;
    }
    memcpy(wide + used, SKIFF_SELFTEST_WIDE_EXTENSION, sizeof SKIFF_SELFTEST_WIDE_EXTENSION);
    return skiff_storage_safe_name(wide, out, sizeof out) == SKIFF_OK &&
                   strlen(out) == SKIFF_SELFTEST_WIDE_KEPT_BYTES &&
                   strcmp(out + strlen(out) - strlen(SKIFF_SELFTEST_WIDE_EXTENSION),
                          SKIFF_SELFTEST_WIDE_EXTENSION) == 0
               ? NULL
               : "a long name was not cut between characters";
}

/* The player's text comes from UTF-8 tables and is filled with snprintf'd numbers on newlib. */
static const char *check_spanish_text(void) {
    char line[SKIFF_SELFTEST_TEXT_MAX];
    const skiff_language spanish = skiff_language_from_psp(SKIFF_PSP_SYSTEM_LANGUAGE_SPANISH);
    if (skiff_error_line(spanish, SKIFF_ERR_STORAGE_NO_SPACE, line, sizeof line) != SKIFF_OK ||
        strcmp(line, SKIFF_SELFTEST_SPANISH_ERROR_LINE) != 0) {
        return "a Spanish error line came out wrong";
    }
    char size[SKIFF_SELFTEST_TEXT_MAX];
    return skiff_ui_format_bytes(SKIFF_SELFTEST_UI_BYTES,
                                 skiff_text(spanish, SKIFF_TEXT_DECIMAL_SEPARATOR), size,
                                 sizeof size) == SKIFF_OK &&
                   strcmp(size, SKIFF_SELFTEST_UI_BYTES_TEXT) == 0
               ? NULL
               : "a size with a decimal comma came out wrong";
}

/* One unit per UTF-8 character, as the PSP's char is signed: lead and continuation bytes must
 * still be told apart. */
static float measure_characters(void *ctx, const char *text) {
    (void)ctx;
    float width = 0.0f;
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; c++) {
        if ((*c & 0xC0U) != 0x80U) {
            width += 1.0f;
        }
    }
    return width;
}

static const char *check_ui_text_fitting(void) {
    char out[SKIFF_SELFTEST_TEXT_MAX];
    return skiff_ui_fit_text(SKIFF_SELFTEST_FIT_TEXT, SKIFF_SELFTEST_FIT_WIDTH, measure_characters,
                             NULL, out, sizeof out) == SKIFF_OK &&
                   strcmp(out, SKIFF_SELFTEST_FIT_EXPECTED) == 0
               ? NULL
               : "a title was not cut between characters";
}

/* The download queue's file is written with cJSON's number printing (newlib's snprintf on the
 * PSP) and read back with strtod: a size just under 4 GiB and a 15-digit ROM id must survive. */
static const char *check_jobs_queue(void) {
    static skiff_job written;
    static skiff_job read[SKIFF_JOBS_MAX];
    static char text[SKIFF_SELFTEST_JOBS_TEXT_MAX];
    memset(&written, 0, sizeof written);
    written.id = 1;
    written.rom_id = SKIFF_SELFTEST_JOBS_ROM_ID;
    snprintf(written.title, sizeof written.title, "Caf\xC3\xA9");
    snprintf(written.file_name, sizeof written.file_name, "%s", SKIFF_SELFTEST_ROMM_NAME);
    snprintf(written.target, sizeof written.target, "ms0:/ISO/%s", SKIFF_SELFTEST_SAFE_NAME);
    written.size = SKIFF_STORAGE_MAX_FILE_BYTES;
    written.has_crc32 = 1;
    written.crc32 = SKIFF_SELFTEST_STATE_CRC32;
    written.state = SKIFF_JOB_ACTIVE;
    size_t length = 0;
    size_t count = 0;
    uint32_t next_id = 0;
    size_t dropped = 0;
    if (skiff_jobs_format(&written, 1, 2, text, sizeof text, &length) != SKIFF_OK ||
        skiff_jobs_parse(text, length, read, &count, &next_id, &dropped) != SKIFF_OK ||
        count != 1) {
        return "a queue file did not survive a round trip";
    }
    const skiff_job *got = &read[0];
    return got->id == written.id && got->rom_id == written.rom_id && got->size == written.size &&
                   got->has_crc32 && got->crc32 == written.crc32 && got->state == written.state &&
                   strcmp(got->title, written.title) == 0 &&
                   strcmp(got->file_name, written.file_name) == 0 &&
                   strcmp(got->target, written.target) == 0 && next_id == 2
               ? NULL
               : "a queue file came back with other values";
}

/* installed.json is written and read with cJSON, which prints numbers through newlib's printf as
 * doubles: the largest RomM id and a size near 4 GiB must come back exactly. */
static const char *check_install_manifest(void) {
    skiff_install_manifest *manifest = NULL;
    skiff_install_manifest *parsed = NULL;
    char text[SKIFF_SELFTEST_MANIFEST_TEXT_MAX];
    size_t length = 0;
    const skiff_install_record record = {SKIFF_INSTALL_ROM_ID_MAX,
                                         "Caf\xC3\xA9.iso",
                                         "games:/Caf\xC3\xA9.iso",
                                         SKIFF_STORAGE_MAX_FILE_BYTES,
                                         1,
                                         SKIFF_SELFTEST_STATE_CRC32,
                                         SKIFF_SELFTEST_MANIFEST_MS};
    const char *failure = NULL;
    if (skiff_install_manifest_create(&manifest) != SKIFF_OK ||
        skiff_install_manifest_create(&parsed) != SKIFF_OK ||
        skiff_install_manifest_record(manifest, &record) != SKIFF_OK ||
        skiff_install_manifest_format(manifest, text, sizeof text, &length) != SKIFF_OK ||
        skiff_install_manifest_parse(parsed, text, length) != SKIFF_OK || parsed->count != 1) {
        failure = "installed.json did not survive a round trip";
    } else if (parsed->records[0].rom_id != record.rom_id ||
               parsed->records[0].size != record.size || parsed->records[0].crc32 != record.crc32 ||
               parsed->records[0].installed_ms != record.installed_ms ||
               strcmp(parsed->records[0].file_name, record.file_name) != 0 ||
               strcmp(parsed->records[0].path, record.path) != 0) {
        failure = "installed.json came back with other values";
    }
    skiff_install_manifest_destroy(parsed);
    skiff_install_manifest_destroy(manifest);
    return failure;
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
    {"log-timestamp", check_log_timestamp},
    {"romm-json", check_romm_json},
    {"safe-name", check_safe_names},
    {"spanish-text", check_spanish_text},
    {"ui-fit", check_ui_text_fitting},
    {"jobs-queue", check_jobs_queue},
    {"install-manifest", check_install_manifest},
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
    return skiff_storage_sibling_path(program_path, SKIFF_SELFTEST_RESULT_FILE, out, out_size);
}
