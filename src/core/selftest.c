#include "skiff/selftest.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "skiff/app.h"
#include "skiff/config.h"
#include "skiff/cover.h"
#include "skiff/download.h"
#include "skiff/http.h"
#include "skiff/i18n.h"
#include "skiff/install.h"
#include "skiff/jobs.h"
#include "skiff/log.h"
#include "skiff/param_sfo.h"
#include "skiff/romm.h"
#include "skiff/romm_saves.h"
#include "skiff/saves.h"
#include "skiff/saves_restore.h"
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
/* A RomM page whose numbers need more than 32 bits, and a CRC-32 with a leading zero, listing the
 * ROM's only file (as a list asked with_files gives it). */
#define SKIFF_SELFTEST_ROMM_PAGE                                                                   \
    "{\"items\":[{\"id\":1099511627776,\"platform_id\":1,\"name\":\"Caf\xC3\xA9\",\"fs_name\":"    \
    "\"a.iso\","                                                                                   \
    "\"fs_size_bytes\":4294967295,\"crc_hash\":\"0a1b2c3d\",\"files\":[{\"rom_id\":1099511627776," \
    "\"file_name\":\"Caf\xC3\xA9.iso\",\"file_size_bytes\":4294967295,\"crc_hash\":\"0a1b2c3d\"}]" \
    "}],"                                                                                          \
    "\"total\":1,\"offset\":0}"
/* RomM's collections list: an id past 32 bits, a C1 control character in a name (escaped, so it
 * reaches the parser decoded: the PSP's char is signed), and the favourites left out. */
#define SKIFF_SELFTEST_COLLECTIONS                                                                 \
    "[{\"id\":1,\"name\":\"Favorites\",\"rom_count\":2,\"is_smart\":false,\"is_favorite\":"        \
    "true},{\"id\":1099511627776,\"name\":\"Caf\xC3\xA9\\u0085\",\"rom_count\":4294967295,"        \
    "\"is_smart\":false,\"is_favorite\":false,\"rom_ids\":[1099511627776]}]"
#define SKIFF_SELFTEST_COLLECTION_NAME "Caf\xC3\xA9?"
#define SKIFF_SELFTEST_ROMM_ID 1099511627776ULL
#define SKIFF_SELFTEST_ROMM_SIZE 4294967295ULL
#define SKIFF_SELFTEST_ROMM_CRC32 0x0a1b2c3dU
/* A ROM whose cover path holds a UTF-8 character and the raw space RomM's "?ts=" carries, and the
 * cover URL it must give. */
#define SKIFF_SELFTEST_COVER_BASE "https://romm.lan"
#define SKIFF_SELFTEST_COVER_ROM                                                                   \
    "{\"id\":7,\"platform_id\":1,\"fs_name\":\"a.iso\",\"fs_size_bytes\":1,\"files\":[{\"rom_"     \
    "id\":7,"                                                                                      \
    "\"file_name\":\"a.iso\",\"file_size_bytes\":1}],\"path_cover_small\":"                        \
    "\"/assets/romm/resources/roms/1/7/cover/Caf\xC3\xA9.png?ts=2026-10-10 04:21:31\"}"
#define SKIFF_SELFTEST_COVER_URL                                                                   \
    SKIFF_SELFTEST_COVER_BASE                                                                      \
    "/assets/romm/resources/roms/1/7/cover/Caf%C3%A9.png?ts=2026-10-10%2004:21:31"
/* A 2x2 RGBA PNG: red, green / blue, transparent white. Every byte escaped, 76 in all. */
#define SKIFF_SELFTEST_COVER_PNG                                                                   \
    "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A\x00\x00\x00\x0D\x49\x48\x44\x52\x00\x00\x00\x02\x00\x00"     \
    "\x00\x02\x08\x06\x00\x00\x00\x72\xB6\x0D\x24\x00\x00\x00\x13\x49\x44\x41\x54\x78\xDA\x63"     \
    "\xF8\xCF\xC0\xF0\x1F\x0C\x81\x34\x08\x30\x00\x00\x48\xC9\x08\xF8\xC5\x34\xFD\x05\x00\x00"     \
    "\x00\x00\x49\x45\x4E\x44\xAE\x42\x60\x82"
/* A negotiate plan and a save as RomM writes them: ids and a size past 32 bits, and the uploading
 * device's name with an accent and a C1 control character (U+0085), which must come out as '?'. */
#define SKIFF_SELFTEST_SYNC_PLAN                                                                   \
    "{\"session_id\":4294967297,\"operations\":[{\"action\":\"download\",\"rom_id\":"              \
    "1099511627776,\"save_id\":4294967298,\"file_name\":\"SKIF00001 [2026-10-11_18-15-01].zip\","  \
    "\"slot\":\"autosave\",\"emulator\":\"psp_console\",\"reason\":\"r\",\"server_updated_at\":"   \
    "\"2026-10-11T18:15:01+00:00\",\"server_content_hash\":"                                       \
    "\"969f6b134c73db736eb427afde88aee4\"}]}"
#define SKIFF_SELFTEST_SAVE                                                                        \
    "{\"id\":4294967298,\"rom_id\":1099511627776,\"file_name\":\"SKIF00001.zip\",\"slot\":"        \
    "\"autosave\",\"emulator\":\"psp_console\",\"content_hash\":"                                  \
    "\"969f6b134c73db736eb427afde88aee4\",\"updated_at\":\"2026-10-11T18:15:01+00:00\","           \
    "\"file_size_bytes\":4294967295,\"origin_device_id\":\"b\",\"device_syncs\":[{\"device_"       \
    "id\":\"b\",\"device_name\":\"Caf\xC3\xA9\xC2\x85PSP\"}]}"
#define SKIFF_SELFTEST_SESSION_ID 4294967297ULL
#define SKIFF_SELFTEST_SAVE_ID 4294967298ULL
#define SKIFF_SELFTEST_SAVE_DEVICE "Caf\xC3\xA9?PSP"
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
/* Keyboard text in each UTF-8 length: "Café 日本" and U+1F3AE, a surrogate pair in UTF-16. */
#define SKIFF_SELFTEST_UTF16_TEXT "Caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xAE"
#define SKIFF_SELFTEST_UTF16_UNITS 10U
#define SKIFF_SELFTEST_UTF16_HIGH_SURROGATE 0xD83CU
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
/* What the storage-list check puts in its scratch folder: a file and a folder. */
#define SKIFF_SELFTEST_SCRATCH_FILE_NAME "file.bin"
#define SKIFF_SELFTEST_SCRATCH_SUBFOLDER_NAME "folder"
#define SKIFF_SELFTEST_SCRATCH_TEXT "skiff selftest"

/* The saves-unit check: the live probe's two save folders of the made-up game SKIF00001
 * (tests/fixtures/saves/make_units.py), whose unit RomM 5.4.0 hashed to SKIFF_SELFTEST_SAVES_HASH,
 * under SKIFF_SELFTEST_SAVES_FOLDER. */
#define SKIFF_SELFTEST_SAVES_TARGET "SKIF00001"
#define SKIFF_SELFTEST_SAVES_DATA00 SKIFF_SELFTEST_SAVES_TARGET "DATA00"
#define SKIFF_SELFTEST_SAVES_DATA01 SKIFF_SELFTEST_SAVES_TARGET "DATA01"
#define SKIFF_SELFTEST_SAVES_ROOT SKIFF_SELFTEST_SAVES_FOLDER "/saves"
#define SKIFF_SELFTEST_SAVES_UNIT SKIFF_SELFTEST_SAVES_FOLDER "/unit.zip"
#define SKIFF_SELFTEST_SAVES_STAGING SKIFF_SELFTEST_SAVES_FOLDER "/stage"
#define SKIFF_SELFTEST_SAVES_HASH "f0c9413915f1a6c3df66129ee6bc744b"
#define SKIFF_SELFTEST_SAVES_SFO "\0PSF" SKIFF_SELFTEST_SAVES_TARGET " synthetic"
#define SKIFF_SELFTEST_SAVES_SLOT_TEXT "slot one"
#define SKIFF_SELFTEST_SAVES_DATA_BYTES 4096U
#define SKIFF_SELFTEST_SAVES_FILLER 'A'
#define SKIFF_SELFTEST_SAVES_FOLDERS 2U
#define SKIFF_SELFTEST_SAVES_FILES 4U
/* The saves-restore check: a live save and the unit restored over it, in scratch app: and saves:
 * folders under SKIFF_SELFTEST_RESTORE_FOLDER. */
#define SKIFF_SELFTEST_RESTORE_OLD "old slot"
#define SKIFF_SELFTEST_RESTORE_NEW "new slot, restored"
#define SKIFF_SELFTEST_RESTORE_SOURCE SKIFF_STORAGE_ROOT_APP "/source"
#define SKIFF_SELFTEST_RESTORE_LIVE                                                                \
    SKIFF_STORAGE_ROOT_SAVES "/" SKIFF_SELFTEST_SAVES_DATA00 "/DATA.BIN"
/* A name with a byte above 0x7F, which a signed char (the PSP's) must not let through. */
#define SKIFF_SELFTEST_SAVES_HIGH_NAME "CAF\xC9.BIN"
/* A PARAM.SFO of two keys: TITLE (text "Skiff") and LEVEL (the number 0x01020304), little-endian
 * as the PSP writes it, so a reader with the wrong byte order gets 0x04030201. */
#define SKIFF_SELFTEST_SFO_TITLE "Skiff"
#define SKIFF_SELFTEST_SFO_LEVEL 0x01020304U
static const unsigned char SELFTEST_SFO[] = {
    0x00, 'P',  'S',  'F',  0x01, 0x01, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x02, 0x06, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x04, 0x04, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 'T',  'I',  'T',  'L',  'E',  0x00, 'L',  'E',  'V',  'E',  'L',  0x00,
    'S',  'k',  'i',  'f',  'f',  0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01};

/* The storage the current run was given, for the checks that touch the device. */
static const skiff_selftest_env *run_env;

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
    static const char collections_text[] = SKIFF_SELFTEST_COLLECTIONS;
    static skiff_romm_rom_page page;
    static skiff_romm_collection_list collections;
    if (skiff_romm_parse_rom_page(page_text, sizeof page_text - 1, &page) != SKIFF_OK ||
        page.count != 1) {
        return "a RomM page did not parse";
    }
    collections.count = 0;
    if (skiff_romm_parse_collections(collections_text, sizeof collections_text - 1, 0,
                                     &collections) != SKIFF_OK ||
        collections.count != 1 || collections.items[0].id != SKIFF_SELFTEST_ROMM_ID ||
        collections.items[0].rom_count != SKIFF_SELFTEST_ROMM_SIZE ||
        strcmp(collections.items[0].name, SKIFF_SELFTEST_COLLECTION_NAME) != 0) {
        return "a RomM collections list came back with other values";
    }
    return page.items[0].id == SKIFF_SELFTEST_ROMM_ID &&
                   page.items[0].size == SKIFF_SELFTEST_ROMM_SIZE && page.items[0].has_crc32 &&
                   page.items[0].crc32 == SKIFF_SELFTEST_ROMM_CRC32 &&
                   strcmp(page.items[0].name, "Caf\xC3\xA9") == 0 && page.items[0].has_file &&
                   page.items[0].file.size == SKIFF_SELFTEST_ROMM_SIZE &&
                   page.items[0].file.crc32 == SKIFF_SELFTEST_ROMM_CRC32 &&
                   strcmp(page.items[0].file.file_name, "Caf\xC3\xA9.iso") == 0
               ? NULL
               : "a RomM page came back with other values";
}

/* A cover URL is percent-encoded byte by byte, and the PSP's char is signed: each byte of a UTF-8
 * character must become its own "%XX". */
static const char *check_romm_cover(void) {
    static const char rom_text[] = SKIFF_SELFTEST_COVER_ROM;
    static skiff_romm_rom rom;
    if (skiff_romm_parse_rom(rom_text, sizeof rom_text - 1, &rom) != SKIFF_OK ||
        rom.cover_path[0] == '\0') {
        return "a RomM cover path did not parse";
    }
    /* Building a URL sends nothing, so the transport is never used. */
    skiff_transport unused = {NULL};
    skiff_romm_client client;
    char url[SKIFF_ROMM_COVER_URL_MAX];
    return skiff_romm_client_init(&client, &unused, SKIFF_SELFTEST_COVER_BASE, NULL) == SKIFF_OK &&
                   skiff_romm_cover_url(&client, rom.cover_path, url, sizeof url) == SKIFF_OK &&
                   strcmp(url, SKIFF_SELFTEST_COVER_URL) == 0
               ? NULL
               : "a cover URL came out wrong";
}

/* Save sync's answers are parsed as ROM pages are, on newlib with 64-bit integers in software and
 * a signed char: ids past 32 bits must come out exact, and a C1 control character in a device name
 * must be caught. */
static const char *check_romm_saves(void) {
    static const char plan_text[] = SKIFF_SELFTEST_SYNC_PLAN;
    static const char save_text[] = SKIFF_SELFTEST_SAVE;
    static skiff_romm_sync_plan plan;
    static skiff_romm_save save;
    if (skiff_romm_parse_sync_plan(plan_text, sizeof plan_text - 1, &plan) != SKIFF_OK ||
        skiff_romm_parse_save(save_text, sizeof save_text - 1, &save) != SKIFF_OK ||
        plan.count != 1) {
        return "a save sync answer did not parse";
    }
    const skiff_romm_sync_operation *op = &plan.operations[0];
    return plan.session_id == SKIFF_SELFTEST_SESSION_ID && op->action == SKIFF_ROMM_SYNC_DOWNLOAD &&
                   op->rom_id == SKIFF_SELFTEST_ROMM_ID && op->save_id == SKIFF_SELFTEST_SAVE_ID &&
                   save.id == SKIFF_SELFTEST_SAVE_ID && save.rom_id == SKIFF_SELFTEST_ROMM_ID &&
                   save.file_size_bytes == SKIFF_SELFTEST_ROMM_SIZE &&
                   strcmp(op->server_content_hash, save.content_hash) == 0 &&
                   strcmp(save.origin_device_name, SKIFF_SELFTEST_SAVE_DEVICE) == 0
               ? NULL
               : "a save sync answer came back with other values";
}

/* libpng and zlib as the EBOOT links them, and RGB565 written as the GE reads it: red in the low
 * bits whatever the CPU's byte order. */
static const char *check_cover_decode(void) {
    static const unsigned char png[] = SKIFF_SELFTEST_COVER_PNG;
    skiff_cover *cover = malloc(sizeof *cover);
    if (cover == NULL) {
        return "no memory for a cover";
    }
    const uint16_t expected[] = {skiff_cover_rgb565(0xFF, 0, 0), skiff_cover_rgb565(0, 0xFF, 0),
                                 skiff_cover_rgb565(0, 0, 0xFF),
                                 skiff_cover_rgb565(SKIFF_COVER_BACKGROUND_RED,
                                                    SKIFF_COVER_BACKGROUND_GREEN,
                                                    SKIFF_COVER_BACKGROUND_BLUE)};
    const int ok = skiff_cover_decode_png(png, sizeof png - 1, cover) == SKIFF_OK &&
                   cover->width == 2 && cover->height == 2 && cover->pixels[0] == expected[0] &&
                   cover->pixels[1] == expected[1] &&
                   cover->pixels[SKIFF_COVER_WIDTH] == expected[2] &&
                   cover->pixels[SKIFF_COVER_WIDTH + 1] == expected[3] && expected[0] == 0x001F;
    free(cover);
    return ok ? NULL : "a PNG cover decoded wrong";
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

/* The system dialogs' text: UTF-8 to UTF-16 and back, which on the PSP's signed char must still
 * tell lead, continuation and four-byte bytes apart. */
static const char *check_ui_utf16(void) {
    uint16_t units[SKIFF_SELFTEST_TEXT_MAX];
    size_t length = 0;
    char back[SKIFF_SELFTEST_TEXT_MAX];
    if (skiff_ui_utf8_to_utf16(SKIFF_SELFTEST_UTF16_TEXT, units, SKIFF_SELFTEST_TEXT_MAX,
                               &length) != SKIFF_OK ||
        length != SKIFF_SELFTEST_UTF16_UNITS ||
        units[length - 2] != SKIFF_SELFTEST_UTF16_HIGH_SURROGATE) {
        return "UTF-8 came out wrong as UTF-16";
    }
    return skiff_ui_utf16_to_utf8(units, length, back, sizeof back) == SKIFF_OK &&
                   strcmp(back, SKIFF_SELFTEST_UTF16_TEXT) == 0
               ? NULL
               : "UTF-16 came back as other UTF-8";
}

/* The app wraps sentences between words with the same measure: on the PSP's signed char a line
 * must still never end inside "é" or "ñ". */
static const char *check_app_wrap(void) {
    char lines[4][SKIFF_TEXT_MAX];
    const size_t count = skiff_app_wrap("A\xC3\xB1"
                                        "adido a las descargas: caf\xC3\xA9",
                                        12.0f, measure_characters, NULL, lines, 4);
    return count == 4 &&
                   strcmp(lines[0], "A\xC3\xB1"
                                    "adido a") == 0 &&
                   strcmp(lines[3], "caf\xC3\xA9") == 0
               ? NULL
               : "a sentence was not wrapped between words";
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

typedef struct scratch_listing {
    int files;
    int folders;
    int others;
    skiff_storage_info file;
} scratch_listing;

/* FAT ignores case, and PPSSPP reports a name that fits 8.3 in capitals ("FILE.BIN"). */
static skiff_storage_list_step note_scratch_entry(void *ctx, const skiff_storage_entry *entry) {
    scratch_listing *seen = ctx;
    if (!entry->info.is_dir && strcasecmp(entry->name, SKIFF_SELFTEST_SCRATCH_FILE_NAME) == 0) {
        seen->files++;
        seen->file = entry->info;
    } else if (entry->info.is_dir &&
               strcasecmp(entry->name, SKIFF_SELFTEST_SCRATCH_SUBFOLDER_NAME) == 0) {
        seen->folders++;
    } else {
        seen->others++;
    }
    return SKIFF_STORAGE_LIST_CONTINUE;
}

/* A failed storage step and the error it returned, for a hardware run's result.txt. */
static const char *storage_failure(const char *step, skiff_err err) {
    static char reason[SKIFF_SELFTEST_LINE_MAX];
    snprintf(reason, sizeof reason, "%s (%s)", step, skiff_err_name(err));
    return reason;
}

static const char *list_scratch(skiff_storage *storage, const char *folder, const char *file,
                                const char *subfolder) {
    static const char text[] = SKIFF_SELFTEST_SCRATCH_TEXT;
    skiff_err err = skiff_storage_mkdir(storage, folder);
    if (err != SKIFF_OK) {
        return storage_failure("could not create a scratch folder under app:", err);
    }
    err = skiff_storage_mkdir(storage, subfolder);
    if (err == SKIFF_OK) {
        err = skiff_storage_replace_whole(storage, file, text, sizeof text - 1);
    }
    if (err != SKIFF_OK) {
        return storage_failure("could not fill the scratch folder", err);
    }
    scratch_listing seen;
    memset(&seen, 0, sizeof seen);
    size_t skipped = 0;
    err = skiff_storage_list(storage, folder, note_scratch_entry, &seen, &skipped);
    if (err != SKIFF_OK || seen.files != 1 || seen.folders != 1 || seen.others != 0 ||
        skipped != 0) {
        return storage_failure("the scratch folder listed other entries than its file and folder",
                               err);
    }
    skiff_storage_info info;
    err = skiff_storage_stat(storage, file, &info);
    if (err != SKIFF_OK || seen.file.size != sizeof text - 1 ||
        seen.file.mtime == SKIFF_STORAGE_MTIME_UNKNOWN || info.is_dir ||
        info.size != seen.file.size || info.mtime != seen.file.mtime) {
        return storage_failure("a file's size or date disagreed between the listing and stat", err);
    }
    err = skiff_storage_stat(storage, subfolder, &info);
    if (err != SKIFF_OK || !info.is_dir) {
        return storage_failure("stat did not see a folder", err);
    }
    err = skiff_storage_rmdir(storage, folder);
    if (err != SKIFF_ERR_STORAGE_IO) {
        return storage_failure("a folder with files in it was not refused", err);
    }
    err = skiff_storage_rmdir(storage, subfolder);
    if (err == SKIFF_OK) {
        err = skiff_storage_stat(storage, subfolder, &info) == SKIFF_ERR_STORAGE_NOT_FOUND
                  ? SKIFF_OK
                  : SKIFF_ERR_STORAGE_IO;
    }
    if (err != SKIFF_OK) {
        return storage_failure("an empty folder was not removed", err);
    }
    return NULL;
}

/* Save sync lists PSP/SAVEDATA with sceIoDopen/sceIoDread, compares file dates as the firmware
 * stamps them, and removes emptied folders: on a scratch folder under app:, since the self-test
 * never touches saves. */
static const char *check_storage_list(void) {
    if (run_env == NULL || run_env->storage == NULL || run_env->roots == NULL) {
        return "no storage to check";
    }
    skiff_storage *storage = run_env->storage;
    char folder[SKIFF_STORAGE_PATH_MAX];
    char file[SKIFF_STORAGE_PATH_MAX];
    char subfolder[SKIFF_STORAGE_PATH_MAX];
    if (skiff_storage_resolve(run_env->roots, SKIFF_SELFTEST_SCRATCH_FOLDER, folder,
                              sizeof folder) != SKIFF_OK ||
        skiff_storage_resolve(run_env->roots,
                              SKIFF_SELFTEST_SCRATCH_FOLDER "/" SKIFF_SELFTEST_SCRATCH_FILE_NAME,
                              file, sizeof file) != SKIFF_OK ||
        skiff_storage_resolve(
            run_env->roots, SKIFF_SELFTEST_SCRATCH_FOLDER "/" SKIFF_SELFTEST_SCRATCH_SUBFOLDER_NAME,
            subfolder, sizeof subfolder) != SKIFF_OK) {
        return "the scratch folder does not fit under app:";
    }
    const char *failure = list_scratch(storage, folder, file, subfolder);
    /* Whatever a failed step left goes too, so the next run starts clean. */
    (void)skiff_storage_remove(storage, file);
    (void)skiff_storage_rmdir(storage, subfolder);
    const skiff_err removed = skiff_storage_rmdir(storage, folder);
    if (removed != SKIFF_OK && failure == NULL) {
        failure = storage_failure("the emptied scratch folder was not removed", removed);
    }
    return failure;
}

/* The saves-unit check's files and folders, relative to SKIFF_SELFTEST_SAVES_FOLDER: created in
 * this order and removed in the reverse one. NULL text is the 4 KiB DATA.BIN. */
typedef struct selftest_save_path {
    const char *path;
    int is_folder;
    const char *text;
    size_t text_bytes;
} selftest_save_path;

#define SAVES_TEXT(text) text, sizeof text - 1
static const selftest_save_path SELFTEST_SAVE_TREE[] = {
    {"", 1, NULL, 0},
    {"/saves", 1, NULL, 0},
    {"/saves/" SKIFF_SELFTEST_SAVES_DATA00, 1, NULL, 0},
    {"/saves/" SKIFF_SELFTEST_SAVES_DATA00 "/PARAM.SFO", 0, SAVES_TEXT(SKIFF_SELFTEST_SAVES_SFO)},
    {"/saves/" SKIFF_SELFTEST_SAVES_DATA00 "/DATA.BIN", 0, NULL, 0},
    {"/saves/" SKIFF_SELFTEST_SAVES_DATA01, 1, NULL, 0},
    {"/saves/" SKIFF_SELFTEST_SAVES_DATA01 "/PARAM.SFO", 0, SAVES_TEXT(SKIFF_SELFTEST_SAVES_SFO)},
    {"/saves/" SKIFF_SELFTEST_SAVES_DATA01 "/DATA.BIN", 0,
     SAVES_TEXT(SKIFF_SELFTEST_SAVES_SLOT_TEXT)},
};

/* What the check writes besides the tree, removed with it. */
static const char *const SELFTEST_SAVE_OUTPUTS[] = {
    "/stage/" SKIFF_SELFTEST_SAVES_DATA00 "/PARAM.SFO",
    "/stage/" SKIFF_SELFTEST_SAVES_DATA00 "/DATA.BIN",
    "/stage/" SKIFF_SELFTEST_SAVES_DATA01 "/PARAM.SFO",
    "/stage/" SKIFF_SELFTEST_SAVES_DATA01 "/DATA.BIN",
    "/stage/" SKIFF_SELFTEST_SAVES_DATA00,
    "/stage/" SKIFF_SELFTEST_SAVES_DATA01,
    "/stage",
    "/unit.zip",
};

typedef struct selftest_saves {
    skiff_saves_unit *unit;
    unsigned char *data;
    char path[SKIFF_STORAGE_PATH_MAX];
    char logical[SKIFF_STORAGE_PATH_MAX];
} selftest_saves;

static skiff_err saves_path(selftest_saves *s, const char *relative) {
    snprintf(s->logical, sizeof s->logical, "%s%s", SKIFF_SELFTEST_SAVES_FOLDER, relative);
    return skiff_storage_resolve(run_env->roots, s->logical, s->path, sizeof s->path);
}

static skiff_err make_save_tree(selftest_saves *s) {
    skiff_err err = SKIFF_OK;
    for (size_t i = 0;
         err == SKIFF_OK && i < sizeof SELFTEST_SAVE_TREE / sizeof SELFTEST_SAVE_TREE[0]; i++) {
        const selftest_save_path *entry = &SELFTEST_SAVE_TREE[i];
        err = saves_path(s, entry->path);
        if (err == SKIFF_OK && entry->is_folder) {
            err = skiff_storage_mkdir(run_env->storage, s->path);
        } else if (err == SKIFF_OK) {
            err = entry->text != NULL
                      ? skiff_storage_replace_whole(run_env->storage, s->path, entry->text,
                                                    entry->text_bytes)
                      : skiff_storage_replace_whole(run_env->storage, s->path, s->data,
                                                    SKIFF_SELFTEST_SAVES_DATA_BYTES);
        }
    }
    return err;
}

/* Everything the check may have made, files before their folders; missing ones are fine. */
static skiff_err remove_save_tree(selftest_saves *s) {
    for (size_t i = 0; i < sizeof SELFTEST_SAVE_OUTPUTS / sizeof SELFTEST_SAVE_OUTPUTS[0]; i++) {
        if (saves_path(s, SELFTEST_SAVE_OUTPUTS[i]) == SKIFF_OK) {
            (void)skiff_storage_remove(run_env->storage, s->path);
            (void)skiff_storage_rmdir(run_env->storage, s->path);
        }
    }
    skiff_err err = SKIFF_OK;
    for (size_t i = sizeof SELFTEST_SAVE_TREE / sizeof SELFTEST_SAVE_TREE[0]; i-- > 0;) {
        if (saves_path(s, SELFTEST_SAVE_TREE[i].path) != SKIFF_OK) {
            continue;
        }
        err = SELFTEST_SAVE_TREE[i].is_folder ? skiff_storage_rmdir(run_env->storage, s->path)
                                              : skiff_storage_remove(run_env->storage, s->path);
    }
    /* Only the last removal, the folder itself, decides: the rest may not exist. */
    return err == SKIFF_ERR_STORAGE_NOT_FOUND ? SKIFF_OK : err;
}

static const char *check_save_round_trip(selftest_saves *s) {
    skiff_storage *storage = run_env->storage;
    skiff_err err = make_save_tree(s);
    if (err != SKIFF_OK) {
        return storage_failure("could not write the save folders under app:", err);
    }
    err = skiff_saves_collect(storage, run_env->roots, SKIFF_SELFTEST_SAVES_ROOT,
                              SKIFF_SELFTEST_SAVES_TARGET, s->unit);
    if (err != SKIFF_OK || s->unit->folder_count != SKIFF_SELFTEST_SAVES_FOLDERS ||
        s->unit->file_count != SKIFF_SELFTEST_SAVES_FILES) {
        return storage_failure("the game's save folders were not collected", err);
    }
    err = skiff_saves_write_unit(storage, run_env->roots, s->unit, SKIFF_SELFTEST_SAVES_UNIT);
    if (err != SKIFF_OK || strcmp(s->unit->content_hash, SKIFF_SELFTEST_SAVES_HASH) != 0) {
        return storage_failure("the unit was not written with RomM's content_hash", err);
    }
    err = skiff_saves_read_unit(storage, run_env->roots, SKIFF_SELFTEST_SAVES_UNIT,
                                SKIFF_SELFTEST_SAVES_TARGET, SKIFF_SELFTEST_SAVES_STAGING, s->unit);
    if (err != SKIFF_OK || strcmp(s->unit->content_hash, SKIFF_SELFTEST_SAVES_HASH) != 0 ||
        s->unit->file_count != SKIFF_SELFTEST_SAVES_FILES) {
        return storage_failure("the unit did not read back the same", err);
    }
    size_t length = 0;
    err = saves_path(s, "/stage/" SKIFF_SELFTEST_SAVES_DATA00 "/DATA.BIN");
    if (err == SKIFF_OK) {
        err = skiff_storage_read_whole(storage, s->path, s->data, SKIFF_SELFTEST_SAVES_DATA_BYTES,
                                       &length);
    }
    for (size_t i = 0; err == SKIFF_OK && i < length; i++) {
        err = s->data[i] == SKIFF_SELFTEST_SAVES_FILLER ? SKIFF_OK : SKIFF_ERR_STORAGE_IO;
    }
    if (err != SKIFF_OK || length != SKIFF_SELFTEST_SAVES_DATA_BYTES) {
        return storage_failure("an extracted save file differs", err);
    }
    return NULL;
}

/* Save units: PARAM.SFO's little-endian fields, names checked byte by byte with the PSP's signed
 * char, then a unit written from save folders under app:, read back, extracted and hashed with
 * Skiff's own MD5 and zlib's CRC-32 and inflate. */
static const char *check_saves_unit(void) {
    skiff_sfo sfo;
    char title[sizeof SKIFF_SELFTEST_SFO_TITLE];
    uint32_t level = 0;
    if (skiff_sfo_parse(SELFTEST_SFO, sizeof SELFTEST_SFO, &sfo) != SKIFF_OK ||
        skiff_sfo_text(&sfo, "TITLE", title, sizeof title) != SKIFF_OK ||
        strcmp(title, SKIFF_SELFTEST_SFO_TITLE) != 0 ||
        skiff_sfo_number(&sfo, "LEVEL", &level) != SKIFF_OK || level != SKIFF_SELFTEST_SFO_LEVEL ||
        skiff_sfo_is_save(&sfo)) {
        return "PARAM.SFO read back other values";
    }
    if (skiff_saves_name_valid(SKIFF_SELFTEST_SAVES_HIGH_NAME) ||
        !skiff_saves_target_valid(SKIFF_SELFTEST_SAVES_TARGET)) {
        return "a save name above ASCII was accepted";
    }
    if (run_env == NULL || run_env->storage == NULL || run_env->roots == NULL) {
        return "no storage to check";
    }
    selftest_saves *s = calloc(1, sizeof *s);
    if (s == NULL) {
        return "out of memory";
    }
    s->data = malloc(SKIFF_SELFTEST_SAVES_DATA_BYTES);
    const char *failure = NULL;
    if (s->data == NULL || skiff_saves_unit_create(&s->unit) != SKIFF_OK) {
        failure = "out of memory";
    } else if (saves_path(s, SELFTEST_SAVE_OUTPUTS[0]) != SKIFF_OK) {
        failure = "the save folders do not fit under app:";
    } else {
        memset(s->data, SKIFF_SELFTEST_SAVES_FILLER, SKIFF_SELFTEST_SAVES_DATA_BYTES);
        failure = check_save_round_trip(s);
        const skiff_err removed = remove_save_tree(s);
        if (removed != SKIFF_OK && failure == NULL) {
            failure = storage_failure("the save folders were not removed", removed);
        }
    }
    skiff_saves_unit_destroy(s->unit);
    free(s->data);
    free(s);
    return failure;
}

/* The saves-restore check's files and folders, relative to SKIFF_SELFTEST_RESTORE_FOLDER: created
 * in this order and removed in the reverse one. */
typedef struct selftest_restore_path {
    const char *path;
    const char *text;
} selftest_restore_path;

static const selftest_restore_path SELFTEST_RESTORE_TREE[] = {
    {"", NULL},
    {"/app", NULL},
    {"/saves", NULL},
    {"/saves/" SKIFF_SELFTEST_SAVES_DATA00, NULL},
    {"/saves/" SKIFF_SELFTEST_SAVES_DATA00 "/DATA.BIN", SKIFF_SELFTEST_RESTORE_OLD},
    {"/app/source", NULL},
    {"/app/source/" SKIFF_SELFTEST_SAVES_DATA00, NULL},
    {"/app/source/" SKIFF_SELFTEST_SAVES_DATA00 "/DATA.BIN", SKIFF_SELFTEST_RESTORE_NEW},
    {"/app/saves-work", NULL},
};

/* What the restore writes (or a failed one may leave) besides the tree, files before folders. */
static const char *const SELFTEST_RESTORE_OUTPUTS[] = {
    "/app/saves-work/" SKIFF_SELFTEST_SAVES_TARGET ".zip",
    "/app/saves-work/stage/" SKIFF_SELFTEST_SAVES_DATA00 "/DATA.BIN",
    "/app/saves-work/stage/" SKIFF_SELFTEST_SAVES_DATA00,
    "/app/saves-work/stage",
    "/app/saves-work/journal.tmp",
    "/app/saves-work/journal.new",
    "/app/saves-work/journal",
    "/app/saves-backup/" SKIFF_SELFTEST_SAVES_TARGET "/1.zip.tmp",
    "/app/saves-backup/" SKIFF_SELFTEST_SAVES_TARGET "/1.zip",
    "/app/saves-backup/" SKIFF_SELFTEST_SAVES_TARGET,
    "/app/saves-backup",
};

typedef struct selftest_restore {
    skiff_storage_roots roots;
    skiff_saves_unit *unit;
    char path[SKIFF_STORAGE_PATH_MAX];
    char logical[SKIFF_STORAGE_PATH_MAX];
    char text[sizeof SKIFF_SELFTEST_RESTORE_NEW];
} selftest_restore;

/* A path under SKIFF_SELFTEST_RESTORE_FOLDER, resolved with the device's own roots. */
static skiff_err restore_path(selftest_restore *s, const char *relative) {
    snprintf(s->logical, sizeof s->logical, "%s%s", SKIFF_SELFTEST_RESTORE_FOLDER, relative);
    return skiff_storage_resolve(run_env->roots, s->logical, s->path, sizeof s->path);
}

static skiff_err make_restore_tree(selftest_restore *s) {
    skiff_err err = SKIFF_OK;
    for (size_t i = 0;
         err == SKIFF_OK && i < sizeof SELFTEST_RESTORE_TREE / sizeof SELFTEST_RESTORE_TREE[0];
         i++) {
        const selftest_restore_path *entry = &SELFTEST_RESTORE_TREE[i];
        err = restore_path(s, entry->path);
        if (err == SKIFF_OK) {
            err = entry->text == NULL
                      ? skiff_storage_mkdir(run_env->storage, s->path)
                      : skiff_storage_replace_whole(run_env->storage, s->path, entry->text,
                                                    strlen(entry->text));
        }
    }
    return err;
}

static skiff_err remove_restore_tree(selftest_restore *s) {
    for (size_t i = 0; i < sizeof SELFTEST_RESTORE_OUTPUTS / sizeof SELFTEST_RESTORE_OUTPUTS[0];
         i++) {
        if (restore_path(s, SELFTEST_RESTORE_OUTPUTS[i]) == SKIFF_OK) {
            (void)skiff_storage_remove(run_env->storage, s->path);
            (void)skiff_storage_rmdir(run_env->storage, s->path);
        }
    }
    skiff_err err = SKIFF_OK;
    for (size_t i = sizeof SELFTEST_RESTORE_TREE / sizeof SELFTEST_RESTORE_TREE[0]; i-- > 0;) {
        if (restore_path(s, SELFTEST_RESTORE_TREE[i].path) != SKIFF_OK) {
            continue;
        }
        err = SELFTEST_RESTORE_TREE[i].text == NULL
                  ? skiff_storage_rmdir(run_env->storage, s->path)
                  : skiff_storage_remove(run_env->storage, s->path);
    }
    /* Only the last removal, the folder itself, decides: the rest may not exist. */
    return err == SKIFF_ERR_STORAGE_NOT_FOUND ? SKIFF_OK : err;
}

static const char *check_restore_steps(selftest_restore *s) {
    skiff_storage *storage = run_env->storage;
    skiff_err err = make_restore_tree(s);
    if (err != SKIFF_OK) {
        return storage_failure("could not write the scratch save folders under app:", err);
    }
    char unit_path[SKIFF_STORAGE_PATH_MAX];
    err = skiff_saves_work_unit_path(SKIFF_SELFTEST_SAVES_TARGET, unit_path, sizeof unit_path);
    if (err == SKIFF_OK) {
        err = skiff_saves_collect(storage, &s->roots, SKIFF_SELFTEST_RESTORE_SOURCE,
                                  SKIFF_SELFTEST_SAVES_TARGET, s->unit);
    }
    if (err == SKIFF_OK) {
        err = skiff_saves_write_unit(storage, &s->roots, s->unit, unit_path);
    }
    if (err != SKIFF_OK) {
        return storage_failure("the unit to restore was not written", err);
    }
    skiff_saves_restore_report report;
    err = skiff_saves_restore(storage, &s->roots, unit_path, SKIFF_SELFTEST_SAVES_TARGET, &report);
    if (err != SKIFF_OK || report.backup != 1 || report.folders_replaced != 1) {
        return storage_failure("the unit was not restored over the live save", err);
    }
    size_t length = 0;
    err = skiff_storage_resolve(&s->roots, SKIFF_SELFTEST_RESTORE_LIVE, s->path, sizeof s->path);
    if (err == SKIFF_OK) {
        err = skiff_storage_read_whole(storage, s->path, s->text, sizeof s->text - 1, &length);
    }
    s->text[err == SKIFF_OK ? length : 0] = '\0';
    if (err != SKIFF_OK || strcmp(s->text, SKIFF_SELFTEST_RESTORE_NEW) != 0) {
        return storage_failure("the restored save file differs", err);
    }
    skiff_saves_backup_list backups;
    err = skiff_saves_backups_list(storage, &s->roots, SKIFF_SELFTEST_SAVES_TARGET, &backups);
    if (err != SKIFF_OK || backups.count != 1 || backups.backups[0].number != 1) {
        return storage_failure("the live save was not backed up", err);
    }
    skiff_saves_recovery recovery;
    err = skiff_saves_recover(storage, &s->roots, &recovery);
    if (err != SKIFF_OK || recovery.outcome != SKIFF_SAVES_RECOVERY_NONE) {
        return storage_failure("a recovery with nothing pending did something", err);
    }
    return NULL;
}

/* The device's roots with app: and saves: moved to scratch folders, checking first that the
 * deepest path the check makes fits. */
static skiff_err scratch_roots(selftest_restore *s) {
    s->roots = *run_env->roots;
    skiff_err err = restore_path(s, SELFTEST_RESTORE_OUTPUTS[1]);
    if (err == SKIFF_OK) {
        err = restore_path(s, "/app");
    }
    if (err == SKIFF_OK) {
        memcpy(s->roots.app, s->path, sizeof s->roots.app);
        err = restore_path(s, "/saves");
    }
    if (err == SKIFF_OK) {
        memcpy(s->roots.saves, s->path, sizeof s->roots.saves);
    }
    return err;
}

/* A restore staged, journalled and copied with the PSP's own folder listing, rmdir and sync, then
 * a recovery with nothing pending: on scratch app: and saves: folders, since the self-test never
 * touches the real saves. */
static const char *check_saves_restore(void) {
    if (run_env == NULL || run_env->storage == NULL || run_env->roots == NULL) {
        return "no storage to check";
    }
    selftest_restore *s = calloc(1, sizeof *s);
    if (s == NULL) {
        return "out of memory";
    }
    const char *failure = NULL;
    if (skiff_saves_unit_create(&s->unit) != SKIFF_OK) {
        failure = "out of memory";
    } else if (scratch_roots(s) != SKIFF_OK) {
        failure = "the restore folders do not fit under app:";
    } else {
        failure = check_restore_steps(s);
        const skiff_err removed = remove_restore_tree(s);
        if (removed != SKIFF_OK && failure == NULL) {
            failure = storage_failure("the restore folders were not removed", removed);
        }
    }
    skiff_saves_unit_destroy(s->unit);
    free(s);
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
    {"romm-cover", check_romm_cover},
    {"romm-saves", check_romm_saves},
    {"cover-decode", check_cover_decode},
    {"safe-name", check_safe_names},
    {"spanish-text", check_spanish_text},
    {"ui-fit", check_ui_text_fitting},
    {"ui-utf16", check_ui_utf16},
    {"app-wrap", check_app_wrap},
    {"jobs-queue", check_jobs_queue},
    {"install-manifest", check_install_manifest},
    {"storage-list", check_storage_list},
    {"saves-unit", check_saves_unit},
    {"saves-restore", check_saves_restore},
};
// clang-format on

static void emit(skiff_selftest_log_fn log, void *ctx, const char *line) {
    if (log != NULL) {
        log(ctx, line);
    }
}

skiff_selftest_result skiff_selftest_run(const skiff_selftest_env *env, skiff_selftest_log_fn log,
                                         void *ctx) {
    skiff_selftest_result result = {0, 0};
    char line[SKIFF_SELFTEST_LINE_MAX];
    run_env = env;

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
    run_env = NULL;

    snprintf(line, sizeof line, "%s %d/%d",
             result.failed == 0 ? SKIFF_SELFTEST_OK_MARKER : SKIFF_SELFTEST_FAIL_MARKER,
             result.passed, result.passed + result.failed);
    emit(log, ctx, line);
    return result;
}

skiff_err skiff_selftest_result_path(const char *program_path, char *out, size_t out_size) {
    return skiff_storage_sibling_path(program_path, SKIFF_SELFTEST_RESULT_FILE, out, out_size);
}
