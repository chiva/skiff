/*
 * The storage seam (skiff/storage.h): the argument checks every implementation gets for free, and
 * the POSIX implementation host tests run on, including the rules it keeps from the PSP (no rename
 * over an existing file, writes from an offset that leave the rest of the file alone). Also the
 * fake that injects Memory Stick failures (tests/support/fake_storage.h).
 */
#include <string.h>

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
                                  "only WRITE_AT takes an offset");
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_replace_write_read_and_size);
    RUN_TEST(test_write_at_overwrites_from_the_offset_and_keeps_the_rest);
    RUN_TEST(test_write_at_past_the_end_or_on_a_missing_file_is_refused);
    RUN_TEST(test_missing_files_are_not_found);
    RUN_TEST(test_rename_refuses_to_replace_and_remove_deletes);
    RUN_TEST(test_reading_past_the_end_gets_nothing);
    RUN_TEST(test_arguments_are_checked_before_the_implementation);
    RUN_TEST(test_fake_fills_up_mid_write_for_matching_paths_only);
    RUN_TEST(test_fake_loses_open_handles_and_fails_syncs_and_renames);
    return UNITY_END();
}
