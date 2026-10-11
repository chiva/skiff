/*
 * The storage seam (skiff/storage.h): the argument checks every implementation gets for free, and
 * the POSIX implementation host tests run on, including the rules it keeps from the PSP (no rename
 * over an existing file, writes from an offset that leave the rest of the file alone), folder
 * listings, stat and folder removal. Also the fake that injects Memory Stick failures
 * (tests/support/fake_storage.h).
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "skiff/storage.h"

#include "fake_storage.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define CONTENT "0123456789"
#define CONTENT_BYTES 10U

static char dir[TEMP_DIR_PATH_MAX];
static char path[TEMP_DIR_PATH_MAX];
static char other[TEMP_DIR_PATH_MAX];
static skiff_storage *storage;

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    TEST_ASSERT_TRUE(temp_dir_path(dir, "file.bin", path, sizeof path));
    TEST_ASSERT_TRUE(temp_dir_path(dir, "other.bin", other, sizeof other));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&storage));
    TEST_PRINTF("temporary directory %s", dir);
}

void tearDown(void) {
    skiff_storage_destroy(storage);
    temp_dir_remove(dir);
}

static void write_file(const char *target, const char *text) {
    skiff_file *file = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(storage, target, SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, text, strlen(text)));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_sync(file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
}

/* The whole file as text (it must fit in out). */
static void read_file(const char *target, char *out, size_t out_size) {
    skiff_file *file = NULL;
    size_t used = 0;
    size_t got = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_open(storage, target, SKIFF_FILE_READ, 0, &file));
    do {
        TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                              skiff_file_read(file, out + used, out_size - 1 - used, &got));
        used += got;
    } while (got > 0 && used < out_size - 1);
    out[used] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
}

static void test_replace_write_read_and_size(void) {
    char text[32];
    uint64_t size = 0;
    write_file(path, CONTENT);
    read_file(path, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING(CONTENT, text);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_size(storage, path, &size));
    TEST_ASSERT_EQUAL_UINT64(CONTENT_BYTES, size);
    write_file(path, "ab");
    read_file(path, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ab", text, "REPLACE empties the file first");
}

static void test_write_at_overwrites_from_the_offset_and_keeps_the_rest(void) {
    char text[32];
    skiff_file *file = NULL;
    write_file(path, CONTENT);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(storage, path, SKIFF_FILE_WRITE_AT, 4, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, "xy", 2));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    read_file(path, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING("0123xy6789", text);

    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_storage_open(storage, path, SKIFF_FILE_WRITE_AT, CONTENT_BYTES, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, "!", 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    read_file(path, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("0123xy6789!", text, "writing at the end appends");
}

static void test_write_at_past_the_end_or_on_a_missing_file_is_refused(void) {
    skiff_file *file = NULL;
    write_file(path, CONTENT);
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_storage_open(storage, path, SKIFF_FILE_WRITE_AT, CONTENT_BYTES + 1, &file));
    TEST_ASSERT_NULL(file);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND,
                          skiff_storage_open(storage, other, SKIFF_FILE_WRITE_AT, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND,
                          skiff_storage_open(storage, other, SKIFF_FILE_READ, 0, &file));
}

static void test_read_starts_at_an_offset_within_the_file(void) {
    skiff_file *file = NULL;
    char text[CONTENT_BYTES + 1];
    size_t got = 0;
    write_file(path, CONTENT);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_open(storage, path, SKIFF_FILE_READ, 4, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_read(file, text, sizeof text, &got));
    text[got] = '\0';
    TEST_PRINTF("from offset 4: \"%s\"", text);
    TEST_ASSERT_EQUAL_STRING("456789", text);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(storage, path, SKIFF_FILE_READ, CONTENT_BYTES, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_read(file, text, sizeof text, &got));
    TEST_ASSERT_EQUAL_size_t(0, got);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_open(storage, path, SKIFF_FILE_READ,
                                                                    CONTENT_BYTES + 1, &file));
    TEST_ASSERT_NULL(file);
}

static void test_missing_files_are_not_found(void) {
    uint64_t size = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_storage_size(storage, path, &size));
    TEST_ASSERT_EQUAL_UINT64(0, size);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_storage_remove(storage, path));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_storage_rename(storage, path, other));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_STORAGE_IO, skiff_storage_size(storage, dir, &size),
                                  "a directory is not a file");
}

static void test_rename_refuses_to_replace_and_remove_deletes(void) {
    char text[32];
    write_file(path, CONTENT);
    write_file(other, "old");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_STORAGE_IO, skiff_storage_rename(storage, path, other),
                                  "FAT cannot replace a file in one step");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(storage, other));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_rename(storage, path, other));
    read_file(other, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING(CONTENT, text);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_storage_remove(storage, path));
}

static void test_reading_past_the_end_gets_nothing(void) {
    char text[4];
    size_t got = 1;
    skiff_file *file = NULL;
    write_file(path, "");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_open(storage, path, SKIFF_FILE_READ, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_read(file, text, sizeof text, &got));
    TEST_ASSERT_EQUAL_size_t(0, got);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_read(file, text, 0, &got));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, NULL, 0));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_STORAGE_IO, skiff_file_write(file, "x", 1),
                                  "a file opened for reading cannot be written");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
}

static void test_arguments_are_checked_before_the_implementation(void) {
    skiff_file *file = NULL;
    uint64_t size = 0;
    size_t got = 0;
    skiff_storage no_ops = {NULL};
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_open(storage, path, SKIFF_FILE_REPLACE, 0, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_open(NULL, path, SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_open(&no_ops, path, SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_open(storage, "", SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_open(storage, NULL, SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_INVALID_ARG,
                                  skiff_storage_open(storage, path, SKIFF_FILE_REPLACE, 1, &file),
                                  "REPLACE takes no offset");
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_open(storage, path, (skiff_file_mode)99, 0, &file));
    TEST_ASSERT_NULL(file);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_file_read(NULL, &size, 1, &got));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_file_write(NULL, "x", 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_file_sync(NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_size(storage, path, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_size(storage, NULL, &size));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_rename(storage, path, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_remove(NULL, path));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_posix_storage_create(NULL));
    skiff_storage_destroy(NULL);
    skiff_storage_destroy(&no_ops);

    write_file(path, CONTENT);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_open(storage, path, SKIFF_FILE_READ, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_file_read(file, NULL, 1, &got));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_file_read(file, &size, 1, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_file_write(file, NULL, 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
}

static void test_fake_fills_up_mid_write_for_matching_paths_only(void) {
    fake_storage fake;
    char text[32];
    skiff_file *file = NULL;
    fake_storage_init(&fake, storage);
    fake.fail_suffix = ".part";
    fake.write_budget = 4;
    fake.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    TEST_ASSERT_TRUE(temp_dir_path(dir, "game.iso.part", other, sizeof other));

    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(&fake.base, other, SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, skiff_file_write(file, CONTENT, 6));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    read_file(other, text, sizeof text);
    TEST_PRINTF("after the budget ran out the file holds \"%s\"", text);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("0123", text, "what fit was written");

    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(&fake.base, path, SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_OK, skiff_file_write(file, CONTENT, CONTENT_BYTES),
                                  "other paths are not failed");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    TEST_ASSERT_EQUAL_INT(1, fake.writes);
    TEST_ASSERT_EQUAL_UINT64(4, fake.bytes_written);
    skiff_storage_destroy(&fake.base);
}

static void test_fake_loses_open_handles_and_fails_syncs_and_renames(void) {
    fake_storage fake;
    skiff_file *file = NULL;
    skiff_file *later = NULL;
    char text[4];
    size_t got = 0;
    fake_storage_init(&fake, storage);
    fake.stale_after_writes = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(&fake.base, path, SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, "ab", 2));
    TEST_ASSERT_EQUAL_INT(1, fake.handles_lost);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_file_write(file, "cd", 2));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_file_sync(file));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_file_read(file, text, sizeof text, &got));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_file_close(file));

    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(&fake.base, path, SKIFF_FILE_WRITE_AT, 2, &later));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_OK, skiff_file_write(later, "cd", 2),
                                  "a file opened afterwards works");
    fake.sync_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_file_sync(later));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(later));

    uint64_t size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_size(&fake.base, path, &size));
    TEST_ASSERT_EQUAL_UINT64(4, size);
    fake.rename_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_storage_rename(&fake.base, path, other));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(&fake.base, path));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND,
                          skiff_storage_open(&fake.base, path, SKIFF_FILE_READ, 0, &file));
    TEST_ASSERT_EQUAL_INT(3, fake.opens);
}

static int is_folder(const char *target) {
    struct stat status;
    return stat(target, &status) == 0 && S_ISDIR(status.st_mode);
}

static void test_mkdir_creates_a_folder_once_and_keeps_an_existing_one(void) {
    char folder[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO", folder, sizeof folder));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdir(storage, folder));
    TEST_ASSERT_TRUE(is_folder(folder));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_OK, skiff_storage_mkdir(storage, folder),
                                  "an existing folder is fine");
    write_file(path, CONTENT);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_STORAGE_IO, skiff_storage_mkdir(storage, path),
                                  "a file by that name is not a folder");
    char orphan[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "missing/child", orphan, sizeof orphan));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_storage_mkdir(storage, orphan));
}

static void test_mkdirs_creates_every_missing_folder(void) {
    char deep[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "PSP/SAVEDATA/ULUS10064DATA00/", deep, sizeof deep));
    TEST_PRINTF("mkdirs %s", deep);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(storage, deep));
    char check[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "PSP/SAVEDATA/ULUS10064DATA00", check, sizeof check));
    TEST_ASSERT_TRUE(is_folder(check));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_OK, skiff_storage_mkdirs(storage, deep),
                                  "running again finds them all in place");
    TEST_ASSERT_TRUE(temp_dir_path(dir, "PSP//GAME", deep, sizeof deep));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_OK, skiff_storage_mkdirs(storage, deep),
                                  "a doubled separator is one");

    write_file(path, CONTENT);
    TEST_ASSERT_TRUE(temp_dir_path(dir, "file.bin/below", deep, sizeof deep));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_STORAGE_IO, skiff_storage_mkdirs(storage, deep),
                                  "a file in the way stops it");

    char too_long[SKIFF_STORAGE_PATH_MAX + 1];
    memset(too_long, 'x', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_mkdirs(storage, too_long));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_mkdirs(storage, "ms0:"));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_mkdirs(storage, "ms0:/"));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_mkdirs(storage, "/"));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_mkdirs(storage, ""));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_mkdirs(NULL, deep));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_mkdir(storage, ""));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_mkdir(NULL, deep));
}

static void test_mkdirs_walks_a_device_path_from_its_first_folder(void) {
    fake_storage fake;
    fake_storage_init(&fake, storage);
    fake.mkdir_error = SKIFF_ERR_STORAGE_NO_MEDIA;
    TEST_PRINTF("\"ms0:/PSP/GAME\": the walk starts at ms0:/PSP, never at the device");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_MEDIA,
                          skiff_storage_mkdirs(&fake.base, "ms0:/PSP/GAME"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake.mkdirs, "stops at the first failure, ms0:/PSP");
    skiff_storage_destroy(&fake.base);
}

static void test_free_space_of_the_host_device(void) {
    uint64_t free_bytes = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_free_space(storage, dir, &free_bytes));
    TEST_PRINTF("free under %s: %llu bytes", dir, (unsigned long long)free_bytes);
    TEST_ASSERT_TRUE(free_bytes > 0);
    char missing[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "missing", missing, sizeof missing));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND,
                          skiff_storage_free_space(storage, missing, &free_bytes));
    TEST_ASSERT_EQUAL_UINT64(0, free_bytes);
    write_file(path, CONTENT);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_STORAGE_IO,
                                  skiff_storage_free_space(storage, path, &free_bytes),
                                  "a file is not a folder");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_free_space(storage, dir, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_free_space(storage, "", &free_bytes));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_free_space(NULL, dir, &free_bytes));
}

#define GAME_BYTES ((uint64_t)700 * 1024 * 1024)

/* The free space the fake reports from now on. */
static void report_free_bytes(fake_storage *fake, uint64_t free_bytes) {
    fake->has_free_bytes = 1;
    fake->free_bytes = free_bytes;
}

static void test_room_needs_the_file_plus_a_margin(void) {
    fake_storage fake;
    fake_storage_init(&fake, storage);
    report_free_bytes(&fake, GAME_BYTES + SKIFF_STORAGE_FREE_MARGIN_BYTES);
    TEST_PRINTF("free %llu: a 700 MiB game fits exactly with the margin",
                (unsigned long long)fake.free_bytes);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_check_room(&fake.base, dir, GAME_BYTES));
    report_free_bytes(&fake, GAME_BYTES + SKIFF_STORAGE_FREE_MARGIN_BYTES - 1);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE,
                          skiff_storage_check_room(&fake.base, dir, GAME_BYTES));
    report_free_bytes(&fake, 0);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, skiff_storage_check_room(&fake.base, dir, 0));

    TEST_PRINTF("4 GiB and up cannot be stored on FAT32, whatever the space");
    report_free_bytes(&fake, UINT64_MAX);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_check_room(&fake.base, dir, SKIFF_STORAGE_MAX_FILE_BYTES));
    const int queries = fake.free_space_queries;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_STORAGE_FILE_TOO_LARGE,
        skiff_storage_check_room(&fake.base, dir, SKIFF_STORAGE_MAX_FILE_BYTES + 1));
    TEST_ASSERT_EQUAL_INT_MESSAGE(queries, fake.free_space_queries, "refused without asking");
    skiff_storage_destroy(&fake.base);

    TEST_PRINTF("the device's own error comes through");
    char missing[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "missing", missing, sizeof missing));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND,
                          skiff_storage_check_room(storage, missing, 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_check_room(storage, dir, 1));
}

/* ---- Small files replaced whole ---- */

static int exists(const char *target) {
    uint64_t size = 0;
    return skiff_storage_size(storage, target, &size) == SKIFF_OK;
}

static void test_a_whole_file_round_trips_and_a_missing_one_reads_empty(void) {
    char text[32];
    size_t length = 99;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_read_whole(storage, path, text, sizeof text, &length));
    TEST_ASSERT_EQUAL_size_t(0, length);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_replace_whole(storage, path, CONTENT, CONTENT_BYTES));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_replace_whole(storage, path, "new", 3));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_read_whole(storage, path, text, sizeof text, &length));
    TEST_ASSERT_EQUAL_size_t(3, length);
    TEST_ASSERT_EQUAL_MEMORY("new", text, 3);
    char draft[TEMP_DIR_PATH_MAX + sizeof SKIFF_STORAGE_DRAFT_SUFFIX];
    char pending[TEMP_DIR_PATH_MAX + sizeof SKIFF_STORAGE_PENDING_SUFFIX];
    snprintf(draft, sizeof draft, "%s" SKIFF_STORAGE_DRAFT_SUFFIX, path);
    snprintf(pending, sizeof pending, "%s" SKIFF_STORAGE_PENDING_SUFFIX, path);
    TEST_ASSERT_FALSE(exists(draft));
    TEST_ASSERT_FALSE(exists(pending));
}

static void test_a_whole_file_too_big_for_the_buffer_is_refused(void) {
    write_file(path, CONTENT);
    char small[CONTENT_BYTES - 1];
    size_t length = 99;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_storage_read_whole(storage, path, small, sizeof small, &length));
    TEST_ASSERT_EQUAL_size_t(0, length);
}

static void test_whole_files_are_binary_and_fill_an_exact_buffer(void) {
    static const unsigned char data[] = {0x00, 0xFF, 'a', 0x00, '\n', 0x7F};
    unsigned char exact[sizeof data];
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_replace_whole(storage, path, data, sizeof data));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_read_whole(storage, path, exact, sizeof exact, &length));
    TEST_ASSERT_EQUAL_size_t(sizeof data, length);
    TEST_ASSERT_EQUAL_MEMORY(data, exact, sizeof data);
}

static void test_a_complete_pending_file_is_put_in_place(void) {
    char pending[TEMP_DIR_PATH_MAX + sizeof SKIFF_STORAGE_PENDING_SUFFIX];
    snprintf(pending, sizeof pending, "%s" SKIFF_STORAGE_PENDING_SUFFIX, path);
    TEST_PRINTF("cut after the old file was removed: only the complete .new file is left");
    write_file(pending, CONTENT);
    char text[32];
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_read_whole(storage, path, text, sizeof text, &length));
    TEST_ASSERT_EQUAL_size_t(CONTENT_BYTES, length);
    TEST_ASSERT_EQUAL_MEMORY(CONTENT, text, CONTENT_BYTES);
    TEST_ASSERT_FALSE(exists(pending));
}

static void test_whole_files_refuse_bad_arguments(void) {
    char text[8];
    size_t length = 0;
    char long_path[SKIFF_STORAGE_PATH_MAX];
    memset(long_path, 'p', sizeof long_path - 1);
    long_path[sizeof long_path - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_read_whole(storage, long_path, text, sizeof text, &length));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_read_whole(NULL, path, text, sizeof text, &length));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_read_whole(storage, path, NULL, sizeof text, &length));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_replace_whole(storage, long_path, "x", 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_replace_whole(storage, path, NULL, 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_replace_whole(storage, path, NULL, 0));
}

/* ---- Folders: listing, stat, removal ---- */

enum { LISTED_MAX = 8 };

typedef struct listing {
    skiff_storage_entry entries[LISTED_MAX];
    int count;
    /* The callback stops the listing once it has seen this many entries; 0 for never. */
    int stop_after;
} listing;

static skiff_storage_list_step collect(void *ctx, const skiff_storage_entry *entry) {
    listing *seen = ctx;
    TEST_PRINTF("  listed \"%s\": %s, %llu bytes, mtime %lld", entry->name,
                entry->info.is_dir ? "folder" : "file", (unsigned long long)entry->info.size,
                (long long)entry->info.mtime);
    if (seen->count < LISTED_MAX) {
        seen->entries[seen->count] = *entry;
    }
    seen->count++;
    return seen->stop_after > 0 && seen->count >= seen->stop_after ? SKIFF_STORAGE_LIST_STOP
                                                                   : SKIFF_STORAGE_LIST_CONTINUE;
}

static const skiff_storage_entry *listed(const listing *seen, const char *name) {
    for (int i = 0; i < seen->count && i < LISTED_MAX; i++) {
        if (strcmp(seen->entries[i].name, name) == 0) {
            return &seen->entries[i];
        }
    }
    return NULL;
}

static skiff_err list_dir(skiff_storage *target, const char *folder, listing *seen,
                          size_t *skipped) {
    TEST_PRINTF("list %s", folder);
    return skiff_storage_list(target, folder, collect, seen, skipped);
}

static void make_folder(const char *name, char *out, size_t out_size) {
    TEST_ASSERT_TRUE(temp_dir_path(dir, name, out, out_size));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdir(storage, out));
}

static void test_an_empty_folder_lists_nothing(void) {
    char folder[TEMP_DIR_PATH_MAX];
    listing seen = {0};
    size_t skipped = 99;
    make_folder("empty", folder, sizeof folder);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, list_dir(storage, folder, &seen, &skipped));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, seen.count, "\".\" and \"..\" are never listed");
    TEST_ASSERT_EQUAL_size_t(0, skipped);
}

static void test_a_folder_lists_its_files_and_folders_with_their_details(void) {
    char sub[TEMP_DIR_PATH_MAX];
    char nested[TEMP_DIR_PATH_MAX];
    listing seen = {0};
    size_t skipped = 99;
    write_file(path, CONTENT);
    write_file(other, "");
    make_folder("ULUS10064DATA00", sub, sizeof sub);
    TEST_ASSERT_TRUE(temp_dir_path(sub, "DATA.BIN", nested, sizeof nested));
    write_file(nested, "not listed: one level only");

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, list_dir(storage, dir, &seen, &skipped));
    TEST_ASSERT_EQUAL_INT(3, seen.count);
    TEST_ASSERT_EQUAL_size_t(0, skipped);
    const skiff_storage_entry *file = listed(&seen, "file.bin");
    const skiff_storage_entry *empty = listed(&seen, "other.bin");
    const skiff_storage_entry *folder = listed(&seen, "ULUS10064DATA00");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_NOT_NULL(empty);
    TEST_ASSERT_NOT_NULL(folder);
    TEST_ASSERT_FALSE(file->info.is_dir);
    TEST_ASSERT_EQUAL_UINT64(CONTENT_BYTES, file->info.size);
    TEST_ASSERT_NOT_EQUAL(SKIFF_STORAGE_MTIME_UNKNOWN, file->info.mtime);
    TEST_ASSERT_FALSE(empty->info.is_dir);
    TEST_ASSERT_EQUAL_UINT64(0, empty->info.size);
    TEST_ASSERT_TRUE(folder->info.is_dir);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0, folder->info.size, "a folder reports no size");
    TEST_ASSERT_NULL_MESSAGE(listed(&seen, "DATA.BIN"), "the listing is not recursive");
}

static void test_the_callback_can_stop_a_listing_early(void) {
    static const char *const NAMES[] = {"a.bin", "b.bin", "c.bin"};
    char file[TEMP_DIR_PATH_MAX];
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        TEST_ASSERT_TRUE(temp_dir_path(dir, NAMES[i], file, sizeof file));
        write_file(file, CONTENT);
    }
    listing seen = {0};
    size_t skipped = 0;
    seen.stop_after = 1;
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_OK, list_dir(storage, dir, &seen, &skipped),
                                  "stopping is not an error");
    TEST_ASSERT_EQUAL_INT(1, seen.count);
}

static void test_listing_a_missing_folder_or_a_file_fails(void) {
    char missing[TEMP_DIR_PATH_MAX];
    char below_file[TEMP_DIR_PATH_MAX];
    listing seen = {0};
    size_t skipped = 99;
    TEST_ASSERT_TRUE(temp_dir_path(dir, "missing", missing, sizeof missing));
    TEST_ASSERT_TRUE(temp_dir_path(dir, "file.bin/below", below_file, sizeof below_file));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, list_dir(storage, missing, &seen, &skipped));
    TEST_ASSERT_EQUAL_size_t(0, skipped);
    write_file(path, CONTENT);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_STORAGE_IO, list_dir(storage, path, &seen, &skipped),
                                  "a file is not a folder");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_STORAGE_NOT_FOUND,
                                  list_dir(storage, below_file, &seen, &skipped),
                                  "a file in the middle of the path: nothing there");
    TEST_ASSERT_EQUAL_INT(0, seen.count);
}

static void test_names_too_long_for_an_entry_are_skipped_and_counted(void) {
    char longest[SKIFF_STORAGE_NAME_MAX];
    char too_long[SKIFF_STORAGE_NAME_MAX + 1];
    char much_too_long[SKIFF_STORAGE_NAME_MAX + 64];
    char file[TEMP_DIR_PATH_MAX + SKIFF_STORAGE_NAME_MAX + 64];
    memset(longest, 'l', sizeof longest - 1);
    longest[sizeof longest - 1] = '\0';
    memset(too_long, 't', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = '\0';
    memset(much_too_long, 'm', sizeof much_too_long - 1);
    much_too_long[sizeof much_too_long - 1] = '\0';
    const char *const names[] = {longest, too_long, much_too_long};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(file, sizeof file, "%s/%s", dir, names[i]);
        write_file(file, CONTENT);
    }
    TEST_PRINTF("names of %u, %u and %u bytes; an entry holds %u", (unsigned)strlen(longest),
                (unsigned)strlen(too_long), (unsigned)strlen(much_too_long),
                (unsigned)(SKIFF_STORAGE_NAME_MAX - 1));
    listing seen = {0};
    size_t skipped = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, list_dir(storage, dir, &seen, &skipped));
    TEST_ASSERT_EQUAL_INT(1, seen.count);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(longest, seen.entries[0].name, "the longest name fits whole");
    TEST_ASSERT_EQUAL_size_t_MESSAGE(2, skipped, "longer names are skipped, never cut");
}

static void test_list_checks_its_arguments(void) {
    listing seen = {0};
    size_t skipped = 99;
    skiff_storage no_ops = {NULL};
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_list(storage, dir, collect, &seen, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_list(storage, dir, NULL, &seen, &skipped));
    TEST_ASSERT_EQUAL_size_t(0, skipped);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_list(storage, "", collect, &seen, &skipped));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_list(storage, NULL, collect, &seen, &skipped));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_list(NULL, dir, collect, &seen, &skipped));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_storage_list(&no_ops, dir, collect, &seen, &skipped));
    TEST_ASSERT_EQUAL_INT(0, seen.count);
}

static void test_rmdir_removes_only_an_empty_folder(void) {
    char folder[TEMP_DIR_PATH_MAX];
    char inside[TEMP_DIR_PATH_MAX];
    make_folder("ULUS10064DATA00", folder, sizeof folder);
    TEST_ASSERT_TRUE(temp_dir_path(folder, "PARAM.SFO", inside, sizeof inside));
    write_file(inside, CONTENT);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_STORAGE_IO, skiff_storage_rmdir(storage, folder),
                                  "a folder with a file in it stays");
    TEST_ASSERT_TRUE(is_folder(folder));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(storage, inside));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_rmdir(storage, folder));
    TEST_ASSERT_FALSE(is_folder(folder));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_storage_rmdir(storage, folder));
    write_file(path, CONTENT);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_STORAGE_IO, skiff_storage_rmdir(storage, path),
                                  "a file is not a folder");
    TEST_ASSERT_TRUE(exists(path));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_rmdir(storage, ""));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_rmdir(storage, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_rmdir(NULL, folder));
}

static void test_stat_describes_a_file_a_folder_or_nothing(void) {
    skiff_storage_info info;
    write_file(path, CONTENT);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_stat(storage, path, &info));
    TEST_PRINTF("file: %llu bytes, mtime %lld", (unsigned long long)info.size,
                (long long)info.mtime);
    TEST_ASSERT_FALSE(info.is_dir);
    TEST_ASSERT_EQUAL_UINT64(CONTENT_BYTES, info.size);
    TEST_ASSERT_NOT_EQUAL(SKIFF_STORAGE_MTIME_UNKNOWN, info.mtime);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_stat(storage, dir, &info));
    TEST_ASSERT_TRUE(info.is_dir);
    TEST_ASSERT_EQUAL_UINT64(0, info.size);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_storage_stat(storage, other, &info));
    TEST_ASSERT_FALSE(info.is_dir);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(SKIFF_STORAGE_MTIME_UNKNOWN, info.mtime, "zeroed on error");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_stat(storage, path, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_stat(storage, "", &info));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_stat(storage, NULL, &info));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_stat(NULL, path, &info));
}

/* 2001-09-09 01:46:40 UTC. */
#define OLD_MTIME 1000000000LL

static void test_mtime_follows_the_file_system_and_changes_on_a_rewrite(void) {
    skiff_storage_info info;
    write_file(path, CONTENT);
    const struct timespec times[2] = {{(time_t)OLD_MTIME, 0}, {(time_t)OLD_MTIME, 0}};
    TEST_ASSERT_EQUAL_INT(0, utimensat(AT_FDCWD, path, times, 0));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_stat(storage, path, &info));
    TEST_ASSERT_EQUAL_INT64_MESSAGE(OLD_MTIME, info.mtime, "the host's mtime is UTC seconds");
    listing seen = {0};
    size_t skipped = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, list_dir(storage, dir, &seen, &skipped));
    TEST_ASSERT_EQUAL_INT(1, seen.count);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(OLD_MTIME, seen.entries[0].info.mtime,
                                    "the listing and stat agree");

    write_file(path, "rewritten");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_stat(storage, path, &info));
    TEST_PRINTF("after the rewrite: mtime %lld", (long long)info.mtime);
    TEST_ASSERT_TRUE_MESSAGE(info.mtime > OLD_MTIME, "a rewrite stamps the time now");
}

enum { DATE_TEXT_MAX = 64 };

/* Unity's TEST_PRINTF has no field widths, so dates are formatted here. */
static const char *date_text(const skiff_storage_date *date, char *out, size_t out_size) {
    snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d", date->year, date->month, date->day,
             date->hour, date->minute, date->second);
    return out;
}

static void assert_mtime(int year, int month, int day, int hour, int minute, int second,
                         int64_t expected) {
    const skiff_storage_date date = {year, month, day, hour, minute, second};
    char text[DATE_TEXT_MAX];
    int64_t mtime = 1;
    const skiff_err err = skiff_storage_mtime_from_date(&date, &mtime);
    TEST_PRINTF("%s -> %lld (%s)", date_text(&date, text, sizeof text), (long long)mtime,
                skiff_err_name(err));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, err);
    TEST_ASSERT_EQUAL_INT64(expected, mtime);
}

static void test_a_date_becomes_seconds_since_1970(void) {
    assert_mtime(1970, 1, 1, 0, 0, 0, 0);
    assert_mtime(1969, 12, 31, 23, 59, 59, -1);
    assert_mtime(2000, 1, 1, 0, 0, 0, 946684800LL);
    assert_mtime(2000, 2, 29, 12, 34, 56, 951827696LL);
    assert_mtime(2026, 10, 11, 18, 15, 0, 1791742500LL);
    assert_mtime(2100, 3, 1, 0, 0, 0, 4107542400LL);
    assert_mtime(1, 1, 1, 0, 0, 0, -62135596800LL);
    assert_mtime(9999, 12, 31, 23, 59, 59, 253402300799LL);
}

static void test_a_date_out_of_range_is_refused(void) {
    static const skiff_storage_date REFUSED[] = {
        {0, 0, 0, 0, 0, 0}, /* a zeroed FAT entry */
        {0, 1, 1, 0, 0, 0},     {10000, 1, 1, 0, 0, 0}, {2026, 0, 1, 0, 0, 0},
        {2026, 13, 1, 0, 0, 0}, {2026, 1, 0, 0, 0, 0},  {2026, 4, 31, 0, 0, 0},
        {1900, 2, 29, 0, 0, 0}, {2026, 1, 1, 24, 0, 0}, {2026, 1, 1, 0, 60, 0},
        {2026, 1, 1, 0, 0, 60}, {2026, 1, 1, -1, 0, 0}, {2026, 1, 1, 0, -1, 0},
        {2026, 1, 1, 0, 0, -1},
    };
    for (size_t i = 0; i < sizeof REFUSED / sizeof REFUSED[0]; i++) {
        const skiff_storage_date *date = &REFUSED[i];
        char text[DATE_TEXT_MAX];
        int64_t mtime = 1;
        TEST_PRINTF("%s refused", date_text(date, text, sizeof text));
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_mtime_from_date(date, &mtime));
        TEST_ASSERT_EQUAL_INT64(SKIFF_STORAGE_MTIME_UNKNOWN, mtime);
    }
    int64_t mtime = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_mtime_from_date(NULL, &mtime));
    TEST_ASSERT_EQUAL_INT64(SKIFF_STORAGE_MTIME_UNKNOWN, mtime);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_storage_mtime_from_date(&REFUSED[0], NULL));
}

/* A SAVEDATA folder holding a.bin and b.bin, and a fake over the storage that fails only it. */
static void fake_over_savedata(fake_storage *fake, char *folder, size_t folder_size) {
    char file[TEMP_DIR_PATH_MAX];
    make_folder("SAVEDATA", folder, folder_size);
    TEST_ASSERT_TRUE(temp_dir_path(folder, "a.bin", file, sizeof file));
    write_file(file, CONTENT);
    TEST_ASSERT_TRUE(temp_dir_path(folder, "b.bin", file, sizeof file));
    write_file(file, CONTENT);
    fake_storage_init(fake, storage);
    fake->fail_suffix = "SAVEDATA";
}

static void test_fake_fails_a_listing_before_any_entry(void) {
    fake_storage fake;
    char folder[TEMP_DIR_PATH_MAX];
    listing seen = {0};
    size_t skipped = 0;
    fake_over_savedata(&fake, folder, sizeof folder);
    fake.list_error = SKIFF_ERR_STORAGE_NO_MEDIA;
    TEST_PRINTF("the folder cannot be opened");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_MEDIA,
                          list_dir(&fake.base, folder, &seen, &skipped));
    TEST_ASSERT_EQUAL_INT(0, seen.count);
    TEST_PRINTF("other folders list normally");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, list_dir(&fake.base, dir, &seen, &skipped));
    TEST_ASSERT_EQUAL_INT(1, seen.count);
    skiff_storage_destroy(&fake.base);
}

static void test_fake_fails_a_listing_part_way_through(void) {
    fake_storage fake;
    char folder[TEMP_DIR_PATH_MAX];
    listing seen = {0};
    size_t skipped = 0;
    fake_over_savedata(&fake, folder, sizeof folder);
    fake.list_error = SKIFF_ERR_STORAGE_IO;
    fake.list_error_after = 1;
    TEST_PRINTF("the handle is lost after the first entry, as after a suspend");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, list_dir(&fake.base, folder, &seen, &skipped));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen.count, "the caller saw one entry, then the error");
    skiff_storage_destroy(&fake.base);
}

static void test_fake_lets_a_listing_shorter_than_the_failure_complete(void) {
    fake_storage fake;
    char folder[TEMP_DIR_PATH_MAX];
    listing seen = {0};
    size_t skipped = 0;
    fake_over_savedata(&fake, folder, sizeof folder);
    fake.list_error = SKIFF_ERR_STORAGE_IO;
    fake.list_error_after = LISTED_MAX;
    TEST_PRINTF("more entries allowed than the folder holds: the listing completes");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, list_dir(&fake.base, folder, &seen, &skipped));
    TEST_ASSERT_EQUAL_INT(2, seen.count);
    skiff_storage_destroy(&fake.base);
}

static void test_fake_fails_stats_of_matching_paths(void) {
    fake_storage fake;
    char folder[TEMP_DIR_PATH_MAX];
    char file[TEMP_DIR_PATH_MAX];
    skiff_storage_info info;
    fake_over_savedata(&fake, folder, sizeof folder);
    fake.stat_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_TRUE(temp_dir_path(folder, "a.bin", file, sizeof file));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_storage_stat(&fake.base, folder, &info));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_stat(&fake.base, file, &info));
    TEST_ASSERT_FALSE(info.is_dir);
    skiff_storage_destroy(&fake.base);
}

static void test_fake_fails_reads_of_matching_files_only(void) {
    fake_storage fake;
    char text[CONTENT_BYTES];
    size_t got = 1;
    skiff_file *file = NULL;
    fake_storage_init(&fake, storage);
    fake.fail_suffix = "file.bin";
    fake.read_error = SKIFF_ERR_STORAGE_IO;
    write_file(path, CONTENT);
    write_file(other, CONTENT);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(&fake.base, path, SKIFF_FILE_READ, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_file_read(file, text, sizeof text, &got));
    TEST_ASSERT_EQUAL_size_t(0, got);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(&fake.base, other, SKIFF_FILE_READ, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_read(file, text, sizeof text, &got));
    TEST_ASSERT_EQUAL_size_t(CONTENT_BYTES, got);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    skiff_storage_destroy(&fake.base);
}

static void test_fake_fails_folder_removals_and_leaves_the_folder(void) {
    fake_storage fake;
    char folder[TEMP_DIR_PATH_MAX];
    char file[TEMP_DIR_PATH_MAX];
    fake_over_savedata(&fake, folder, sizeof folder);
    fake.rmdir_error = SKIFF_ERR_STORAGE_NO_MEDIA;
    TEST_ASSERT_TRUE(temp_dir_path(folder, "a.bin", file, sizeof file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(&fake.base, file));
    TEST_ASSERT_TRUE(temp_dir_path(folder, "b.bin", file, sizeof file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(&fake.base, file));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_MEDIA, skiff_storage_rmdir(&fake.base, folder));
    TEST_ASSERT_TRUE_MESSAGE(is_folder(folder), "a failed removal leaves the folder");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_storage_rmdir(&fake.base, dir));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake.rmdirs,
                                  "a full folder is refused before the device is asked");
    skiff_storage_destroy(&fake.base);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_rmdir(storage, folder));
    TEST_ASSERT_FALSE(is_folder(folder));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_replace_write_read_and_size);
    RUN_TEST(test_write_at_overwrites_from_the_offset_and_keeps_the_rest);
    RUN_TEST(test_write_at_past_the_end_or_on_a_missing_file_is_refused);
    RUN_TEST(test_read_starts_at_an_offset_within_the_file);
    RUN_TEST(test_missing_files_are_not_found);
    RUN_TEST(test_rename_refuses_to_replace_and_remove_deletes);
    RUN_TEST(test_reading_past_the_end_gets_nothing);
    RUN_TEST(test_arguments_are_checked_before_the_implementation);
    RUN_TEST(test_fake_fills_up_mid_write_for_matching_paths_only);
    RUN_TEST(test_fake_loses_open_handles_and_fails_syncs_and_renames);
    RUN_TEST(test_mkdir_creates_a_folder_once_and_keeps_an_existing_one);
    RUN_TEST(test_mkdirs_creates_every_missing_folder);
    RUN_TEST(test_mkdirs_walks_a_device_path_from_its_first_folder);
    RUN_TEST(test_free_space_of_the_host_device);
    RUN_TEST(test_room_needs_the_file_plus_a_margin);
    RUN_TEST(test_a_whole_file_round_trips_and_a_missing_one_reads_empty);
    RUN_TEST(test_a_whole_file_too_big_for_the_buffer_is_refused);
    RUN_TEST(test_whole_files_are_binary_and_fill_an_exact_buffer);
    RUN_TEST(test_a_complete_pending_file_is_put_in_place);
    RUN_TEST(test_whole_files_refuse_bad_arguments);
    RUN_TEST(test_an_empty_folder_lists_nothing);
    RUN_TEST(test_a_folder_lists_its_files_and_folders_with_their_details);
    RUN_TEST(test_the_callback_can_stop_a_listing_early);
    RUN_TEST(test_listing_a_missing_folder_or_a_file_fails);
    RUN_TEST(test_names_too_long_for_an_entry_are_skipped_and_counted);
    RUN_TEST(test_list_checks_its_arguments);
    RUN_TEST(test_rmdir_removes_only_an_empty_folder);
    RUN_TEST(test_stat_describes_a_file_a_folder_or_nothing);
    RUN_TEST(test_mtime_follows_the_file_system_and_changes_on_a_rewrite);
    RUN_TEST(test_a_date_becomes_seconds_since_1970);
    RUN_TEST(test_a_date_out_of_range_is_refused);
    RUN_TEST(test_fake_fails_a_listing_before_any_entry);
    RUN_TEST(test_fake_fails_a_listing_part_way_through);
    RUN_TEST(test_fake_lets_a_listing_shorter_than_the_failure_complete);
    RUN_TEST(test_fake_fails_stats_of_matching_paths);
    RUN_TEST(test_fake_fails_reads_of_matching_files_only);
    RUN_TEST(test_fake_fails_folder_removals_and_leaves_the_folder);
    return UNITY_END();
}
