/*
 * Restoring save units (skiff/saves_restore.h) over a temporary directory laid out as a Memory
 * Stick. The live game has DATA00 (replaced), DATA01 (which the unit lacks, so removed), an
 * install folder and another game's folder (both left alone); the unit brings DATA00 and a new
 * DATA02. Covered: the plain restore and its backup; a power cut after every single change the
 * restore makes, each followed by the recovery, which must end in the old save or the new one,
 * never a mix nor neither; a suspend during the copy; a full Memory Stick, a damaged unit and
 * folders no unit may hold, refused before anything is written; folders a listing gives in
 * capitals; the empty unit; the recovery's own cases; and the backups' caps and order. Every save
 * here is synthetic.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/param_sfo.h"
#include "skiff/saves.h"
#include "skiff/saves_restore.h"

#include "fake_storage.h"
#include "saves_internal.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define GAME "SKIF00001"
#define OTHER_GAME "SKIF00002"
#define DATA00 GAME "DATA00"
#define DATA01 GAME "DATA01"
#define DATA02 GAME "DATA02"
#define INSTALL GAME "INSTALL"
#define OTHER_DATA OTHER_GAME "DATA00"
#define SAVES SKIFF_STORAGE_ROOT_SAVES
#define SOURCE SKIFF_STORAGE_ROOT_APP "/source"
#define UNIT_PATH SKIFF_SAVES_WORK_FOLDER "/" GAME SKIFF_SAVES_UNIT_EXTENSION
#define CHECK_UNIT SKIFF_STORAGE_ROOT_APP "/check.zip"
#define OLD_DATA00 "old save, slot 0"
#define OLD_DATA01 "old save, slot 1"
#define NEW_DATA00 "new save, slot 0, longer than before"
#define NEW_DATA02 "new save, slot 2"
#define NEW_EXTRA02 "slot 2 extra"
#define INSTALL_DATA "installed game data"
#define OTHER_DATA_TEXT "another game's save"
#define TEXT_MAX 256
#define SFO_CAPACITY 512
#define SFO_VALUE_BYTES 4U
/* 11 MiB: three of them pass the 32 MiB cap on backups. */
#define BIG_BACKUP_BYTES ((size_t)11 * 1024 * 1024)
#define CHUNK_BYTES ((size_t)64 * 1024)
/* Other games' folders in the save root: more than one listing of the restore keeps. */
#define OTHER_FOLDERS 300

static char dir[TEMP_DIR_PATH_MAX];
static skiff_storage *posix;
static fake_storage storage;
static skiff_storage_roots roots;
static skiff_saves_unit *unit;
static char old_hash[SKIFF_SAVES_HASH_SIZE];
static char new_hash[SKIFF_SAVES_HASH_SIZE];

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
    old_hash[0] = '\0';
    new_hash[0] = '\0';
}

void tearDown(void) {
    skiff_saves_unit_destroy(unit);
    skiff_storage_destroy(&storage.base);
    skiff_storage_destroy(posix);
    temp_dir_remove(dir);
}

/* ---- The Memory Stick ---- */

static void resolve(const char *logical, char *out) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_resolve(&roots, logical, out, SKIFF_STORAGE_PATH_MAX));
}

static void put_file(const char *logical, const void *data, size_t size) {
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(logical, path);
    skiff_file *file = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_open(posix, path, SKIFF_FILE_REPLACE, 0, &file));
    if (size > 0) {
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, data, size));
    }
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

/* The file's text, or "(missing)". */
static const char *read_text(const char *logical, char *out) {
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(logical, path);
    size_t length = 0;
    skiff_storage_info info;
    if (skiff_storage_stat(posix, path, &info) != SKIFF_OK) {
        return "(missing)";
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_read_whole(posix, path, out, TEXT_MAX - 1, &length));
    out[length] = '\0';
    return out;
}

static int exists(const char *logical) {
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(logical, path);
    skiff_storage_info info;
    return skiff_storage_stat(posix, path, &info) == SKIFF_OK;
}

static void assert_text(const char *expected, const char *logical) {
    char text[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, read_text(logical, text), logical);
}

/* A PARAM.SFO with the given keys, each a 4-byte binary value, as the PSP lays it out. */
static void put_sfo(const char *logical, const char *const *keys, size_t count) {
    unsigned char out[SFO_CAPACITY];
    memset(out, 0, sizeof out);
    size_t key_bytes = 0;
    for (size_t i = 0; i < count; i++) {
        key_bytes += strlen(keys[i]) + 1;
    }
    const size_t key_table = SKIFF_SFO_HEADER_BYTES + count * SKIFF_SFO_INDEX_ENTRY_BYTES;
    const size_t data_table = (key_table + key_bytes + 3) / 4 * 4;
    const size_t size = data_table + count * SFO_VALUE_BYTES;
    memcpy(out, SKIFF_SFO_MAGIC, SKIFF_SFO_MAGIC_BYTES);
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
    skiff_sfo parsed;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_sfo_parse(out, size, &parsed));
    put_file(logical, out, size);
}

static const char *const SAVE_KEYS[] = {SKIFF_SFO_KEY_SAVEDATA_PARAMS, "TITLE"};
static const char *const INSTALL_KEYS[] = {"TITLE"};

/* The game as the player left it, an install beside it and another game's save. */
static void put_live(void) {
    put_folder(SAVES "/" DATA00);
    put_sfo(SAVES "/" DATA00 "/PARAM.SFO", SAVE_KEYS, 2);
    put_text(SAVES "/" DATA00 "/DATA.BIN", OLD_DATA00);
    put_folder(SAVES "/" DATA01);
    put_text(SAVES "/" DATA01 "/DATA.BIN", OLD_DATA01);
    put_folder(SAVES "/" INSTALL);
    put_sfo(SAVES "/" INSTALL "/PARAM.SFO", INSTALL_KEYS, 1);
    put_text(SAVES "/" INSTALL "/GAME.BIN", INSTALL_DATA);
    put_folder(SAVES "/" OTHER_DATA);
    put_text(SAVES "/" OTHER_DATA "/DATA.BIN", OTHER_DATA_TEXT);
    TEST_PRINTF("live: %s (PARAM.SFO, DATA.BIN), %s (DATA.BIN), install %s, other game %s", DATA00,
                DATA01, INSTALL, OTHER_DATA);
}

/* RomM's content_hash of the game's live folders now, or "" when collecting refuses. */
static void live_hash(char *out) {
    out[0] = '\0';
    skiff_saves_unit *live = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_unit_create(&live));
    if (skiff_saves_collect(posix, &roots, SAVES, GAME, live) == SKIFF_OK &&
        skiff_saves_write_unit(posix, &roots, live, CHECK_UNIT) == SKIFF_OK) {
        memcpy(out, live->content_hash, SKIFF_SAVES_HASH_SIZE);
    }
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(CHECK_UNIT, path);
    (void)skiff_storage_remove(posix, path);
    skiff_saves_unit_destroy(live);
}

/* Writes the unit of SOURCE's folders to UNIT_PATH, its hash into new_hash. */
static void write_source_unit(void) {
    put_folder(SKIFF_SAVES_WORK_FOLDER);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_collect(posix, &roots, SOURCE, GAME, unit));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_write_unit(posix, &roots, unit, UNIT_PATH));
    memcpy(new_hash, unit->content_hash, sizeof new_hash);
    TEST_PRINTF("unit %s: %zu folders, %zu files, hash %s", UNIT_PATH, unit->folder_count,
                unit->file_count, new_hash);
}

/* The unit RomM sends: DATA00 rewritten (its PARAM.SFO kept), DATA02 new, DATA01 gone. */
static void put_new_unit(void) {
    put_folder(SOURCE "/" DATA00);
    put_sfo(SOURCE "/" DATA00 "/PARAM.SFO", SAVE_KEYS, 2);
    put_text(SOURCE "/" DATA00 "/DATA.BIN", NEW_DATA00);
    put_folder(SOURCE "/" DATA02);
    put_text(SOURCE "/" DATA02 "/DATA.BIN", NEW_DATA02);
    put_text(SOURCE "/" DATA02 "/EXTRA.BIN", NEW_EXTRA02);
    write_source_unit();
}

/* The live game, the unit, and both hashes. */
static void prepare(void) {
    put_live();
    live_hash(old_hash);
    TEST_ASSERT_NOT_EQUAL_INT(0, (int)strlen(old_hash));
    put_new_unit();
    TEST_ASSERT_TRUE(strcmp(old_hash, new_hash) != 0);
}

static void assert_others_untouched(void) {
    assert_text(INSTALL_DATA, SAVES "/" INSTALL "/GAME.BIN");
    TEST_ASSERT_TRUE(exists(SAVES "/" INSTALL "/PARAM.SFO"));
    assert_text(OTHER_DATA_TEXT, SAVES "/" OTHER_DATA "/DATA.BIN");
}

static void assert_new_save(void) {
    assert_text(NEW_DATA00, SAVES "/" DATA00 "/DATA.BIN");
    TEST_ASSERT_TRUE(exists(SAVES "/" DATA00 "/PARAM.SFO"));
    TEST_ASSERT_FALSE_MESSAGE(exists(SAVES "/" DATA01), "the folder the unit lacks is removed");
    assert_text(NEW_DATA02, SAVES "/" DATA02 "/DATA.BIN");
    assert_text(NEW_EXTRA02, SAVES "/" DATA02 "/EXTRA.BIN");
    char hash[SKIFF_SAVES_HASH_SIZE];
    live_hash(hash);
    TEST_ASSERT_EQUAL_STRING(new_hash, hash);
    assert_others_untouched();
}

static void assert_old_save(void) {
    char hash[SKIFF_SAVES_HASH_SIZE];
    live_hash(hash);
    TEST_ASSERT_EQUAL_STRING(old_hash, hash);
    assert_text(OLD_DATA00, SAVES "/" DATA00 "/DATA.BIN");
    assert_text(OLD_DATA01, SAVES "/" DATA01 "/DATA.BIN");
    TEST_ASSERT_FALSE(exists(SAVES "/" DATA02));
    assert_others_untouched();
}

static void assert_work_clean(void) {
    TEST_ASSERT_FALSE_MESSAGE(exists(SKIFF_SAVES_STAGE_FOLDER), "the stage is removed");
    TEST_ASSERT_FALSE_MESSAGE(exists(SKIFF_SAVES_JOURNAL_PATH), "the journal is removed");
    TEST_ASSERT_FALSE(exists(SKIFF_SAVES_JOURNAL_PATH SKIFF_STORAGE_PENDING_SUFFIX));
    TEST_ASSERT_FALSE(exists(SKIFF_SAVES_JOURNAL_PATH SKIFF_STORAGE_DRAFT_SUFFIX));
}

static skiff_err restore_on(skiff_storage *on, skiff_saves_restore_report *report) {
    const skiff_err err = skiff_saves_restore(on, &roots, UNIT_PATH, GAME, report);
    TEST_PRINTF("restore %s: %s", GAME, skiff_err_name(err));
    if (err == SKIFF_OK && report != NULL) {
        TEST_PRINTF("  hash %s, backup %u, %zu replaced, %zu removed, %zu installs kept, "
                    "recovered %s",
                    report->content_hash, (unsigned)report->backup, report->folders_replaced,
                    report->folders_removed, report->installs_kept,
                    skiff_err_name(report->recovered));
    }
    return err;
}

static skiff_err recover_on(skiff_storage *on, skiff_saves_recovery *recovery) {
    const skiff_err err = skiff_saves_recover(on, &roots, recovery);
    TEST_PRINTF("recover: %s, outcome %d, game \"%s\", backup %u", skiff_err_name(err),
                (int)recovery->outcome, recovery->save_target, (unsigned)recovery->backup);
    return err;
}

/* The game's backup number's hash, as read back. */
static void backup_hash(uint32_t number, char *out) {
    char logical[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_backup_path(GAME, number, logical, sizeof logical));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_read_unit(posix, &roots, logical, GAME, NULL, unit));
    memcpy(out, unit->content_hash, SKIFF_SAVES_HASH_SIZE);
}

/* ---- A restore ---- */

static void test_restore_replaces_removes_and_adds_folders_after_a_backup(void) {
    prepare();
    skiff_saves_restore_report report;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, &report));
    assert_new_save();
    assert_work_clean();
    TEST_ASSERT_EQUAL_STRING(new_hash, report.content_hash);
    TEST_ASSERT_EQUAL_UINT32(1, report.backup);
    TEST_ASSERT_EQUAL_size_t(2, report.folders_replaced);
    TEST_ASSERT_EQUAL_size_t(1, report.folders_removed);
    TEST_ASSERT_EQUAL_size_t(0, report.installs_kept);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, report.recovered);
    TEST_ASSERT_TRUE_MESSAGE(exists(UNIT_PATH), "the unit itself is the caller's to remove");
    char hash[SKIFF_SAVES_HASH_SIZE];
    backup_hash(1, hash);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(old_hash, hash, "the backup holds the save as it was");
    TEST_ASSERT_FALSE(exists(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/1.zip.tmp"));
}

/* DATA01 is the only live folder removed whole; with the stage's two folders and the stage
 * itself, four folders reach the device's rmdir, each emptied first: one with files would make
 * skiff_storage_rmdir() refuse and the restore fail. */
static void test_folders_reach_rmdir_only_once_emptied(void) {
    prepare();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, NULL));
    TEST_PRINTF("device rmdirs: %d", storage.rmdirs);
    TEST_ASSERT_EQUAL_INT(4, storage.rmdirs);
    assert_new_save();
}

static void test_a_restore_of_the_same_unit_twice_ends_the_same(void) {
    prepare();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, NULL));
    skiff_saves_restore_report report;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, &report));
    TEST_ASSERT_EQUAL_UINT32(2, report.backup);
    TEST_ASSERT_EQUAL_size_t(0, report.folders_removed);
    assert_new_save();
    assert_work_clean();
}

/* A listing may give a folder in capitals: the unit's "SKIF00001data00" replaces the live
 * "SKIF00001DATA00" in place, never beside it. */
static void test_live_folders_match_the_unit_ignoring_case(void) {
    put_folder(SAVES "/" DATA00);
    put_text(SAVES "/" DATA00 "/DATA.BIN", OLD_DATA00);
    put_text(SAVES "/" DATA00 "/OLD.BIN", OLD_DATA01);
    put_folder(SOURCE "/" GAME "data00");
    put_text(SOURCE "/" GAME "data00/DATA.BIN", NEW_DATA00);
    write_source_unit();
    skiff_saves_restore_report report;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, &report));
    TEST_ASSERT_EQUAL_size_t(1, report.folders_replaced);
    TEST_ASSERT_EQUAL_size_t(0, report.folders_removed);
    assert_text(NEW_DATA00, SAVES "/" DATA00 "/DATA.BIN");
    TEST_ASSERT_FALSE(exists(SAVES "/" DATA00 "/OLD.BIN"));
    TEST_ASSERT_FALSE_MESSAGE(exists(SAVES "/" GAME "data00"), "no second folder beside it");
    assert_work_clean();
}

/* A unit (one grout wrote) carrying the install's folder: the install is kept as it is. */
static void test_an_install_is_never_touched_even_when_the_unit_carries_it(void) {
    put_live();
    put_folder(SOURCE "/" DATA00);
    put_text(SOURCE "/" DATA00 "/DATA.BIN", NEW_DATA00);
    put_folder(SOURCE "/" INSTALL);
    put_text(SOURCE "/" INSTALL "/GAME.BIN", "a unit's own copy");
    /* No PARAM.SFO in the source's install folder, so the collect keeps it in the unit. */
    write_source_unit();
    TEST_ASSERT_EQUAL_size_t(2, unit->folder_count);
    skiff_saves_restore_report report;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, &report));
    TEST_ASSERT_EQUAL_size_t(1, report.installs_kept);
    TEST_ASSERT_EQUAL_size_t(1, report.folders_replaced);
    TEST_ASSERT_EQUAL_size_t(1, report.folders_removed);
    assert_text(NEW_DATA00, SAVES "/" DATA00 "/DATA.BIN");
    assert_others_untouched();
    assert_work_clean();
}

/* A Memory Stick full of other games' saves: the install is still found among them, however far
 * down the listing it comes. */
static void test_an_install_is_found_among_hundreds_of_other_folders(void) {
    for (int i = 0; i < OTHER_FOLDERS; i++) {
        char folder[SKIFF_STORAGE_PATH_MAX];
        snprintf(folder, sizeof folder, "%s/ULUS%05dDATA00", SAVES, i);
        put_folder(folder);
    }
    put_live();
    put_folder(SOURCE "/" DATA00);
    put_text(SOURCE "/" DATA00 "/DATA.BIN", NEW_DATA00);
    put_folder(SOURCE "/" INSTALL);
    put_text(SOURCE "/" INSTALL "/GAME.BIN", "a unit's own copy");
    write_source_unit();
    skiff_saves_restore_report report;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, &report));
    TEST_ASSERT_EQUAL_size_t(1, report.installs_kept);
    assert_others_untouched();
}

/* An empty unit (22 bytes): the save was deleted elsewhere. Restoring it removes the game's save
 * folders, after backing them up; the install and other games stay. */
static void test_an_empty_unit_removes_the_save_folders_after_a_backup(void) {
    put_live();
    live_hash(old_hash);
    write_source_unit();
    TEST_ASSERT_EQUAL_size_t(0, unit->file_count);
    skiff_saves_restore_report report;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, &report));
    TEST_ASSERT_EQUAL_size_t(0, report.folders_replaced);
    TEST_ASSERT_EQUAL_size_t(2, report.folders_removed);
    TEST_ASSERT_EQUAL_UINT32(1, report.backup);
    TEST_ASSERT_FALSE(exists(SAVES "/" DATA00));
    TEST_ASSERT_FALSE(exists(SAVES "/" DATA01));
    assert_others_untouched();
    char hash[SKIFF_SAVES_HASH_SIZE];
    backup_hash(1, hash);
    TEST_ASSERT_EQUAL_STRING(old_hash, hash);
    assert_work_clean();
}

/* Nothing live: no backup, and the unit's folders are created. */
static void test_a_game_without_saves_gets_the_unit_and_no_backup(void) {
    put_new_unit();
    skiff_saves_restore_report report;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, &report));
    TEST_ASSERT_EQUAL_UINT32(0, report.backup);
    TEST_ASSERT_FALSE(exists(SKIFF_SAVES_BACKUP_FOLDER));
    assert_text(NEW_DATA00, SAVES "/" DATA00 "/DATA.BIN");
    assert_text(NEW_DATA02, SAVES "/" DATA02 "/DATA.BIN");
    assert_work_clean();
}

/* ---- Refused before anything is written ---- */

static void assert_nothing_changed(skiff_err expected, skiff_err err) {
    TEST_PRINTF("changes reaching the device: %d", storage.changes);
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(expected), skiff_err_name(err));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, storage.changes, "nothing is written before the checks pass");
    assert_old_save();
    assert_work_clean();
}

/* The stage, the new folders and the backup, plus the 8 MiB margin, must fit. */
static void test_a_full_memory_stick_is_refused_before_anything_is_written(void) {
    prepare();
    storage.has_free_bytes = 1;
    storage.free_bytes = SKIFF_STORAGE_FREE_MARGIN_BYTES;
    assert_nothing_changed(SKIFF_ERR_STORAGE_NO_SPACE, restore_on(&storage.base, NULL));
    TEST_ASSERT_EQUAL_INT(1, storage.free_space_queries);
}

static void test_a_device_that_cannot_tell_its_free_space_is_not_refused(void) {
    prepare();
    storage.free_space_error = SKIFF_ERR_NOT_IMPLEMENTED;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, NULL));
    assert_new_save();
}

static void test_a_damaged_unit_is_refused_before_anything_is_written(void) {
    prepare();
    put_text(UNIT_PATH, "not a zip at all, just text that is long enough");
    assert_nothing_changed(SKIFF_ERR_STORAGE_SAVE_DAMAGED, restore_on(&storage.base, NULL));
}

static void test_another_games_unit_is_refused(void) {
    prepare();
    const skiff_err err = skiff_saves_restore(&storage.base, &roots, UNIT_PATH, OTHER_GAME, NULL);
    assert_nothing_changed(SKIFF_ERR_STORAGE_SAVE_DAMAGED, err);
}

/* A live save folder holding a folder: it could not be backed up as a unit. */
static void test_live_folders_no_unit_may_hold_are_refused(void) {
    prepare();
    put_folder(SAVES "/" DATA01 "/NESTED");
    const skiff_err err = restore_on(&storage.base, NULL);
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE),
                             skiff_err_name(err));
    TEST_ASSERT_EQUAL_INT(0, storage.changes);
    assert_text(OLD_DATA01, SAVES "/" DATA01 "/DATA.BIN");
}

/* A file where the unit needs a folder: step 5 could never create it, so a journal would never
 * finish. */
static void test_a_file_in_place_of_a_unit_folder_is_refused(void) {
    prepare();
    put_text(SAVES "/" DATA02, "a file, not a folder");
    const skiff_err err = restore_on(&storage.base, NULL);
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE),
                             skiff_err_name(err));
    TEST_ASSERT_EQUAL_INT(0, storage.changes);
    assert_text("a file, not a folder", SAVES "/" DATA02);
    assert_text(OLD_DATA00, SAVES "/" DATA00 "/DATA.BIN");
    assert_text(OLD_DATA01, SAVES "/" DATA01 "/DATA.BIN");
    assert_work_clean();
}

static void test_restore_rejects_bad_arguments(void) {
    skiff_saves_restore_report report;
    memset(&report, 0xA5, sizeof report);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_restore(NULL, &roots, UNIT_PATH, GAME, &report));
    TEST_ASSERT_EQUAL_UINT32(0, report.backup);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_restore(&storage.base, NULL, UNIT_PATH, GAME, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_restore(&storage.base, &roots, NULL, GAME, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_restore(&storage.base, &roots, UNIT_PATH, "bad/id", NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_recover(NULL, &roots, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_recover(&storage.base, NULL, NULL));
}

static void test_work_unit_path_names_the_game(void) {
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_work_unit_path(GAME, path, sizeof path));
    TEST_ASSERT_EQUAL_STRING(UNIT_PATH, path);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_work_unit_path("../x", path, sizeof path));
    TEST_ASSERT_EQUAL_STRING("", path);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_work_unit_path(GAME, NULL, 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_saves_work_unit_path(GAME, path, strlen(UNIT_PATH)));
    TEST_ASSERT_EQUAL_STRING("", path);
}

/* ---- Cut short ---- */

/*
 * The power goes after each change the restore makes in turn (the first file the stage gets, the
 * backup, each step of the journal's crash-safe write, each file of each folder in step 5, the
 * done mark, the stage's removal); the recovery then runs on the device as it was left. The game
 * must hold the old save or the new one, whole, never a mix and never neither; once a cut gives
 * the new save, every later cut must too; and the work folder is clean afterwards.
 */
static void test_a_cut_after_any_change_ends_in_the_old_save_or_the_new_one(void) {
    prepare();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, NULL));
    const int total = storage.changes;
    TEST_PRINTF("a whole restore makes %d changes", total);
    TEST_ASSERT_GREATER_THAN_INT(20, total);
    int old_seen = 0;
    int new_seen = 0;
    for (int cut = 1; cut <= total; cut++) {
        tearDown();
        setUp();
        prepare();
        storage.cut_after_changes = cut;
        const skiff_err err = restore_on(&storage.base, NULL);
        skiff_saves_recovery recovery;
        const skiff_err recovered = recover_on(posix, &recovery);
        char hash[SKIFF_SAVES_HASH_SIZE];
        live_hash(hash);
        const int is_new = strcmp(hash, new_hash) == 0;
        TEST_PRINTF("cut after %d of %d: restore %s, recover %s, live %s", cut, total,
                    skiff_err_name(err), skiff_err_name(recovered), is_new ? "new" : "old");
        TEST_ASSERT_TRUE_MESSAGE(recovered == SKIFF_OK ||
                                     recovered == SKIFF_ERR_STORAGE_SAVE_RESTORE_FINISHED,
                                 "a complete stage is always applied");
        if (is_new) {
            assert_new_save();
            new_seen = 1;
        } else {
            TEST_ASSERT_FALSE_MESSAGE(new_seen, "a later cut never goes back to the old save");
            assert_old_save();
            old_seen = 1;
        }
        TEST_ASSERT_TRUE_MESSAGE(recovered != SKIFF_ERR_STORAGE_SAVE_RESTORE_FINISHED || is_new,
                                 "a finished recovery leaves the new save");
        assert_work_clean();
        skiff_saves_recovery again;
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, recover_on(posix, &again));
        TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_RECOVERY_NONE, again.outcome);
    }
    TEST_ASSERT_TRUE(old_seen);
    TEST_ASSERT_TRUE(new_seen);
}

/* A suspend during step 5 invalidates the files it has open: the copy is reopened and redone. */
static void test_a_suspend_during_the_copy_reopens_the_files(void) {
    prepare();
    /* DATA.BIN's writes: two into the stage, then the first copy into the live folder. */
    storage.fail_suffix = "/DATA.BIN";
    storage.stale_after_writes = 3;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, NULL));
    TEST_ASSERT_EQUAL_INT(1, storage.handles_lost);
    assert_new_save();
    assert_work_clean();
}

/* A suspend while the stage is written: the extraction is undone and done again. */
static void test_a_suspend_during_the_extraction_extracts_again(void) {
    prepare();
    storage.fail_suffix = "/DATA.BIN";
    storage.stale_after_writes = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, NULL));
    TEST_ASSERT_EQUAL_INT(1, storage.handles_lost);
    assert_new_save();
}

/* A write error that does not go away in step 5 (the stick filled up): the restore fails with it,
 * the journal stays, and the recovery finishes it once the device works. */
static void test_a_failure_in_step_5_is_finished_by_the_recovery(void) {
    prepare();
    storage.fail_suffix = "/EXTRA.BIN";
    storage.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    storage.write_budget = sizeof NEW_EXTRA02 - 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, restore_on(&storage.base, NULL));
    TEST_ASSERT_TRUE_MESSAGE(exists(SKIFF_SAVES_JOURNAL_PATH), "the journal waits for a recovery");
    TEST_ASSERT_FALSE_MESSAGE(exists(SAVES "/" DATA02 "/EXTRA.BIN"), "a failed copy is removed");
    skiff_saves_recovery recovery;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, recover_on(&storage.base, &recovery));
    TEST_ASSERT_TRUE(exists(SKIFF_SAVES_JOURNAL_PATH));
    storage.write_error = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_SAVE_RESTORE_FINISHED,
                          recover_on(&storage.base, &recovery));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_RECOVERY_FINISHED, recovery.outcome);
    TEST_ASSERT_EQUAL_STRING(GAME, recovery.save_target);
    TEST_ASSERT_EQUAL_UINT32(1, recovery.backup);
    assert_new_save();
    assert_work_clean();
}

/* A restore finds an earlier one cut short (another game's): it finishes that one first. */
static void test_a_restore_finishes_a_pending_one_first(void) {
    prepare();
    storage.fail_suffix = "/EXTRA.BIN";
    storage.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    storage.write_budget = sizeof NEW_EXTRA02 - 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, restore_on(&storage.base, NULL));
    storage.write_error = SKIFF_OK;
    put_folder(SOURCE "/" OTHER_DATA);
    put_text(SOURCE "/" OTHER_DATA "/DATA.BIN", "the other game's new save");
    char other_unit[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_work_unit_path(OTHER_GAME, other_unit, sizeof other_unit));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_collect(posix, &roots, SOURCE, OTHER_GAME, unit));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_write_unit(posix, &roots, unit, other_unit));
    skiff_saves_restore_report report;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_saves_restore(&storage.base, &roots, other_unit, OTHER_GAME, &report));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_SAVE_RESTORE_FINISHED, report.recovered);
    assert_text("the other game's new save", SAVES "/" OTHER_DATA "/DATA.BIN");
    assert_text(NEW_DATA00, SAVES "/" DATA00 "/DATA.BIN");
    assert_text(NEW_EXTRA02, SAVES "/" DATA02 "/EXTRA.BIN");
    assert_work_clean();
}

/* A pending restore that cannot be finished makes the next one fail with its error. */
static void test_a_restore_fails_while_a_pending_one_cannot_finish(void) {
    prepare();
    storage.fail_suffix = "/EXTRA.BIN";
    storage.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    storage.write_budget = sizeof NEW_EXTRA02 - 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, restore_on(&storage.base, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, restore_on(&storage.base, NULL));
    TEST_ASSERT_TRUE(exists(SKIFF_SAVES_JOURNAL_PATH));
}

/* ---- The recovery's own cases ---- */

static void test_recover_with_nothing_pending_does_nothing(void) {
    put_live();
    skiff_saves_recovery recovery;
    memset(&recovery, 0xA5, sizeof recovery);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, recover_on(&storage.base, &recovery));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_RECOVERY_NONE, recovery.outcome);
    TEST_ASSERT_EQUAL_STRING("", recovery.save_target);
    TEST_ASSERT_EQUAL_INT(0, storage.changes);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_recover(&storage.base, &roots, NULL));
    assert_others_untouched();
}

/* A stage without a journal: a restore cut before step 5. It goes; the saves stay. */
static void test_recover_removes_a_stage_left_without_a_journal(void) {
    put_live();
    put_folder(SKIFF_SAVES_STAGE_FOLDER "/" DATA00);
    put_text(SKIFF_SAVES_STAGE_FOLDER "/" DATA00 "/DATA.BIN", NEW_DATA00);
    put_text(SKIFF_SAVES_STAGE_FOLDER "/stray", "x");
    skiff_saves_recovery recovery;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, recover_on(&storage.base, &recovery));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_RECOVERY_NONE, recovery.outcome);
    assert_text(OLD_DATA00, SAVES "/" DATA00 "/DATA.BIN");
    assert_work_clean();
}

/* Brings a restore to the point where step 5 is about to start, and stops it there. */
static void restore_up_to_step_5(void) {
    prepare();
    /* Step 5 starts by looking at the first live folder it replaces. */
    storage.fail_suffix = "SAVEDATA/" DATA00;
    storage.stat_error = SKIFF_ERR_STORAGE_NO_MEDIA;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_MEDIA, restore_on(&storage.base, NULL));
    storage.fail_suffix = NULL;
    storage.stat_error = SKIFF_OK;
    assert_old_save();
    TEST_ASSERT_TRUE(exists(SKIFF_SAVES_JOURNAL_PATH));
}

static void test_a_damaged_journal_leaves_the_saves_and_reports_it(void) {
    restore_up_to_step_5();
    char journal[SKIFF_STORAGE_PATH_MAX];
    resolve(SKIFF_SAVES_JOURNAL_PATH, journal);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_replace_whole(posix, journal, "{\"version\":1,",
                                                                strlen("{\"version\":1,")));
    char before[SKIFF_SAVES_HASH_SIZE];
    live_hash(before);
    skiff_saves_recovery recovery;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED,
                          recover_on(&storage.base, &recovery));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_RECOVERY_UNFINISHED, recovery.outcome);
    TEST_ASSERT_EQUAL_STRING("", recovery.save_target);
    char after[SKIFF_SAVES_HASH_SIZE];
    live_hash(after);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(before, after, "the live saves are left as they are");
    TEST_ASSERT_TRUE_MESSAGE(exists(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/1.zip"),
                             "the backup is kept");
    assert_work_clean();
}

static void test_a_journal_whose_stage_is_gone_is_given_up(void) {
    restore_up_to_step_5();
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(SKIFF_SAVES_STAGE_FOLDER "/" DATA02 "/EXTRA.BIN", path);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(posix, path));
    skiff_saves_recovery recovery;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED,
                          recover_on(&storage.base, &recovery));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_RECOVERY_UNFINISHED, recovery.outcome);
    TEST_ASSERT_EQUAL_STRING(GAME, recovery.save_target);
    TEST_ASSERT_EQUAL_UINT32(1, recovery.backup);
    assert_old_save();
    assert_work_clean();
}

static void test_a_staged_file_of_another_size_is_given_up(void) {
    restore_up_to_step_5();
    put_text(SKIFF_SAVES_STAGE_FOLDER "/" DATA02 "/EXTRA.BIN", "cut");
    skiff_saves_recovery recovery;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED,
                          recover_on(&storage.base, &recovery));
    assert_old_save();
    assert_work_clean();
}

static void test_a_journal_left_before_step_5_is_finished(void) {
    restore_up_to_step_5();
    skiff_saves_recovery recovery;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_SAVE_RESTORE_FINISHED,
                          recover_on(&storage.base, &recovery));
    assert_new_save();
    assert_work_clean();
}

/* Journals that are not Skiff's are refused whole, never half applied. */
static void test_journals_skiff_did_not_write_are_given_up(void) {
    static const char *const JOURNALS[] = {
        "[]",
        "{\"version\":2,\"done\":true}",
        "{\"version\":1,\"save_target\":\"bad/x\",\"backup\":0,\"replace\":[],\"remove\":[],"
        "\"files\":[]}",
        "{\"version\":1,\"save_target\":\"" GAME
        "\",\"backup\":0,\"replace\":[{\"stage\":\"" OTHER_DATA "\",\"live\":\"" OTHER_DATA
        "\"}],\"remove\":[],\"files\":[]}",
        "{\"version\":1,\"save_target\":\"" GAME "\",\"backup\":0,\"replace\":[{\"stage\":\"" DATA00
        "\",\"live\":\"" DATA00 "\"},{\"stage\":\"" DATA00 "\",\"live\":\"" DATA00
        "\"}],\"remove\":[],\"files\":[]}",
        "{\"version\":1,\"save_target\":\"" GAME
        "\",\"backup\":0,\"replace\":[],\"remove\":[\"..\"],"
        "\"files\":[]}",
        "{\"version\":1,\"save_target\":\"" GAME "\",\"backup\":0,\"replace\":[],\"remove\":[7],"
        "\"files\":[]}",
        "{\"version\":1,\"save_target\":\"" GAME "\",\"backup\":0,\"replace\":[],\"remove\":[],"
        "\"files\":[{\"name\":\"" DATA00 "/DATA.BIN\",\"size\":1}]}",
        "{\"version\":1,\"save_target\":\"" GAME "\",\"backup\":0,\"replace\":[{\"stage\":\"" DATA00
        "\",\"live\":\"" DATA00 "\"}],\"remove\":[],\"files\":[{\"name\":\"" DATA00
        "/a:b\",\"size\":1}]}",
        "{\"version\":1,\"save_target\":\"" GAME "\",\"backup\":0,\"replace\":[{\"stage\":\"" DATA00
        "\",\"live\":\"" DATA00 "\"}],\"remove\":[],\"files\":[{\"name\":\"" DATA00
        "/DATA.BIN\",\"size\":-1}]}",
        "{\"version\":1,\"save_target\":\"" GAME
        "\",\"backup\":0,\"replace\":[\"x\"],\"remove\":[],"
        "\"files\":[]}",
        "{\"version\":1,\"save_target\":\"" GAME "\",\"backup\":0,\"replace\":[],\"remove\":[],"
        "\"files\":[{\"name\":\"nofolder\",\"size\":1}]}",
        "{\"version\":1,\"save_target\":\"" GAME "\"} trailing",
    };
    for (size_t i = 0; i < sizeof JOURNALS / sizeof JOURNALS[0]; i++) {
        tearDown();
        setUp();
        put_live();
        put_folder(SKIFF_SAVES_WORK_FOLDER);
        put_text(SKIFF_SAVES_JOURNAL_PATH, JOURNALS[i]);
        TEST_PRINTF("journal %zu: %s", i, JOURNALS[i]);
        skiff_saves_recovery recovery;
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED,
                              recover_on(&storage.base, &recovery));
        assert_text(OLD_DATA00, SAVES "/" DATA00 "/DATA.BIN");
        assert_text(OLD_DATA01, SAVES "/" DATA01 "/DATA.BIN");
        assert_work_clean();
    }
}

/* A journal larger than any Skiff writes is damaged too. */
static void test_an_oversized_journal_is_given_up(void) {
    put_live();
    put_folder(SKIFF_SAVES_WORK_FOLDER);
    char *text = malloc(SKIFF_SAVES_JOURNAL_BYTES_MAX + 1);
    TEST_ASSERT_NOT_NULL(text);
    memset(text, ' ', SKIFF_SAVES_JOURNAL_BYTES_MAX + 1);
    put_file(SKIFF_SAVES_JOURNAL_PATH, text, SKIFF_SAVES_JOURNAL_BYTES_MAX + 1);
    free(text);
    skiff_saves_recovery recovery;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED,
                          recover_on(&storage.base, &recovery));
    assert_work_clean();
}

/* A journal marked done: step 5 finished, only the stage and the journal are removed. */
static void test_a_done_journal_only_cleans_up(void) {
    put_live();
    put_folder(SKIFF_SAVES_STAGE_FOLDER "/" DATA00);
    put_text(SKIFF_SAVES_STAGE_FOLDER "/" DATA00 "/DATA.BIN", NEW_DATA00);
    put_text(SKIFF_SAVES_JOURNAL_PATH, "{\"version\":1,\"done\":true}");
    skiff_saves_recovery recovery;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, recover_on(&storage.base, &recovery));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_RECOVERY_NONE, recovery.outcome);
    assert_text(OLD_DATA00, SAVES "/" DATA00 "/DATA.BIN");
    assert_work_clean();
}

/* The journal cannot be read now (the device fails): nothing is decided, it stays. */
static void test_a_journal_that_cannot_be_read_stays(void) {
    restore_up_to_step_5();
    storage.fail_suffix = "journal";
    storage.read_error = SKIFF_ERR_STORAGE_IO;
    skiff_saves_recovery recovery;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, recover_on(&storage.base, &recovery));
    TEST_ASSERT_TRUE(exists(SKIFF_SAVES_JOURNAL_PATH));
    storage.read_error = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_SAVE_RESTORE_FINISHED,
                          recover_on(&storage.base, &recovery));
    assert_new_save();
}

/* ---- Backups ---- */

static void restore_text(const char *text) {
    put_folder(SOURCE "/" DATA00);
    put_text(SOURCE "/" DATA00 "/DATA.BIN", text);
    write_source_unit();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, NULL));
}

static void list_backups(skiff_saves_backup_list *list) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_backups_list(&storage.base, &roots, GAME, list));
    for (size_t i = 0; i < list->count; i++) {
        TEST_PRINTF("backup %u: %llu bytes, mtime %lld", (unsigned)list->backups[i].number,
                    (unsigned long long)list->backups[i].size, (long long)list->backups[i].mtime);
    }
}

static void test_a_game_keeps_its_three_newest_backups(void) {
    put_live();
    static const char *const TEXTS[] = {"one", "two", "three", "four", "five"};
    for (size_t i = 0; i < sizeof TEXTS / sizeof TEXTS[0]; i++) {
        restore_text(TEXTS[i]);
    }
    skiff_saves_backup_list list;
    list_backups(&list);
    TEST_ASSERT_EQUAL_size_t(SKIFF_SAVES_BACKUPS_PER_GAME, list.count);
    TEST_ASSERT_EQUAL_UINT32(5, list.backups[0].number);
    TEST_ASSERT_EQUAL_UINT32(4, list.backups[1].number);
    TEST_ASSERT_EQUAL_UINT32(3, list.backups[2].number);
    TEST_ASSERT_GREATER_THAN_UINT64(0, list.backups[0].size);
    TEST_ASSERT_NOT_EQUAL_INT64(SKIFF_STORAGE_MTIME_UNKNOWN, list.backups[0].mtime);
    TEST_ASSERT_FALSE(exists(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/1.zip"));
    TEST_ASSERT_FALSE(exists(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/2.zip"));
    /* Backup 5 holds what restore 5 replaced: "four". */
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_restore_backup(&storage.base, &roots, GAME, 5, NULL));
    assert_text("four", SAVES "/" DATA00 "/DATA.BIN");
}

static void put_big_backup(const char *game, const char *name) {
    char folder[SKIFF_STORAGE_PATH_MAX];
    snprintf(folder, sizeof folder, "%s/%s", SKIFF_SAVES_BACKUP_FOLDER, game);
    put_folder(folder);
    char logical[SKIFF_STORAGE_PATH_MAX];
    snprintf(logical, sizeof logical, "%s/%s/%s", SKIFF_SAVES_BACKUP_FOLDER, game, name);
    char path[SKIFF_STORAGE_PATH_MAX];
    resolve(logical, path);
    unsigned char *chunk = calloc(1, CHUNK_BYTES);
    TEST_ASSERT_NOT_NULL(chunk);
    skiff_file *file = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_open(posix, path, SKIFF_FILE_REPLACE, 0, &file));
    for (size_t written = 0; written < BIG_BACKUP_BYTES; written += CHUNK_BYTES) {
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, chunk, CHUNK_BYTES));
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    free(chunk);
}

/* Three games' backups of 11 MiB pass 32 MiB once the restore adds its own: the oldest across
 * every game goes (and its emptied game folder), never the one just made; a draft a cut backup
 * left goes too, and a backup listed in capitals counts. */
static void test_backups_past_32_mib_drop_the_oldest_of_every_game(void) {
    put_big_backup("SKIF00011", "1.zip");
    put_big_backup("SKIF00012", "2.ZIP");
    put_big_backup("SKIF00013", "3.zip");
    put_folder(SKIFF_SAVES_BACKUP_FOLDER "/" GAME);
    put_text(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/9.zip.tmp", "a cut backup");
    put_text(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/notes.txt", "not Skiff's");
    put_text(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/012.zip", "not Skiff's either");
    prepare();
    skiff_saves_restore_report report;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, &report));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4, report.backup, "numbers count on from every game's");
    TEST_ASSERT_FALSE(exists(SKIFF_SAVES_BACKUP_FOLDER "/SKIF00011/1.zip"));
    TEST_ASSERT_FALSE_MESSAGE(exists(SKIFF_SAVES_BACKUP_FOLDER "/SKIF00011"),
                              "an emptied game folder goes");
    TEST_ASSERT_TRUE(exists(SKIFF_SAVES_BACKUP_FOLDER "/SKIF00012/2.ZIP"));
    TEST_ASSERT_TRUE(exists(SKIFF_SAVES_BACKUP_FOLDER "/SKIF00013/3.zip"));
    TEST_ASSERT_TRUE(exists(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/4.zip"));
    TEST_ASSERT_FALSE(exists(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/9.zip.tmp"));
    TEST_ASSERT_TRUE(exists(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/notes.txt"));
    TEST_ASSERT_TRUE(exists(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/012.zip"));
    skiff_saves_backup_list list;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_backups_list(&storage.base, &roots, "SKIF00012", &list));
    TEST_ASSERT_EQUAL_size_t(1, list.count);
    TEST_ASSERT_EQUAL_UINT32(2, list.backups[0].number);
    TEST_ASSERT_EQUAL_UINT64(BIG_BACKUP_BYTES, list.backups[0].size);
}

/* The backup a restore just made is never dropped for the total cap, even when it is the oldest
 * (here, by number), so the save it holds survives the restore that replaced it. */
static void test_the_backup_just_made_is_never_dropped(void) {
    put_big_backup("SKIF00011", "1.zip");
    put_big_backup("SKIF00012", "2.zip");
    put_big_backup("SKIF00013", "3.zip");
    saves_backups_prune(&storage.base, &roots, 1);
    TEST_ASSERT_TRUE(exists(SKIFF_SAVES_BACKUP_FOLDER "/SKIF00011/1.zip"));
    TEST_ASSERT_FALSE(exists(SKIFF_SAVES_BACKUP_FOLDER "/SKIF00012/2.zip"));
    TEST_ASSERT_TRUE(exists(SKIFF_SAVES_BACKUP_FOLDER "/SKIF00013/3.zip"));
}

/* A backup is a unit: restoring it goes through the same restore, which backs up first. */
static void test_restoring_a_backup_brings_the_old_save_back(void) {
    prepare();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, restore_on(&storage.base, NULL));
    skiff_saves_restore_report report;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_restore_backup(&storage.base, &roots, GAME, 1, &report));
    TEST_ASSERT_EQUAL_UINT32(2, report.backup);
    assert_old_save();
    char hash[SKIFF_SAVES_HASH_SIZE];
    backup_hash(2, hash);
    TEST_ASSERT_EQUAL_STRING(new_hash, hash);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND,
                          skiff_saves_restore_backup(&storage.base, &roots, GAME, 7, &report));
    put_folder(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/8.zip");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND,
                          skiff_saves_restore_backup(&storage.base, &roots, GAME, 8, &report));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_restore_backup(&storage.base, &roots, GAME, 0, &report));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_restore_backup(NULL, &roots, GAME, 1, &report));
}

/* A backup that fails to write leaves no file under a backup's name, and nothing is restored. */
static void test_a_failed_backup_stops_the_restore(void) {
    prepare();
    storage.fail_suffix = ".zip.tmp";
    storage.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, restore_on(&storage.base, NULL));
    TEST_ASSERT_FALSE(exists(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/1.zip.tmp"));
    TEST_ASSERT_FALSE(exists(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/1.zip"));
    assert_old_save();
    assert_work_clean();
}

/* The journal's crash-safe write failing: nothing applied, the stage removed. */
static void test_a_failed_journal_write_stops_the_restore(void) {
    prepare();
    storage.fail_suffix = "journal.tmp";
    storage.rename_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, restore_on(&storage.base, NULL));
    assert_old_save();
    assert_work_clean();
}

static void test_backup_helpers_check_their_arguments(void) {
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_backup_path(GAME, 12, path, sizeof path));
    TEST_ASSERT_EQUAL_STRING(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/12.zip", path);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_backup_path(GAME, 0, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_saves_backup_path(GAME, SKIFF_SAVES_BACKUP_NUMBER_MAX + 1, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_backup_path("x", 1, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL, skiff_saves_backup_path(GAME, 1, path, 8));
    TEST_ASSERT_EQUAL_STRING("", path);
    skiff_saves_backup_list list;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_backups_list(&storage.base, &roots, GAME, &list));
    TEST_ASSERT_EQUAL_size_t(0, list.count);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_backups_list(NULL, &roots, GAME, &list));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_backups_list(&storage.base, &roots, "bad", &list));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_backups_list(&storage.base, &roots, GAME, NULL));
    put_folder(SKIFF_SAVES_BACKUP_FOLDER "/" GAME);
    put_text(SKIFF_SAVES_BACKUP_FOLDER "/" GAME "/1.zip", "x");
    storage.list_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO,
                          skiff_saves_backups_list(&storage.base, &roots, GAME, &list));
    TEST_ASSERT_EQUAL_size_t(0, list.count);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_restore_replaces_removes_and_adds_folders_after_a_backup);
    RUN_TEST(test_folders_reach_rmdir_only_once_emptied);
    RUN_TEST(test_a_restore_of_the_same_unit_twice_ends_the_same);
    RUN_TEST(test_live_folders_match_the_unit_ignoring_case);
    RUN_TEST(test_an_install_is_never_touched_even_when_the_unit_carries_it);
    RUN_TEST(test_an_install_is_found_among_hundreds_of_other_folders);
    RUN_TEST(test_an_empty_unit_removes_the_save_folders_after_a_backup);
    RUN_TEST(test_a_game_without_saves_gets_the_unit_and_no_backup);
    RUN_TEST(test_a_full_memory_stick_is_refused_before_anything_is_written);
    RUN_TEST(test_a_device_that_cannot_tell_its_free_space_is_not_refused);
    RUN_TEST(test_a_damaged_unit_is_refused_before_anything_is_written);
    RUN_TEST(test_another_games_unit_is_refused);
    RUN_TEST(test_live_folders_no_unit_may_hold_are_refused);
    RUN_TEST(test_a_file_in_place_of_a_unit_folder_is_refused);
    RUN_TEST(test_restore_rejects_bad_arguments);
    RUN_TEST(test_work_unit_path_names_the_game);
    RUN_TEST(test_a_cut_after_any_change_ends_in_the_old_save_or_the_new_one);
    RUN_TEST(test_a_suspend_during_the_copy_reopens_the_files);
    RUN_TEST(test_a_suspend_during_the_extraction_extracts_again);
    RUN_TEST(test_a_failure_in_step_5_is_finished_by_the_recovery);
    RUN_TEST(test_a_restore_finishes_a_pending_one_first);
    RUN_TEST(test_a_restore_fails_while_a_pending_one_cannot_finish);
    RUN_TEST(test_recover_with_nothing_pending_does_nothing);
    RUN_TEST(test_recover_removes_a_stage_left_without_a_journal);
    RUN_TEST(test_a_damaged_journal_leaves_the_saves_and_reports_it);
    RUN_TEST(test_a_journal_whose_stage_is_gone_is_given_up);
    RUN_TEST(test_a_staged_file_of_another_size_is_given_up);
    RUN_TEST(test_a_journal_left_before_step_5_is_finished);
    RUN_TEST(test_journals_skiff_did_not_write_are_given_up);
    RUN_TEST(test_an_oversized_journal_is_given_up);
    RUN_TEST(test_a_done_journal_only_cleans_up);
    RUN_TEST(test_a_journal_that_cannot_be_read_stays);
    RUN_TEST(test_a_game_keeps_its_three_newest_backups);
    RUN_TEST(test_backups_past_32_mib_drop_the_oldest_of_every_game);
    RUN_TEST(test_the_backup_just_made_is_never_dropped);
    RUN_TEST(test_restoring_a_backup_brings_the_old_save_back);
    RUN_TEST(test_a_failed_backup_stops_the_restore);
    RUN_TEST(test_a_failed_journal_write_stops_the_restore);
    RUN_TEST(test_backup_helpers_check_their_arguments);
    return UNITY_END();
}
