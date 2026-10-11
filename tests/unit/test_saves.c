/*
 * Save units (skiff/saves.h) over a temporary directory laid out as a Memory Stick: which folders
 * a game's collect takes (prefix ignoring case, installs left out, folders without a readable
 * PARAM.SFO kept) and what makes it refuse; the writer against argosy-sigil's unit byte for byte
 * (tests/fixtures/saves/, made by make_units.py) and RomM's content_hash; reading Skiff's, sigil's,
 * Argosy-style and streamed units back, extracting into a staging folder; and Memory Stick failures
 * (a full stick, a write error after a suspend, a read error) that must leave no partial unit or
 * staged file behind. Every save here is synthetic. Crafted refusals are in test_saves_refusals.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "skiff/param_sfo.h"
#include "skiff/saves.h"

#include "fake_storage.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define GAME "SKIF00001"
#define OTHER_GAME "SKIF00002"
#define DATA00 GAME "DATA00"
#define DATA01 GAME "DATA01"
#define UNIT_PATH SKIFF_STORAGE_ROOT_APP "/" GAME SKIFF_SAVES_UNIT_EXTENSION
#define STAGING SKIFF_STORAGE_ROOT_APP "/stage"
/* The live probe's content, which RomM 5.4.0 hashed to PROBE_HASH (make_units.py). */
#define PROBE_SFO "\0PSF" GAME " synthetic"
#define PROBE_SFO_BYTES (sizeof PROBE_SFO - 1)
#define PROBE_DATA00_BYTES 4096U
#define PROBE_DATA01 "slot one"
#define PROBE_HASH "f0c9413915f1a6c3df66129ee6bc744b"
#define PROBE_DATA01_MD5 "62134c9338960a832f1556f3d570b59f"
#define PROBE_UNIT_BYTES 4672U
/* RomM's hash of DATA, DATA.BIN and DATA1 (make_units.py), where sorting the lines would differ. */
#define NAME_ORDER_HASH "a807b2791983e787322df2dd909e04f8"
#define EMPTY_HASH "d41d8cd98f00b204e9800998ecf8427e"
#define ZIP_END_BYTES 22U
#define UNIT_CAPACITY ((size_t)64 * 1024)
#define SFO_CAPACITY 512
#define SFO_VALUE_BYTES 4U
#define TOO_LONG_TO_LIST 130

static char dir[TEMP_DIR_PATH_MAX];
static skiff_storage *posix;
static fake_storage storage;
static skiff_storage_roots roots;
static skiff_saves_unit *unit;

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    char app[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "PSP/GAME/Skiff", app, sizeof app));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_init(dir, app, &roots));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&posix));
    fake_storage_init(&storage, posix);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, roots.saves));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, roots.app));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_unit_create(&unit));
}

void tearDown(void) {
    skiff_saves_unit_destroy(unit);
    skiff_storage_destroy(&storage.base);
    skiff_storage_destroy(posix);
    temp_dir_remove(dir);
}

static void resolve(const char *logical, char *out) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_resolve(&roots, logical, out, SKIFF_STORAGE_PATH_MAX));
}

static void put_file(const char *logical, const void *data, size_t size) {
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(logical, path);
    skiff_file *file = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_open(posix, path, SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, data, size));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
}

static void put_text(const char *logical, const char *text) {
    put_file(logical, text, strlen(text));
}

static void put_folder(const char *logical) {
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(logical, path);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, path));
}

/* A file made at the device path, for names skiff_storage_resolve() refuses. */
static void put_raw(const char *folder_logical, const char *name, const char *text) {
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(folder_logical, path);
    char file_path[SKIFF_STORAGE_PATH_MAX * 2];
    snprintf(file_path, sizeof file_path, "%s/%s", path, name);
    skiff_file *file = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(posix, file_path, SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, text, strlen(text)));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
}

static size_t read_file(const char *logical, unsigned char *out, size_t capacity) {
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(logical, path);
    skiff_file *file = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_open(posix, path, SKIFF_FILE_READ, 0, &file));
    size_t used = 0;
    size_t got = 1;
    while (got > 0 && used < capacity) {
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_read(file, out + used, capacity - used, &got));
        used += got;
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    return used;
}

static int exists(const char *logical) {
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(logical, path);
    skiff_storage_info info;
    return skiff_storage_stat(posix, path, &info) == SKIFF_OK;
}

/* The live probe's two folders under root. */
static void put_probe(const char *root) {
    char logical[SKIFF_STORAGE_PATH_MAX];
    static unsigned char data00[PROBE_DATA00_BYTES];
    memset(data00, 'A', sizeof data00);
    snprintf(logical, sizeof logical, "%s/" DATA00, root);
    put_folder(logical);
    snprintf(logical, sizeof logical, "%s/" DATA00 "/PARAM.SFO", root);
    put_file(logical, PROBE_SFO, PROBE_SFO_BYTES);
    snprintf(logical, sizeof logical, "%s/" DATA00 "/DATA.BIN", root);
    put_file(logical, data00, sizeof data00);
    snprintf(logical, sizeof logical, "%s/" DATA01, root);
    put_folder(logical);
    snprintf(logical, sizeof logical, "%s/" DATA01 "/PARAM.SFO", root);
    put_file(logical, PROBE_SFO, PROBE_SFO_BYTES);
    snprintf(logical, sizeof logical, "%s/" DATA01 "/DATA.BIN", root);
    put_text(logical, PROBE_DATA01);
    TEST_PRINTF("probe folders " DATA00 " and " DATA01 " under %s", root);
}

/* A PARAM.SFO with the given keys, each a 4-byte binary value, as the PSP lays it out. */
static size_t build_sfo(unsigned char *out, const char *const *keys, size_t count) {
    memset(out, 0, SFO_CAPACITY);
    size_t key_bytes = 0;
    for (size_t i = 0; i < count; i++) {
        key_bytes += strlen(keys[i]) + 1;
    }
    const size_t key_table = SKIFF_SFO_HEADER_BYTES + count * SKIFF_SFO_INDEX_ENTRY_BYTES;
    const size_t data_table = (key_table + key_bytes + 3) / 4 * 4;
    const size_t size = data_table + count * SFO_VALUE_BYTES;
    TEST_ASSERT_TRUE(size <= SFO_CAPACITY);
    for (size_t i = 0; i < SKIFF_SFO_MAGIC_BYTES; i++) {
        out[i] = (unsigned char)SKIFF_SFO_MAGIC[i];
    }
    out[4] = 0x01;
    out[5] = 0x01;
    out[8] = (unsigned char)key_table;
    out[12] = (unsigned char)data_table;
    out[16] = (unsigned char)count;
    size_t key_at = 0;
    for (size_t i = 0; i < count; i++) {
        unsigned char *entry = out + SKIFF_SFO_HEADER_BYTES + i * SKIFF_SFO_INDEX_ENTRY_BYTES;
        entry[0] = (unsigned char)key_at;
        entry[2] = (unsigned char)(SKIFF_SFO_FORMAT_BYTES & 0xFF);
        entry[4] = SFO_VALUE_BYTES;
        entry[8] = SFO_VALUE_BYTES;
        entry[12] = (unsigned char)(i * SFO_VALUE_BYTES);
        memcpy(out + key_table + key_at, keys[i], strlen(keys[i]) + 1);
        key_at += strlen(keys[i]) + 1;
    }
    return size;
}

static void put_sfo(const char *logical, const char *const *keys, size_t count) {
    unsigned char sfo[SFO_CAPACITY];
    const size_t size = build_sfo(sfo, keys, count);
    skiff_sfo parsed;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_sfo_parse(sfo, size, &parsed));
    put_file(logical, sfo, size);
}

static void put_fixture(const char *name, const char *logical) {
    char path[TEMP_DIR_PATH_MAX * 2];
    snprintf(path, sizeof path, "%s/saves/%s", SKIFF_FIXTURES_DIR, name);
    FILE *fixture = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(fixture);
    static unsigned char bytes[UNIT_CAPACITY];
    const size_t size = fread(bytes, 1, sizeof bytes, fixture);
    fclose(fixture);
    put_file(logical, bytes, size);
    TEST_PRINTF("fixture %s (%zu bytes) at %s", name, size, logical);
}

static skiff_err collect(const char *root) {
    const skiff_err err = skiff_saves_collect(&storage.base, &roots, root, GAME, unit);
    TEST_PRINTF("collect %s in %s: %s, %zu folders, %zu files, %llu bytes, problem %s \"%s\"", GAME,
                root, skiff_err_name(err), unit->folder_count, unit->file_count,
                (unsigned long long)unit->total_bytes, skiff_saves_problem_name(unit->problem),
                unit->problem_name);
    return err;
}

static skiff_err write_unit(void) {
    const skiff_err err = skiff_saves_write_unit(&storage.base, &roots, unit, UNIT_PATH);
    TEST_PRINTF("write %s: %s, content_hash \"%s\"", UNIT_PATH, skiff_err_name(err),
                unit->content_hash);
    return err;
}

static skiff_err read_unit(const char *staging) {
    const skiff_err err =
        skiff_saves_read_unit(&storage.base, &roots, UNIT_PATH, GAME, staging, unit);
    TEST_PRINTF("read %s into %s: %s, %zu folders, %zu files, hash \"%s\", problem %s \"%s\"",
                UNIT_PATH, staging != NULL ? staging : "(nothing)", skiff_err_name(err),
                unit->folder_count, unit->file_count, unit->content_hash,
                skiff_saves_problem_name(unit->problem), unit->problem_name);
    return err;
}

static void assert_unsyncable(skiff_saves_problem problem, const char *name) {
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE),
                             skiff_err_name(collect(SKIFF_STORAGE_ROOT_SAVES)));
    TEST_ASSERT_EQUAL_STRING(skiff_saves_problem_name(problem),
                             skiff_saves_problem_name(unit->problem));
    TEST_ASSERT_EQUAL_STRING(name, unit->problem_name);
    TEST_ASSERT_EQUAL_size_t(0, unit->folder_count);
    TEST_ASSERT_EQUAL_size_t(0, unit->file_count);
}

static void assert_probe_unit(void) {
    TEST_ASSERT_EQUAL_STRING(GAME, unit->save_target);
    TEST_ASSERT_EQUAL_size_t(2, unit->folder_count);
    TEST_ASSERT_EQUAL_STRING(DATA00, unit->folders[0]);
    TEST_ASSERT_EQUAL_STRING(DATA01, unit->folders[1]);
    TEST_ASSERT_EQUAL_size_t(4, unit->file_count);
    TEST_ASSERT_EQUAL_STRING(DATA00 "/DATA.BIN", unit->files[0].name);
    TEST_ASSERT_EQUAL_STRING(DATA00 "/PARAM.SFO", unit->files[1].name);
    TEST_ASSERT_EQUAL_STRING(DATA01 "/DATA.BIN", unit->files[2].name);
    TEST_ASSERT_EQUAL_STRING(DATA01 "/PARAM.SFO", unit->files[3].name);
    TEST_ASSERT_EQUAL_UINT64(PROBE_DATA00_BYTES, unit->files[0].size);
    TEST_ASSERT_EQUAL_UINT64(PROBE_SFO_BYTES, unit->files[1].size);
    TEST_ASSERT_EQUAL_UINT64(sizeof PROBE_DATA01 - 1, unit->files[2].size);
    TEST_ASSERT_EQUAL_UINT64(PROBE_DATA00_BYTES + 2 * PROBE_SFO_BYTES + sizeof PROBE_DATA01 - 1,
                             unit->total_bytes);
}

/* ---- Names ---- */

static void test_save_targets_are_4_to_16_capitals_or_digits(void) {
    static const char *const VALID[] = {"ULUS10064", "SKIF", "ABCDEFGHIJ012345", "0000"};
    static const char *const INVALID[] = {
        "",     "SKI",    "ABCDEFGHIJ0123456", "ulus10064", "ULUS-10064", "ULUS/1", "ULUS.",
        "ms0:", "ULUS 1", "SKIF\xC9"};
    for (size_t i = 0; i < sizeof VALID / sizeof VALID[0]; i++) {
        TEST_PRINTF("valid save_target \"%s\"", VALID[i]);
        TEST_ASSERT_TRUE(skiff_saves_target_valid(VALID[i]));
    }
    for (size_t i = 0; i < sizeof INVALID / sizeof INVALID[0]; i++) {
        TEST_PRINTF("invalid save_target \"%s\"", INVALID[i]);
        TEST_ASSERT_FALSE(skiff_saves_target_valid(INVALID[i]));
    }
    TEST_ASSERT_FALSE(skiff_saves_target_valid(NULL));
}

static void test_names_follow_fat_rules(void) {
    char longest[SKIFF_SAVES_NAME_MAX + 2];
    memset(longest, 'N', sizeof longest);
    longest[SKIFF_SAVES_NAME_MAX] = '\0';
    TEST_ASSERT_TRUE(skiff_saves_name_valid(longest));
    longest[SKIFF_SAVES_NAME_MAX] = 'N';
    longest[SKIFF_SAVES_NAME_MAX + 1] = '\0';
    TEST_ASSERT_FALSE(skiff_saves_name_valid(longest));
    static const char *const VALID[] = {"PARAM.SFO", "DATA.BIN", "ICON0.PNG", "A b-c_d (1)~",
                                        "..A"};
    static const char *const INVALID[] = {"",    " LEAD",   "TRAIL ",   "DOT.",  ".",     "..",
                                          "A/B", "A\\B",    "A:B",      "A*B",   "A?B",   "A\"B",
                                          "A<B", "A>B",     "A|B",      "A\x01", "A\x7F", "CAF\xC9",
                                          "CON", "con.txt", "LPT9.BIN", "Nul"};
    for (size_t i = 0; i < sizeof VALID / sizeof VALID[0]; i++) {
        TEST_PRINTF("valid name \"%s\"", VALID[i]);
        TEST_ASSERT_TRUE(skiff_saves_name_valid(VALID[i]));
    }
    for (size_t i = 0; i < sizeof INVALID / sizeof INVALID[0]; i++) {
        TEST_PRINTF("invalid name \"%s\"", INVALID[i]);
        TEST_ASSERT_FALSE(skiff_saves_name_valid(INVALID[i]));
    }
    TEST_ASSERT_FALSE(skiff_saves_name_valid(NULL));
}

static void test_problems_have_names_for_logs(void) {
    TEST_ASSERT_EQUAL_STRING("none", skiff_saves_problem_name(SKIFF_SAVES_PROBLEM_NONE));
    TEST_ASSERT_EQUAL_STRING("nested-folder",
                             skiff_saves_problem_name(SKIFF_SAVES_PROBLEM_NESTED_FOLDER));
    TEST_ASSERT_EQUAL_STRING("structure", skiff_saves_problem_name(SKIFF_SAVES_PROBLEM_STRUCTURE));
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    TEST_ASSERT_EQUAL_STRING("unknown", skiff_saves_problem_name((skiff_saves_problem)-1));
    const skiff_saves_problem past_the_end =
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        (skiff_saves_problem)(SKIFF_SAVES_PROBLEM_STRUCTURE + 1);
    TEST_ASSERT_EQUAL_STRING("unknown", skiff_saves_problem_name(past_the_end));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_unit_create(NULL));
    skiff_saves_unit_destroy(NULL);
}

/* ---- Collect ---- */

static void test_collect_finds_the_games_folders_and_their_files(void) {
    put_probe(SKIFF_STORAGE_ROOT_SAVES);
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/" OTHER_GAME "DATA00");
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" OTHER_GAME "DATA00/DATA.BIN", "another game");
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/X" GAME);
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" GAME ".BIN", "a file, not a folder");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    assert_probe_unit();
    TEST_ASSERT_EQUAL_STRING(SKIFF_STORAGE_ROOT_SAVES, unit->root);
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_PROBLEM_NONE, unit->problem);
    for (size_t i = 0; i < unit->file_count; i++) {
        TEST_ASSERT_TRUE(unit->files[i].mtime != SKIFF_STORAGE_MTIME_UNKNOWN);
        TEST_ASSERT_EQUAL_STRING("", unit->files[i].md5);
    }
    TEST_ASSERT_EQUAL_STRING("", unit->content_hash);
}

static void test_collect_matches_the_prefix_ignoring_case(void) {
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/skif00001data02");
    put_text(SKIFF_STORAGE_ROOT_SAVES "/skif00001data02/data.bin", "lowercase on a PC");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    TEST_ASSERT_EQUAL_size_t(1, unit->folder_count);
    TEST_ASSERT_EQUAL_STRING("skif00001data02", unit->folders[0]);
    TEST_ASSERT_EQUAL_STRING("skif00001data02/data.bin", unit->files[0].name);
}

static void test_collect_leaves_out_installs_but_keeps_folders_without_a_readable_sfo(void) {
    static const char *const SAVE_KEYS[] = {"CATEGORY", SKIFF_SFO_KEY_SAVEDATA_FILE_LIST,
                                            SKIFF_SFO_KEY_SAVEDATA_PARAMS, "TITLE"};
    static const char *const ONE_KEY[] = {"CATEGORY", SKIFF_SFO_KEY_SAVEDATA_PARAMS};
    static const char *const INSTALL_KEYS[] = {"CATEGORY", "SAVEDATA_DIRECTORY", "TITLE"};
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/" DATA00);
    put_sfo(SKIFF_STORAGE_ROOT_SAVES "/" DATA00 "/PARAM.SFO", SAVE_KEYS, 4);
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" DATA00 "/DATA.BIN", "save");
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/" GAME "SYS");
    put_sfo(SKIFF_STORAGE_ROOT_SAVES "/" GAME "SYS/PARAM.SFO", ONE_KEY, 2);
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/" GAME "INSTALL");
    put_sfo(SKIFF_STORAGE_ROOT_SAVES "/" GAME "INSTALL/PARAM.SFO", INSTALL_KEYS, 3);
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" GAME "INSTALL/DISC.DAT", "installed disc data");
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/" GAME "NOSFO");
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" GAME "NOSFO/DATA.BIN", "no sfo");
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/" GAME "TEXT");
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" GAME "TEXT/PARAM.SFO", "SAVEDATA_PARAMS, but as text");
    unsigned char *big = calloc(1, SKIFF_SFO_BYTES_MAX + 1);
    TEST_ASSERT_NOT_NULL(big);
    build_sfo(big, INSTALL_KEYS, 3);
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/" GAME "BIG");
    put_file(SKIFF_STORAGE_ROOT_SAVES "/" GAME "BIG/PARAM.SFO", big, SKIFF_SFO_BYTES_MAX + 1);
    free(big);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    static const char *const KEPT[] = {GAME "BIG", DATA00, GAME "NOSFO", GAME "SYS", GAME "TEXT"};
    TEST_ASSERT_EQUAL_size_t(sizeof KEPT / sizeof KEPT[0], unit->folder_count);
    for (size_t i = 0; i < unit->folder_count; i++) {
        TEST_PRINTF("kept %s", unit->folders[i]);
        TEST_ASSERT_EQUAL_STRING(KEPT[i], unit->folders[i]);
    }
    for (size_t i = 0; i < unit->file_count; i++) {
        TEST_ASSERT_NULL(strstr(unit->files[i].name, "INSTALL"));
    }
    TEST_ASSERT_EQUAL_size_t(6, unit->file_count);
}

static void test_collect_of_a_missing_root_or_no_folders_finds_nothing(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_APP "/missing"));
    TEST_ASSERT_EQUAL_size_t(0, unit->folder_count);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    TEST_ASSERT_EQUAL_size_t(0, unit->folder_count);
    TEST_ASSERT_EQUAL_size_t(0, unit->file_count);
}

static void test_collect_reports_a_folder_inside_a_save_folder(void) {
    put_probe(SKIFF_STORAGE_ROOT_SAVES);
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/" DATA01 "/SUB");
    assert_unsyncable(SKIFF_SAVES_PROBLEM_NESTED_FOLDER, DATA01 "/SUB");
}

static void test_collect_reports_odd_names(void) {
    put_probe(SKIFF_STORAGE_ROOT_SAVES);
    put_raw(SKIFF_STORAGE_ROOT_SAVES "/" DATA00, "CAF\xC3\xA9.BIN", "accented");
    assert_unsyncable(SKIFF_SAVES_PROBLEM_NAME, DATA00 "/CAF\xC3\xA9.BIN");

    tearDown();
    setUp();
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(SKIFF_STORAGE_ROOT_SAVES, path);
    char odd[SKIFF_STORAGE_PATH_MAX * 2];
    snprintf(odd, sizeof odd, "%s/" GAME "A\x01", path);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdir(posix, odd));
    assert_unsyncable(SKIFF_SAVES_PROBLEM_NAME, GAME "A\x01");
}

static void test_collect_reports_names_too_long_for_a_unit_or_a_listing(void) {
    put_probe(SKIFF_STORAGE_ROOT_SAVES);
    char name[TOO_LONG_TO_LIST + 1];
    memset(name, 'L', sizeof name);
    name[SKIFF_SAVES_NAME_MAX + 1] = '\0';
    put_raw(SKIFF_STORAGE_ROOT_SAVES "/" DATA00, name, "65 bytes of name");
    char expected[SKIFF_SAVES_ENTRY_SIZE];
    snprintf(expected, sizeof expected, DATA00 "/%.*s", SKIFF_SAVES_NAME_MAX, name);
    assert_unsyncable(SKIFF_SAVES_PROBLEM_NAME, expected);

    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(SKIFF_STORAGE_ROOT_SAVES "/" DATA00, path);
    char file_path[SKIFF_STORAGE_PATH_MAX * 2];
    snprintf(file_path, sizeof file_path, "%s/%s", path, name);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(posix, file_path));
    memset(name, 'L', TOO_LONG_TO_LIST);
    name[TOO_LONG_TO_LIST] = '\0';
    put_raw(SKIFF_STORAGE_ROOT_SAVES "/" DATA00, name, "too long to list");
    assert_unsyncable(SKIFF_SAVES_PROBLEM_NAME, DATA00);
}

static void test_collect_reports_names_that_differ_only_in_case(void) {
    put_probe(SKIFF_STORAGE_ROOT_SAVES);
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" DATA01 "/data.bin", "a second spelling");
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE),
                             skiff_err_name(collect(SKIFF_STORAGE_ROOT_SAVES)));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_PROBLEM_DUPLICATE, unit->problem);

    tearDown();
    setUp();
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/" DATA00);
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/skif00001data00");
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE),
                             skiff_err_name(collect(SKIFF_STORAGE_ROOT_SAVES)));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_PROBLEM_DUPLICATE, unit->problem);
}

static void test_collect_caps_folders_files_and_bytes(void) {
    char logical[SKIFF_STORAGE_PATH_MAX];
    for (int i = 0; i <= SKIFF_SAVES_FOLDERS_MAX; i++) {
        snprintf(logical, sizeof logical, SKIFF_STORAGE_ROOT_SAVES "/" GAME "F%03d", i);
        put_folder(logical);
    }
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE),
                             skiff_err_name(collect(SKIFF_STORAGE_ROOT_SAVES)));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_PROBLEM_TOO_MANY_FOLDERS, unit->problem);

    tearDown();
    setUp();
    for (int i = 0; i <= SKIFF_SAVES_FILES_MAX; i++) {
        snprintf(logical, sizeof logical, SKIFF_STORAGE_ROOT_SAVES "/" GAME "F%d", i / 100);
        put_folder(logical);
        snprintf(logical, sizeof logical, SKIFF_STORAGE_ROOT_SAVES "/" GAME "F%d/FILE%03d.BIN",
                 i / 100, i);
        put_text(logical, "x");
    }
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE),
                             skiff_err_name(collect(SKIFF_STORAGE_ROOT_SAVES)));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_PROBLEM_TOO_MANY_FILES, unit->problem);

    tearDown();
    setUp();
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/" DATA00);
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" DATA00 "/HUGE.BIN", "");
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(SKIFF_STORAGE_ROOT_SAVES "/" DATA00 "/HUGE.BIN", path);
    /* Sparse: the collect only lists sizes. */
    TEST_ASSERT_EQUAL_INT(0, truncate(path, (off_t)SKIFF_SAVES_UNIT_BYTES_MAX + 1));
    assert_unsyncable(SKIFF_SAVES_PROBLEM_TOO_LARGE, "");
}

static void test_collect_passes_storage_errors_on(void) {
    put_probe(SKIFF_STORAGE_ROOT_SAVES);
    storage.fail_suffix = "SAVEDATA";
    storage.list_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, collect(SKIFF_STORAGE_ROOT_SAVES));
    storage.fail_suffix = DATA01;
    storage.list_error = SKIFF_ERR_STORAGE_NO_MEDIA;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_MEDIA, collect(SKIFF_STORAGE_ROOT_SAVES));
    TEST_ASSERT_EQUAL_size_t(0, unit->folder_count);
    storage.list_error = SKIFF_OK;
    storage.fail_suffix = "PARAM.SFO";
    storage.read_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, collect(SKIFF_STORAGE_ROOT_SAVES));
}

static void test_collect_checks_its_arguments(void) {
    char long_root[SKIFF_STORAGE_PATH_MAX + 1];
    memset(long_root, 'r', sizeof long_root - 1);
    long_root[sizeof long_root - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_collect(NULL, &roots, SKIFF_STORAGE_ROOT_SAVES, GAME, unit));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_saves_collect(&storage.base, NULL, SKIFF_STORAGE_ROOT_SAVES, GAME, unit));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_collect(&storage.base, &roots, NULL, GAME, unit));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_saves_collect(&storage.base, &roots, SKIFF_STORAGE_ROOT_SAVES, "../X", unit));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_saves_collect(&storage.base, &roots, SKIFF_STORAGE_ROOT_SAVES, GAME, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_collect(&storage.base, &roots, long_root, GAME, unit));
}

/* ---- Write ---- */

static void test_the_written_unit_is_sigils_byte_for_byte(void) {
    put_probe(SKIFF_STORAGE_ROOT_SAVES);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, write_unit());
    TEST_ASSERT_EQUAL_STRING(PROBE_HASH, unit->content_hash);
    TEST_ASSERT_EQUAL_STRING(PROBE_DATA01_MD5, unit->files[2].md5);
    static unsigned char written[UNIT_CAPACITY];
    const size_t size = read_file(UNIT_PATH, written, sizeof written);
    put_fixture("probe-stored.zip", SKIFF_STORAGE_ROOT_APP "/golden.zip");
    static unsigned char golden[UNIT_CAPACITY];
    const size_t golden_size =
        read_file(SKIFF_STORAGE_ROOT_APP "/golden.zip", golden, sizeof golden);
    TEST_PRINTF("written %zu bytes, sigil's unit %zu bytes", size, golden_size);
    TEST_ASSERT_EQUAL_size_t(PROBE_UNIT_BYTES, golden_size);
    TEST_ASSERT_EQUAL_size_t(golden_size, size);
    TEST_ASSERT_EQUAL_MEMORY(golden, written, size);
}

static void test_the_content_hash_takes_names_in_order_as_romm_does(void) {
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/" DATA00);
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" DATA00 "/DATA1", "three");
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" DATA00 "/DATA.BIN", "two");
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" DATA00 "/DATA", "one");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    TEST_ASSERT_EQUAL_STRING(DATA00 "/DATA", unit->files[0].name);
    TEST_ASSERT_EQUAL_STRING(DATA00 "/DATA.BIN", unit->files[1].name);
    TEST_ASSERT_EQUAL_STRING(DATA00 "/DATA1", unit->files[2].name);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, write_unit());
    TEST_ASSERT_EQUAL_STRING(NAME_ORDER_HASH, unit->content_hash);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, read_unit(NULL));
    TEST_ASSERT_EQUAL_STRING(NAME_ORDER_HASH, unit->content_hash);
}

static void test_a_game_without_saves_writes_an_empty_unit(void) {
    put_folder(SKIFF_STORAGE_ROOT_SAVES "/" GAME "EMPTY");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    TEST_ASSERT_EQUAL_size_t(1, unit->folder_count);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, write_unit());
    TEST_ASSERT_EQUAL_STRING(EMPTY_HASH, unit->content_hash);
    unsigned char bytes[ZIP_END_BYTES + 1];
    TEST_ASSERT_EQUAL_size_t(ZIP_END_BYTES, read_file(UNIT_PATH, bytes, sizeof bytes));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, read_unit(NULL));
    TEST_ASSERT_EQUAL_size_t(0, unit->file_count);
    TEST_ASSERT_EQUAL_STRING(EMPTY_HASH, unit->content_hash);
}

static void assert_no_unit_after(skiff_err expected) {
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(expected), skiff_err_name(write_unit()));
    TEST_ASSERT_FALSE(exists(UNIT_PATH));
    TEST_ASSERT_EQUAL_STRING("", unit->content_hash);
    for (size_t i = 0; i < unit->file_count; i++) {
        TEST_ASSERT_EQUAL_STRING("", unit->files[i].md5);
    }
}

static void test_a_cut_write_leaves_no_partial_unit(void) {
    static const uint64_t BUDGETS[] = {0, 10, 30, 100, 4200, PROBE_UNIT_BYTES - 1};
    put_probe(SKIFF_STORAGE_ROOT_SAVES);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    storage.fail_suffix = SKIFF_SAVES_UNIT_EXTENSION;
    storage.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    for (size_t i = 0; i < sizeof BUDGETS / sizeof BUDGETS[0]; i++) {
        storage.bytes_written = 0;
        storage.write_budget = BUDGETS[i];
        TEST_PRINTF("the Memory Stick fills after %llu bytes", (unsigned long long)BUDGETS[i]);
        assert_no_unit_after(SKIFF_ERR_STORAGE_NO_SPACE);
    }
    storage.write_error = SKIFF_OK;
    storage.bytes_written = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, write_unit());
    TEST_ASSERT_EQUAL_STRING(PROBE_HASH, unit->content_hash);
}

static void test_a_suspend_or_failed_sync_leaves_no_partial_unit(void) {
    put_probe(SKIFF_STORAGE_ROOT_SAVES);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    storage.fail_suffix = SKIFF_SAVES_UNIT_EXTENSION;
    storage.stale_after_writes = 1;
    TEST_PRINTF("open files go stale after the first write to the unit, as after a suspend");
    assert_no_unit_after(SKIFF_ERR_STORAGE_IO);
    TEST_ASSERT_EQUAL_INT(1, storage.handles_lost);
    storage.stale_after_writes = 0;
    storage.sync_error = SKIFF_ERR_STORAGE_IO;
    assert_no_unit_after(SKIFF_ERR_STORAGE_IO);
    storage.sync_error = SKIFF_OK;
    storage.fail_suffix = "DATA.BIN";
    storage.read_error = SKIFF_ERR_STORAGE_IO;
    assert_no_unit_after(SKIFF_ERR_STORAGE_IO);
}

static void test_a_save_changed_since_the_collect_is_refused(void) {
    put_probe(SKIFF_STORAGE_ROOT_SAVES);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" DATA01 "/DATA.BIN", "slot one, played longer");
    assert_no_unit_after(SKIFF_ERR_STORAGE_IO);
    put_text(SKIFF_STORAGE_ROOT_SAVES "/" DATA01 "/DATA.BIN", "slot");
    assert_no_unit_after(SKIFF_ERR_STORAGE_IO);
}

static void test_write_checks_its_arguments(void) {
    put_probe(SKIFF_STORAGE_ROOT_SAVES);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_write_unit(NULL, &roots, unit, UNIT_PATH));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_write_unit(&storage.base, NULL, unit, UNIT_PATH));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_write_unit(&storage.base, &roots, NULL, UNIT_PATH));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_write_unit(&storage.base, &roots, unit, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_write_unit(&storage.base, &roots, unit, "nowhere:/x.zip"));
    snprintf(unit->files[1].name, sizeof unit->files[1].name, "%s", "NOFOLDER");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, write_unit());
    snprintf(unit->files[1].name, sizeof unit->files[1].name, "%s", DATA00 "/../X");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, write_unit());
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    unit->files[0].size = SKIFF_SAVES_UNIT_BYTES_MAX;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, write_unit());
    unit->root[0] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, write_unit());
    TEST_ASSERT_FALSE(exists(UNIT_PATH));
}

/* ---- Read ---- */

static void test_skiffs_unit_reads_back_the_same(void) {
    put_probe(SKIFF_STORAGE_ROOT_SAVES);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, collect(SKIFF_STORAGE_ROOT_SAVES));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, write_unit());
    char md5[SKIFF_SAVES_FILES_MAX][SKIFF_MD5_HEX_SIZE];
    for (size_t i = 0; i < unit->file_count; i++) {
        memcpy(md5[i], unit->files[i].md5, sizeof md5[i]);
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, read_unit(NULL));
    assert_probe_unit();
    TEST_ASSERT_EQUAL_STRING(PROBE_HASH, unit->content_hash);
    TEST_ASSERT_EQUAL_STRING("", unit->root);
    for (size_t i = 0; i < unit->file_count; i++) {
        TEST_ASSERT_EQUAL_STRING(md5[i], unit->files[i].md5);
        TEST_ASSERT_EQUAL_INT64(SKIFF_STORAGE_MTIME_UNKNOWN, unit->files[i].mtime);
    }
    TEST_ASSERT_FALSE(exists(STAGING));
}

static void assert_staged_probe(void) {
    static unsigned char bytes[PROBE_DATA00_BYTES + 1];
    TEST_ASSERT_EQUAL_size_t(PROBE_DATA00_BYTES,
                             read_file(STAGING "/" DATA00 "/DATA.BIN", bytes, sizeof bytes));
    for (size_t i = 0; i < PROBE_DATA00_BYTES; i++) {
        TEST_ASSERT_EQUAL_HEX8('A', bytes[i]);
    }
    TEST_ASSERT_EQUAL_size_t(sizeof PROBE_DATA01 - 1,
                             read_file(STAGING "/" DATA01 "/DATA.BIN", bytes, sizeof bytes));
    TEST_ASSERT_EQUAL_MEMORY(PROBE_DATA01, bytes, sizeof PROBE_DATA01 - 1);
    TEST_ASSERT_EQUAL_size_t(PROBE_SFO_BYTES,
                             read_file(STAGING "/" DATA01 "/PARAM.SFO", bytes, sizeof bytes));
    TEST_ASSERT_EQUAL_MEMORY(PROBE_SFO, bytes, PROBE_SFO_BYTES);
}

static void test_units_from_sigil_argosy_and_streaming_writers_extract_alike(void) {
    static const char *const FIXTURES[] = {"probe-stored.zip", "probe-deflated.zip",
                                           "probe-descriptors.zip"};
    for (size_t i = 0; i < sizeof FIXTURES / sizeof FIXTURES[0]; i++) {
        put_fixture(FIXTURES[i], UNIT_PATH);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, read_unit(STAGING));
        assert_probe_unit();
        TEST_ASSERT_EQUAL_STRING(PROBE_HASH, unit->content_hash);
        TEST_ASSERT_EQUAL_STRING(STAGING, unit->root);
        assert_staged_probe();
        tearDown();
        setUp();
    }
}

/* The staging folder holds nothing but empty folders the unit did not create. */
static size_t staged_entries;

static skiff_storage_list_step count_entry(void *ctx, const skiff_storage_entry *entry) {
    (void)ctx;
    TEST_PRINTF("left in staging: %s", entry->name);
    staged_entries++;
    return SKIFF_STORAGE_LIST_CONTINUE;
}

static void assert_nothing_staged(void) {
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(STAGING, path);
    staged_entries = 0;
    size_t skipped = 0;
    const skiff_err err = skiff_storage_list(posix, path, count_entry, NULL, &skipped);
    TEST_ASSERT_TRUE(err == SKIFF_OK || err == SKIFF_ERR_STORAGE_NOT_FOUND);
    TEST_ASSERT_EQUAL_size_t(0, staged_entries);
}

static void test_a_failed_extraction_removes_what_it_wrote(void) {
    put_fixture("probe-deflated.zip", UNIT_PATH);
    storage.fail_suffix = "PARAM.SFO";
    storage.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    storage.write_budget = PROBE_SFO_BYTES + 3;
    TEST_PRINTF("the stick fills during the second PARAM.SFO");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, read_unit(STAGING));
    assert_nothing_staged();
    TEST_ASSERT_EQUAL_size_t(0, unit->file_count);
    TEST_ASSERT_EQUAL_STRING("", unit->content_hash);
    storage.write_error = SKIFF_OK;
    storage.fail_suffix = "DATA.BIN";
    storage.stale_after_writes = 1;
    storage.writes = 0;
    TEST_PRINTF("a suspend after the first DATA.BIN write");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, read_unit(STAGING));
    assert_nothing_staged();
    storage.stale_after_writes = 0;
    storage.sync_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, read_unit(STAGING));
    assert_nothing_staged();
    storage.sync_error = SKIFF_OK;
    storage.fail_suffix = SKIFF_SAVES_UNIT_EXTENSION;
    storage.read_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, read_unit(STAGING));
    assert_nothing_staged();
    storage.read_error = SKIFF_OK;
    storage.fail_suffix = DATA01;
    storage.mkdir_error = SKIFF_ERR_STORAGE_NO_MEDIA;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_MEDIA, read_unit(STAGING));
    assert_nothing_staged();
    storage.mkdir_error = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, read_unit(STAGING));
    assert_staged_probe();
}

static void test_a_missing_unit_or_bad_arguments(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, read_unit(NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_read_unit(NULL, &roots, UNIT_PATH, GAME, NULL, unit));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_read_unit(&storage.base, NULL, UNIT_PATH, GAME, NULL, unit));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_read_unit(&storage.base, &roots, NULL, GAME, NULL, unit));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_read_unit(&storage.base, &roots, UNIT_PATH, "x", NULL, unit));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_saves_read_unit(&storage.base, &roots, UNIT_PATH, GAME, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_saves_read_unit(&storage.base, &roots, "nowhere:/x.zip", GAME, NULL, unit));
    put_fixture("probe-stored.zip", UNIT_PATH);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, read_unit("nowhere:/stage"));
    assert_nothing_staged();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_save_targets_are_4_to_16_capitals_or_digits);
    RUN_TEST(test_names_follow_fat_rules);
    RUN_TEST(test_problems_have_names_for_logs);
    RUN_TEST(test_collect_finds_the_games_folders_and_their_files);
    RUN_TEST(test_collect_matches_the_prefix_ignoring_case);
    RUN_TEST(test_collect_leaves_out_installs_but_keeps_folders_without_a_readable_sfo);
    RUN_TEST(test_collect_of_a_missing_root_or_no_folders_finds_nothing);
    RUN_TEST(test_collect_reports_a_folder_inside_a_save_folder);
    RUN_TEST(test_collect_reports_odd_names);
    RUN_TEST(test_collect_reports_names_too_long_for_a_unit_or_a_listing);
    RUN_TEST(test_collect_reports_names_that_differ_only_in_case);
    RUN_TEST(test_collect_caps_folders_files_and_bytes);
    RUN_TEST(test_collect_passes_storage_errors_on);
    RUN_TEST(test_collect_checks_its_arguments);
    RUN_TEST(test_the_written_unit_is_sigils_byte_for_byte);
    RUN_TEST(test_the_content_hash_takes_names_in_order_as_romm_does);
    RUN_TEST(test_a_game_without_saves_writes_an_empty_unit);
    RUN_TEST(test_a_cut_write_leaves_no_partial_unit);
    RUN_TEST(test_a_suspend_or_failed_sync_leaves_no_partial_unit);
    RUN_TEST(test_a_save_changed_since_the_collect_is_refused);
    RUN_TEST(test_write_checks_its_arguments);
    RUN_TEST(test_skiffs_unit_reads_back_the_same);
    RUN_TEST(test_units_from_sigil_argosy_and_streaming_writers_extract_alike);
    RUN_TEST(test_a_failed_extraction_removes_what_it_wrote);
    RUN_TEST(test_a_missing_unit_or_bad_arguments);
    return UNITY_END();
}
