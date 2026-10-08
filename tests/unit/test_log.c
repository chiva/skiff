/*
 * The log file (skiff/log.h). It lives on the Memory Stick, where every write during a download
 * also pauses Wi-Fi, so lines must be batched; it is attached to public bug reports, so no secret
 * may reach it, not even part of one in a line that was cut; and it must keep working after a
 * suspend invalidates open files, without ever failing the code that logs.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/log.h"

#include "fake_storage.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define TOKEN "rmm_0123456789abcdef"
#define CF_SECRET "cf-secret-value-42"
#define FILE_MAX (64 * 1024)
#define SMALL_CAP 1024U
#define NOW_MS 1791290096789LL
#define NOW_TEXT "2026-10-06 12:34:56.789Z"

static char dir[TEMP_DIR_PATH_MAX];
static char path[TEMP_DIR_PATH_MAX];
static char rotated[TEMP_DIR_PATH_MAX];
static skiff_storage *posix;
static fake_storage fake;
static skiff_log_config config;
static skiff_log *logger;
static char contents[FILE_MAX];

typedef struct lock_counter {
    int depth;
    int deepest;
    int taken;
} lock_counter;

static lock_counter locks;

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    TEST_ASSERT_TRUE(temp_dir_path(dir, "skiff.log", path, sizeof path));
    TEST_ASSERT_TRUE(
        temp_dir_path(dir, "skiff.log" SKIFF_LOG_ROTATED_SUFFIX, rotated, sizeof rotated));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&posix));
    fake_storage_init(&fake, posix);
    memset(&config, 0, sizeof config);
    config.storage = &fake.base;
    config.path = path;
    config.level = SKIFF_LOG_INFO;
    memset(&locks, 0, sizeof locks);
    logger = NULL;
}

void tearDown(void) {
    skiff_log_destroy(logger);
    skiff_storage_destroy(posix);
    temp_dir_remove(dir);
}

static void create(void) { TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_create(&config, &logger)); }

/* The whole file as text, or "" when it does not exist. */
static const char *read_file(const char *target) {
    skiff_file *file = NULL;
    size_t used = 0;
    size_t got = 0;
    contents[0] = '\0';
    if (skiff_storage_open(posix, target, SKIFF_FILE_READ, 0, &file) != SKIFF_OK) {
        return contents;
    }
    do {
        TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                              skiff_file_read(file, contents + used, FILE_MAX - 1 - used, &got));
        used += got;
    } while (got > 0 && used < FILE_MAX - 1);
    contents[used] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    TEST_PRINTF("%s (%u bytes):\n%s", target, (unsigned)used, contents);
    return contents;
}

static uint64_t file_size(const char *target) {
    uint64_t size = 0;
    return skiff_storage_size(posix, target, &size) == SKIFF_OK ? size : 0;
}

static int count_lines(const char *text) {
    int lines = 0;
    for (; *text != '\0'; text++) {
        lines += *text == '\n';
    }
    return lines;
}

/* A clock that moves on by a millisecond each time it is read. */
static int ticking_clock(void *ctx, int64_t *unix_ms) {
    int64_t *now = ctx;
    *unix_ms = (*now)++;
    return 1;
}

static int broken_clock(void *ctx, int64_t *unix_ms) {
    (void)ctx;
    *unix_ms = 0;
    return 0;
}

static void counting_lock(void *ctx) {
    lock_counter *counter = ctx;
    counter->depth++;
    counter->taken++;
    if (counter->depth > counter->deepest) {
        counter->deepest = counter->depth;
    }
}

static void counting_unlock(void *ctx) { ((lock_counter *)ctx)->depth--; }

/* ---- Timestamps ---- */

static void assert_timestamp(int64_t unix_ms, const char *expected) {
    char out[SKIFF_LOG_TIMESTAMP_MAX];
    const skiff_err err = skiff_log_format_timestamp(unix_ms, out, sizeof out);
    TEST_PRINTF("%lld ms -> %s", (long long)unix_ms, out);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, err);
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

static void test_timestamps_follow_the_gregorian_calendar(void) {
    assert_timestamp(0, "1970-01-01 00:00:00.000Z");
    assert_timestamp(-1, "1969-12-31 23:59:59.999Z");
    assert_timestamp(NOW_MS, NOW_TEXT);
    assert_timestamp(1709251199999LL, "2024-02-29 23:59:59.999Z");
    assert_timestamp(951782400000LL, "2000-02-29 00:00:00.000Z");
    assert_timestamp(-2203891200000LL, "1900-03-01 00:00:00.000Z");
    assert_timestamp(4107542400000LL, "2100-03-01 00:00:00.000Z");
    assert_timestamp(2147483648123LL, "2038-01-19 03:14:08.123Z");
    assert_timestamp(-62167219200000LL, "0000-01-01 00:00:00.000Z");
    assert_timestamp(253402300799999LL, "9999-12-31 23:59:59.999Z");
}

static void test_timestamps_outside_four_digit_years_or_buffers_are_refused(void) {
    char out[SKIFF_LOG_TIMESTAMP_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_log_format_timestamp(253402300800000LL, out, sizeof out));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_log_format_timestamp(-62167219200001LL, out, sizeof out));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_log_format_timestamp(0, out, SKIFF_LOG_TIMESTAMP_MAX - 1));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_log_format_timestamp(0, NULL, 0));
}

/* ---- Levels and creation ---- */

static void test_level_names_round_trip_and_ignore_case(void) {
    skiff_log_level level = SKIFF_LOG_ERROR;
    for (int i = SKIFF_LOG_ERROR; i <= SKIFF_LOG_DEBUG; i++) {
        const char *name = skiff_log_level_name((skiff_log_level)i);
        TEST_PRINTF("level %d = %s", i, name);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_level_from_name(name, &level));
        TEST_ASSERT_EQUAL_INT(i, level);
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_level_from_name("DeBuG", &level));
    TEST_ASSERT_EQUAL_INT(SKIFF_LOG_DEBUG, level);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_INVALID_VALUE,
                          skiff_log_level_from_name("verbose", &level));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_INVALID_VALUE, skiff_log_level_from_name("", &level));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_log_level_from_name(NULL, &level));
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    TEST_ASSERT_EQUAL_STRING("unknown",
                             skiff_log_level_name((skiff_log_level)(SKIFF_LOG_DEBUG + 1)));
}

static void expect_create_refused(skiff_err expected, const char *why) {
    TEST_PRINTF("refused: %s", why);
    logger = (skiff_log *)&fake;
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, skiff_log_create(&config, &logger), why);
    TEST_ASSERT_NULL(logger);
}

static void test_create_refuses_unusable_settings(void) {
    char long_path[SKIFF_LOG_PATH_MAX];
    memset(long_path, 'a', sizeof long_path - 2);
    long_path[sizeof long_path - 2] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_log_create(&config, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_log_create(NULL, &logger));
    config.path = "";
    expect_create_refused(SKIFF_ERR_INVALID_ARG, "empty path");
    config.path = long_path;
    expect_create_refused(SKIFF_ERR_BUFFER_TOO_SMALL, "no room for the .1 suffix");
    config.path = path;
    config.buffer_bytes = SKIFF_LOG_LINE_MAX - 1;
    expect_create_refused(SKIFF_ERR_INVALID_ARG, "buffer smaller than a line");
    config.buffer_bytes = SMALL_CAP + 1;
    config.cap_bytes = SMALL_CAP;
    expect_create_refused(SKIFF_ERR_INVALID_ARG, "buffer larger than the cap");
    config.buffer_bytes = SKIFF_LOG_BUFFER_MAX + 1;
    config.cap_bytes = UINT64_MAX;
    expect_create_refused(SKIFF_ERR_INVALID_ARG, "buffer larger than the maximum");
    config.buffer_bytes = SIZE_MAX;
    expect_create_refused(SKIFF_ERR_INVALID_ARG, "a buffer size that would wrap the allocation");
    config.buffer_bytes = 0;
    config.cap_bytes = 0;
    config.lock = counting_lock;
    expect_create_refused(SKIFF_ERR_INVALID_ARG, "lock without unlock");
    config.lock = NULL;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    config.level = (skiff_log_level)(SKIFF_LOG_DEBUG + 1);
    expect_create_refused(SKIFF_ERR_INVALID_ARG, "unknown level");
    config.level = SKIFF_LOG_INFO;
    config.storage = NULL;
    expect_create_refused(SKIFF_ERR_INVALID_ARG, "no storage");
}

static void test_lines_below_the_level_are_left_out(void) {
    create();
    skiff_log_write(logger, SKIFF_LOG_DEBUG, "jobs", "chunk %d", 1);
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "download %s", "started");
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    skiff_log_write(logger, (skiff_log_level)(SKIFF_LOG_DEBUG + 1), "jobs", "never");
    skiff_log_flush(logger);
    TEST_ASSERT_EQUAL_STRING("I jobs: download started\n", read_file(path));
}

static void test_null_logger_and_arguments_are_ignored(void) {
    skiff_log_write(NULL, SKIFF_LOG_ERROR, "jobs", "nothing");
    skiff_log_header(NULL, SKIFF_LOG_ERROR, "net", "ETag", "\"x\"");
    skiff_log_destroy(NULL);
    skiff_log_flush(NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_log_add_secret(NULL, TOKEN));
    create();
    skiff_log_write(logger, SKIFF_LOG_ERROR, NULL, "%s", "no tag");
    skiff_log_header(logger, SKIFF_LOG_ERROR, "net", NULL, "x");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_log_add_secret(logger, NULL));
    TEST_ASSERT_EQUAL_STRING("E -: no tag\n", read_file(path));
}

/* ---- Timestamps in lines ---- */

static void test_lines_start_with_the_clock_when_it_can_be_read(void) {
    int64_t now = NOW_MS;
    config.clock = ticking_clock;
    config.clock_ctx = &now;
    create();
    skiff_log_write(logger, SKIFF_LOG_WARN, "app", "Skiff %s", "0.2.0");
    TEST_ASSERT_EQUAL_STRING(NOW_TEXT " W app: Skiff 0.2.0\n", read_file(path));
}

static void test_lines_go_without_a_timestamp_when_the_clock_fails(void) {
    config.clock = broken_clock;
    create();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "app", "no clock");
    TEST_ASSERT_EQUAL_STRING("E app: no clock\n", read_file(path));
}

/* ---- Batching ---- */

static void test_info_lines_wait_in_memory_until_a_flush(void) {
    create();
    for (int i = 0; i < 10; i++) {
        skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "line %d", i);
    }
    TEST_PRINTF("after 10 info lines: %d opens, %d writes", fake.opens, fake.writes);
    TEST_ASSERT_EQUAL_INT(0, fake.opens);
    TEST_ASSERT_EQUAL_INT(0, fake.writes);
    skiff_log_flush(logger);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake.writes, "ten lines, one Memory Stick write");
    TEST_ASSERT_EQUAL_INT(10, count_lines(read_file(path)));
    skiff_log_flush(logger);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake.writes, "nothing buffered, nothing written");
}

static void test_warnings_and_errors_are_written_at_once(void) {
    create();
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "before");
    skiff_log_write(logger, SKIFF_LOG_WARN, "jobs", "retrying");
    TEST_ASSERT_EQUAL_INT(1, fake.writes);
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "%s (%d)", "SKIFF_ERR_NET_TIMEOUT", 103);
    TEST_ASSERT_EQUAL_INT(2, fake.writes);
    TEST_ASSERT_EQUAL_STRING(
        "I jobs: before\nW jobs: retrying\nE jobs: SKIFF_ERR_NET_TIMEOUT (103)\n", read_file(path));
}

static void test_a_full_buffer_is_written_before_the_next_line(void) {
    config.buffer_bytes = SKIFF_LOG_LINE_MAX;
    create();
    char filler[SKIFF_LOG_LINE_MAX];
    memset(filler, 'x', 100);
    filler[100] = '\0';
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "%s", filler);
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "%s", filler);
    TEST_ASSERT_EQUAL_INT(0, fake.writes);
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "%s", filler);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake.writes, "the third line did not fit");
    skiff_log_destroy(logger);
    logger = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, fake.writes, "destroy writes the rest");
    TEST_ASSERT_EQUAL_INT(3, count_lines(read_file(path)));
}

static void test_batches_are_appended_to_an_existing_log(void) {
    create();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "app", "first run");
    skiff_log_destroy(logger);
    create();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "app", "second run");
    TEST_ASSERT_EQUAL_STRING("E app: first run\nE app: second run\n", read_file(path));
}

/* ---- Rotation ---- */

static void write_lines_of(int count, size_t length) {
    char text[SKIFF_LOG_LINE_MAX];
    const size_t prefix = strlen("I jobs: ");
    memset(text, 'r', length - prefix - 1);
    text[length - prefix - 1] = '\0';
    for (int i = 0; i < count; i++) {
        skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "%s", text);
        skiff_log_flush(logger);
    }
}

static void test_a_log_at_the_cap_stays_and_one_past_it_rotates(void) {
    config.cap_bytes = SMALL_CAP;
    config.buffer_bytes = SKIFF_LOG_LINE_MAX;
    create();
    write_lines_of(4, SMALL_CAP / 4);
    TEST_PRINTF("4 lines of %u bytes: log %llu bytes", SMALL_CAP / 4,
                (unsigned long long)file_size(path));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(SMALL_CAP, file_size(path), "exactly at the cap: no rotation");
    TEST_ASSERT_EQUAL_UINT64(0, file_size(rotated));
    write_lines_of(1, 10);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(SMALL_CAP, file_size(rotated), "the full log moved aside");
    TEST_ASSERT_EQUAL_UINT64(10, file_size(path));
}

static void test_a_second_rotation_replaces_the_older_file(void) {
    config.cap_bytes = SMALL_CAP;
    config.buffer_bytes = SKIFF_LOG_LINE_MAX;
    create();
    write_lines_of(4, SMALL_CAP / 4);
    write_lines_of(4, SMALL_CAP / 4);
    write_lines_of(1, 20);
    TEST_ASSERT_EQUAL_UINT64(SMALL_CAP, file_size(rotated));
    TEST_ASSERT_EQUAL_UINT64(20, file_size(path));
    TEST_ASSERT_EQUAL_INT(4, count_lines(read_file(rotated)));
}

static void test_a_failed_rotation_starts_the_log_over_rather_than_pass_the_cap(void) {
    config.cap_bytes = SMALL_CAP;
    config.buffer_bytes = SKIFF_LOG_LINE_MAX;
    create();
    write_lines_of(4, SMALL_CAP / 4);
    fake.rename_error = SKIFF_ERR_STORAGE_IO;
    write_lines_of(1, 30);
    TEST_ASSERT_EQUAL_UINT64(0, file_size(rotated));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(30, file_size(path), "replaced, never past the cap");
}

/* ---- Failures ---- */

#define REPORT_ONE "W log: 1 earlier lines could not be written\n"
/* What the Memory Stick takes of a batch before it fills up, in the tests that cut one short. */
#define FRAGMENT_BYTES 5U

/* An info line of exactly length bytes, newline included: "I jobs: " and fill. Copied into line
 * (SKIFF_LOG_LINE_MAX + 1 bytes) unless it is NULL. */
static void info_line_of(size_t length, char fill, char *line) {
    char text[SKIFF_LOG_LINE_MAX];
    const char *prefix = "I jobs: ";
    const size_t prefix_length = strlen(prefix);
    const size_t fill_length = length - prefix_length - 1;
    memset(text, fill, fill_length);
    text[fill_length] = '\0';
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "%s", text);
    if (line != NULL) {
        memcpy(line, prefix, prefix_length);
        memcpy(line + prefix_length, text, fill_length);
        line[length - 1] = '\n';
        line[length] = '\0';
    }
}

/* Every later write fails, after storing at most bytes more. */
static void refuse_writes_after(uint64_t bytes) {
    fake.write_budget = fake.bytes_written + bytes;
    fake.write_error = SKIFF_ERR_STORAGE_IO;
}

static void accept_writes(void) { fake.write_error = SKIFF_OK; }

static int count_of(const char *text, const char *needle) {
    int count = 0;
    for (const char *at = strstr(text, needle); at != NULL; at = strstr(at + 1, needle)) {
        count++;
    }
    return count;
}

static void test_a_handle_lost_to_a_suspend_is_reopened_without_repeating_lines(void) {
    create();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "app", "before suspend");
    fake.stale_after_writes = fake.writes + 1;
    skiff_log_write(logger, SKIFF_LOG_ERROR, "app", "after wake");
    TEST_PRINTF("handles lost %d, opens %d", fake.handles_lost, fake.opens);
    TEST_ASSERT_EQUAL_INT(1, fake.handles_lost);
    TEST_ASSERT_EQUAL_STRING("E app: before suspend\nE app: after wake\n", read_file(path));
}

static void test_a_refused_batch_stays_buffered_and_goes_out_with_the_next(void) {
    create();
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "one");
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "two");
    refuse_writes_after(0);
    skiff_log_flush(logger);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, fake.writes, "one try and one retry");
    TEST_ASSERT_EQUAL_STRING("", read_file(path));
    accept_writes();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "three");
    TEST_ASSERT_EQUAL_STRING("I jobs: one\nI jobs: two\nE jobs: three\n", read_file(path));
}

/* J1 on a PSP-1000 (2026-10-07/08): right after waking, the warning saying why the download
 * stopped was refused twice and lost, in three runs out of three. */
static void test_a_warning_logged_right_after_waking_waits_until_the_memory_stick_takes_it(void) {
    create();
    skiff_log_write(logger, SKIFF_LOG_WARN, "jobs", "attempt 1: SKIFF_ERR_NET_UNAVAILABLE (100)");
    refuse_writes_after(0);
    skiff_log_write(logger, SKIFF_LOG_WARN, "jobs",
                    "attempt 2: SKIFF_ERR_NET_CONNECTION_LOST (111)");
    const int after_warning = fake.writes;
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "rejoin profile 1: SKIFF_OK (0)");
    TEST_PRINTF("writes: %d after the refused warning, %d after an info line", after_warning,
                fake.writes);
    TEST_ASSERT_EQUAL_INT_MESSAGE(after_warning, fake.writes, "an info line adds no try");
    skiff_log_flush(logger);
    TEST_ASSERT_EQUAL_INT_MESSAGE(after_warning + 2, fake.writes, "a flush tries again");
    accept_writes();
    skiff_log_flush(logger);
    TEST_ASSERT_EQUAL_STRING("W jobs: attempt 1: SKIFF_ERR_NET_UNAVAILABLE (100)\n"
                             "W jobs: attempt 2: SKIFF_ERR_NET_CONNECTION_LOST (111)\n"
                             "I jobs: rejoin profile 1: SKIFF_OK (0)\n",
                             read_file(path));
}

static void test_lines_wait_in_memory_while_the_memory_stick_refuses(void) {
    create();
    refuse_writes_after(0);
    for (int i = 0; i < 3; i++) {
        skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "error %d", i);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(6, fake.writes, "two tries per error");
    accept_writes();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "back");
    TEST_ASSERT_EQUAL_STRING("E jobs: error 0\nE jobs: error 1\nE jobs: error 2\nE jobs: back\n",
                             read_file(path));
}

static void test_part_of_a_refused_batch_in_the_file_is_written_over(void) {
    create();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "kept");
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "cut short");
    TEST_PRINTF("the Memory Stick takes 5 bytes of the batch, then fills up");
    refuse_writes_after(5);
    skiff_log_flush(logger);
    TEST_ASSERT_EQUAL_STRING("E jobs: kept\nI job", read_file(path));
    accept_writes();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "next");
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "and after");
    TEST_ASSERT_EQUAL_STRING("E jobs: kept\nI jobs: cut short\nE jobs: next\nE jobs: and after\n",
                             read_file(path));
}

static void test_a_batch_cut_short_after_a_failed_plan_is_written_over_too(void) {
    create();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "kept");
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "cut short");
    TEST_PRINTF("the first look at the log's size fails, the retry writes 5 bytes and fills up");
    fake.size_failures = 1;
    refuse_writes_after(5);
    skiff_log_flush(logger);
    TEST_ASSERT_EQUAL_INT(0, fake.size_failures);
    accept_writes();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "next");
    TEST_ASSERT_EQUAL_STRING("E jobs: kept\nI jobs: cut short\nE jobs: next\n", read_file(path));
}

static void test_a_batch_that_cannot_be_synced_is_written_again_in_place(void) {
    create();
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "one");
    fake.sync_error = SKIFF_ERR_STORAGE_IO;
    skiff_log_flush(logger);
    TEST_PRINTF("syncs %d", fake.syncs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, fake.syncs, "one sync per try");
    fake.sync_error = SKIFF_OK;
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "two");
    TEST_ASSERT_EQUAL_STRING("I jobs: one\nE jobs: two\n", read_file(path));
}

/* 3 lines of 256 bytes and one of 228: 996 bytes, 28 short of the cap. */
#define NEAR_CAP_BYTES 996U
/* "I jobs: cut short\n" and "E jobs: x\n". */
#define CUT_SHORT_AND_X_BYTES 28U

static void fill_near_the_cap(void) {
    config.cap_bytes = SMALL_CAP;
    config.buffer_bytes = SKIFF_LOG_LINE_MAX;
    create();
    write_lines_of(3, 256);
    write_lines_of(1, NEAR_CAP_BYTES - 3 * 256);
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "cut short");
    refuse_writes_after(FRAGMENT_BYTES);
    skiff_log_flush(logger);
    accept_writes();
    TEST_ASSERT_EQUAL_UINT64(NEAR_CAP_BYTES + FRAGMENT_BYTES, file_size(path));
}

static void test_a_batch_tried_again_is_checked_against_the_cap_where_it_goes(void) {
    fill_near_the_cap();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "x");
    TEST_PRINTF("log %llu bytes, rotated %llu bytes", (unsigned long long)file_size(path),
                (unsigned long long)file_size(rotated));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0, file_size(rotated),
                                     "written over its own part: exactly at the cap, no rotation");
    TEST_ASSERT_EQUAL_UINT64(NEAR_CAP_BYTES + CUT_SHORT_AND_X_BYTES, file_size(path));
}

static void test_a_batch_tried_again_past_the_cap_rotates_and_leaves_its_part_behind(void) {
    fill_near_the_cap();
    char line[SKIFF_LOG_LINE_MAX + 1];
    info_line_of(100, 'n', line);
    skiff_log_flush(logger);
    TEST_ASSERT_EQUAL_UINT64(NEAR_CAP_BYTES + FRAGMENT_BYTES, file_size(rotated));
    char expected[2 * SKIFF_LOG_LINE_MAX];
    snprintf(expected, sizeof expected, "I jobs: cut short\n%s", line);
    TEST_ASSERT_EQUAL_STRING(expected, read_file(path));
}

static void test_a_full_buffer_gives_up_its_oldest_lines_while_the_memory_stick_refuses(void) {
    config.buffer_bytes = SKIFF_LOG_LINE_MAX;
    create();
    char filler[SKIFF_LOG_LINE_MAX];
    memset(filler, 'x', 100);
    filler[100] = '\0';
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "%s", filler);
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "%s", filler);
    refuse_writes_after(0);
    /* Does not fit next to the first two: they are tried, refused, and give way to it. */
    skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "last %s", filler);
    accept_writes();
    skiff_log_destroy(logger);
    logger = NULL;
    char expected[2 * SKIFF_LOG_LINE_MAX];
    snprintf(expected, sizeof expected,
             "W log: 2 earlier lines could not be written\nI jobs: last %s\n", filler);
    TEST_ASSERT_EQUAL_STRING(expected, read_file(path));
}

enum { MANY_LINES = 40, MANY_LINE_BYTES = 60 };

static void test_a_refusing_memory_stick_gets_no_try_per_line_and_the_count_is_exact(void) {
    config.buffer_bytes = SKIFF_LOG_LINE_MAX;
    create();
    refuse_writes_after(0);
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "first");
    const int before = fake.writes;
    char newest[SKIFF_LOG_LINE_MAX + 1];
    for (int i = 0; i < MANY_LINES; i++) {
        info_line_of(MANY_LINE_BYTES, (char)('a' + i % 26), newest);
    }
    const int tries = fake.writes - before;
    TEST_PRINTF("%d info lines of %d bytes into a %d-byte buffer: %d write tries", MANY_LINES,
                MANY_LINE_BYTES, SKIFF_LOG_LINE_MAX, tries);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, tries, "a buffer's worth of lost lines tries again");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(MANY_LINES, tries, "fewer tries than lines");
    accept_writes();
    skiff_log_flush(logger);
    const char *text = read_file(path);
    const char *report_prefix = "W log: ";
    TEST_ASSERT_EQUAL_INT(0, strncmp(text, report_prefix, strlen(report_prefix)));
    char *after_count = NULL;
    const unsigned long lost = strtoul(text + strlen(report_prefix), &after_count, 10);
    TEST_ASSERT_EQUAL_INT(0, strncmp(after_count, " earlier lines could not be written\n",
                                     strlen(" earlier lines could not be written\n")));
    const int kept_lines = count_lines(text) - 1;
    TEST_PRINTF("%lu lost, %d kept", lost, kept_lines);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1 + MANY_LINES, (int)lost + kept_lines,
                                  "every line written or counted, once");
    TEST_ASSERT_EQUAL_INT(1, count_of(text, "earlier lines"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(newest, text + strlen(text) - strlen(newest),
                                     "the newest line is kept");
}

static void test_a_report_cut_short_is_written_over_not_repeated(void) {
    config.buffer_bytes = SKIFF_LOG_LINE_MAX;
    create();
    char second[SKIFF_LOG_LINE_MAX + 1];
    char third[SKIFF_LOG_LINE_MAX + 1];
    info_line_of(100, 'a', NULL);
    info_line_of(100, 'b', second);
    refuse_writes_after(0);
    skiff_log_flush(logger);
    /* No room next to the first two: the oldest goes, and the report takes its place. */
    info_line_of(100, 'c', third);
    TEST_PRINTF("the next try gets 10 bytes of the report onto the Memory Stick");
    refuse_writes_after(10);
    skiff_log_flush(logger);
    accept_writes();
    skiff_log_flush(logger);
    char expected[4 * SKIFF_LOG_LINE_MAX];
    snprintf(expected, sizeof expected, REPORT_ONE "%s%s", second, third);
    TEST_ASSERT_EQUAL_STRING(expected, read_file(path));
}

static void test_a_part_whose_first_lines_were_given_up_gets_a_line_of_its_own(void) {
    config.buffer_bytes = SKIFF_LOG_LINE_MAX;
    create();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "kept");
    info_line_of(200, 'a', NULL);
    refuse_writes_after(FRAGMENT_BYTES);
    skiff_log_flush(logger);
    /* No room next to the first: it goes, so its 5 bytes in the file can no longer be written
     * over. */
    char second[SKIFF_LOG_LINE_MAX + 1];
    info_line_of(151, 'b', second);
    accept_writes();
    skiff_log_flush(logger);
    char expected[4 * SKIFF_LOG_LINE_MAX];
    snprintf(expected, sizeof expected, "E jobs: kept\nI job\n" REPORT_ONE "%s", second);
    TEST_ASSERT_EQUAL_STRING(expected, read_file(path));
}

/* 3 lines of 256 bytes and one of 56. */
#define BEFORE_FRAGMENT_BYTES 824U

static void test_the_line_break_after_a_part_counts_against_the_cap(void) {
    config.cap_bytes = SMALL_CAP;
    config.buffer_bytes = SKIFF_LOG_LINE_MAX;
    create();
    write_lines_of(3, 256);
    write_lines_of(1, BEFORE_FRAGMENT_BYTES - 3 * 256);
    /* Exactly to the cap: tried at 824, 5 bytes land. */
    info_line_of(SMALL_CAP - BEFORE_FRAGMENT_BYTES, 'a', NULL);
    refuse_writes_after(FRAGMENT_BYTES);
    skiff_log_flush(logger);
    /* The first line gives way; the report and this one would end exactly at the cap after the
     * part, but not with the line break in front. */
    const size_t report_bytes = strlen(REPORT_ONE);
    const size_t second_bytes = SMALL_CAP - BEFORE_FRAGMENT_BYTES - FRAGMENT_BYTES - report_bytes;
    info_line_of(second_bytes, 'b', NULL);
    accept_writes();
    skiff_log_flush(logger);
    TEST_PRINTF("log %llu bytes, rotated %llu bytes", (unsigned long long)file_size(path),
                (unsigned long long)file_size(rotated));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(BEFORE_FRAGMENT_BYTES + FRAGMENT_BYTES, file_size(rotated),
                                     "the line break would have passed the cap: rotated");
    TEST_ASSERT_EQUAL_UINT64(report_bytes + second_bytes, file_size(path));
}

static void test_kept_lines_stay_redacted(void) {
    create();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, TOKEN));
    refuse_writes_after(0);
    skiff_log_write(logger, SKIFF_LOG_ERROR, "romm", "token %s refused", TOKEN);
    accept_writes();
    skiff_log_flush(logger);
    TEST_ASSERT_EQUAL_STRING("E romm: token " SKIFF_LOG_REDACTED " refused\n", read_file(path));
}

/* ---- Redaction ---- */

static void test_registered_secrets_never_reach_the_file(void) {
    create();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, TOKEN));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, CF_SECRET));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_OK, skiff_log_add_secret(logger, TOKEN), "twice is fine");
    skiff_log_write(logger, SKIFF_LOG_INFO, "romm", "GET /api/roms?token=%s&x=1", TOKEN);
    skiff_log_write(logger, SKIFF_LOG_INFO, "romm", "%s", TOKEN);
    skiff_log_write(logger, SKIFF_LOG_INFO, "romm", "%s%s and %s", TOKEN, TOKEN, CF_SECRET);
    /* Half from the format string, half from an argument. */
    skiff_log_write(logger, SKIFF_LOG_INFO, "romm", "rmm_01234567%s", "89abcdef");
    skiff_log_flush(logger);
    const char *text = read_file(path);
    TEST_ASSERT_NULL(strstr(text, "0123456789"));
    TEST_ASSERT_NULL(strstr(text, "cf-secret"));
    TEST_ASSERT_EQUAL_STRING("I romm: GET /api/roms?token=" SKIFF_LOG_REDACTED "&x=1\n"
                             "I romm: " SKIFF_LOG_REDACTED "\n"
                             "I romm: " SKIFF_LOG_REDACTED SKIFF_LOG_REDACTED
                             " and " SKIFF_LOG_REDACTED "\n"
                             "I romm: " SKIFF_LOG_REDACTED "\n",
                             text);
}

static void test_twelve_secrets_of_up_to_255_bytes_are_redacted(void) {
    char longest[SKIFF_LOG_SECRET_MAX + 1];
    char name[32];
    create();
    memset(longest, 's', SKIFF_LOG_SECRET_MAX);
    longest[SKIFF_LOG_SECRET_MAX] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, longest));
    for (int i = 1; i < SKIFF_LOG_SECRETS_MAX; i++) {
        snprintf(name, sizeof name, "secret-%02d", i);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, name));
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_OK, skiff_log_add_secret(logger, "secret-05"),
                                  "a known one is fine once the table is full");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_log_add_secret(logger, ""));
    skiff_log_write(logger, SKIFF_LOG_ERROR, "app", "%s|secret-11", longest);
    TEST_ASSERT_EQUAL_STRING("E app: " SKIFF_LOG_REDACTED "|" SKIFF_LOG_REDACTED "\n",
                             read_file(path));
}

/* A value the logger cannot look for must not be printable either: every later message is held
 * back, whatever it contains. */
static void expect_withheld_after(const char *value, skiff_err expected) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_create(&config, &logger));
    skiff_log_write(logger, SKIFF_LOG_ERROR, "app", "before");
    TEST_ASSERT_EQUAL_INT(expected, skiff_log_add_secret(logger, value));
    skiff_log_write(logger, SKIFF_LOG_ERROR, "app", "after %s", value);
    skiff_log_header(logger, SKIFF_LOG_ERROR, "net", "ETag", "\"e\"");
    TEST_ASSERT_EQUAL_STRING("E app: before\nE app: " SKIFF_LOG_WITHHELD
                             "\nE net: " SKIFF_LOG_WITHHELD "\n",
                             read_file(path));
    skiff_log_destroy(logger);
    logger = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(posix, path));
}

static void test_a_secret_that_cannot_be_registered_withholds_every_message(void) {
    char too_long[SKIFF_LOG_SECRET_MAX + 2];
    char name[32];
    memset(too_long, 's', SKIFF_LOG_SECRET_MAX + 1);
    too_long[SKIFF_LOG_SECRET_MAX + 1] = '\0';
    TEST_PRINTF("too short");
    expect_withheld_after("1234567", SKIFF_ERR_INVALID_ARG);
    TEST_PRINTF("too long");
    expect_withheld_after(too_long, SKIFF_ERR_BUFFER_TOO_SMALL);
    TEST_PRINTF("one too many");
    create();
    for (int i = 0; i < SKIFF_LOG_SECRETS_MAX; i++) {
        snprintf(name, sizeof name, "secret-%02d", i);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, name));
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL, skiff_log_add_secret(logger, "one-too-many"));
    skiff_log_write(logger, SKIFF_LOG_ERROR, "app", "one-too-many");
    TEST_ASSERT_EQUAL_STRING("E app: " SKIFF_LOG_WITHHELD "\n", read_file(path));
}

static void test_nested_and_overlapping_secrets_are_redacted_whole(void) {
    create();
    /* The shorter one first: matching it alone would leave the longer one's tail in the clear. */
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, "abcdefgh"));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, "abcdefghijklmnop"));
    /* Overlaps the end of the first: "ghXYZ..." starts inside "abcdefgh". */
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, "ghXYZ12345"));
    skiff_log_write(logger, SKIFF_LOG_ERROR, "app",
                    "a=abcdefghijklmnop b=abcdefgh c=abcdefXYZ12345!");
    TEST_ASSERT_EQUAL_STRING("E app: a=" SKIFF_LOG_REDACTED " b=" SKIFF_LOG_REDACTED
                             " c=abcdefXYZ12345!\n",
                             read_file(path));
    skiff_log_write(logger, SKIFF_LOG_ERROR, "app", "d=abcdefghXYZ12345!");
    TEST_ASSERT_NOT_NULL(strstr(read_file(path), "E app: d=" SKIFF_LOG_REDACTED "!\n"));
}

static void test_a_cut_line_keeps_no_part_of_a_secret(void) {
    create();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, TOKEN));
    char filler[SKIFF_LOG_LINE_MAX];
    /* Puts the token across the cut for every position near the end of the line. */
    for (size_t length = 220; length < SKIFF_LOG_LINE_MAX; length++) {
        memset(filler, 'f', length);
        filler[length] = '\0';
        skiff_log_write(logger, SKIFF_LOG_INFO, "romm", "%s%s tail", filler, TOKEN);
    }
    skiff_log_flush(logger);
    const char *text = read_file(path);
    TEST_ASSERT_NULL_MESSAGE(strstr(text, "fr"), "no line ends in the token's first letter");
    TEST_ASSERT_NULL(strstr(text, "rmm"));
    for (const char *line = text; *line != '\0'; line = strchr(line, '\n') + 1) {
        const size_t length = (size_t)(strchr(line, '\n') - line);
        TEST_ASSERT_LESS_OR_EQUAL_UINT(SKIFF_LOG_LINE_MAX - 1, length);
        const int ends_cut = strncmp(line + length - 3, SKIFF_LOG_CUT_MARKER, 3) == 0;
        const int ends_whole = strncmp(line + length - 5, " tail", 5) == 0;
        TEST_ASSERT_TRUE_MESSAGE(ends_cut || ends_whole,
                                 "a line either ends whole or with the cut marker");
    }
}

/* Formatting stops at twice the line length; redacting two long secrets then shortens the text so
 * much that the cut, inside a secret that has a shorter secret as its start, lands in the line. */
static void test_a_secret_cut_by_formatting_is_redacted_to_the_end(void) {
    char first[201];
    char second[201];
    char filler[93];
    memset(first, 'k', 200);
    first[200] = '\0';
    memset(second, 'q', 200);
    second[200] = '\0';
    memset(filler, 'f', 92);
    filler[92] = '\0';
    create();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, first));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, second));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, "abcdefgh"));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, "abcdefghijklmnop"));
    /* "E app: " + 400 + 92 + 12 bytes of the long nested secret = the 511 that fit. */
    skiff_log_write(logger, SKIFF_LOG_ERROR, "app", "%s%s%sabcdefghijklmnop and more", first,
                    second, filler);
    const char *text = read_file(path);
    TEST_ASSERT_NULL_MESSAGE(strstr(text, "ijkl"), "nothing of the cut secret after its start");
    char expected[SKIFF_LOG_LINE_MAX];
    snprintf(expected, sizeof expected,
             "E app: " SKIFF_LOG_REDACTED SKIFF_LOG_REDACTED
             "%s" SKIFF_LOG_REDACTED SKIFF_LOG_CUT_MARKER "\n",
             filler);
    TEST_ASSERT_EQUAL_STRING(expected, text);
}

static void test_a_long_secret_redacted_leaves_room_for_the_rest(void) {
    char secret[SKIFF_LOG_SECRET_MAX + 1];
    memset(secret, 'k', SKIFF_LOG_SECRET_MAX);
    secret[SKIFF_LOG_SECRET_MAX] = '\0';
    create();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, secret));
    skiff_log_write(logger, SKIFF_LOG_ERROR, "net", "header %s end", secret);
    TEST_ASSERT_EQUAL_STRING("E net: header " SKIFF_LOG_REDACTED " end\n", read_file(path));
}

static void test_control_characters_cannot_forge_lines(void) {
    create();
    skiff_log_write(logger, SKIFF_LOG_ERROR, "romm", "body: %s", "ok\r\nE app: fake\x7f\tend");
    TEST_ASSERT_EQUAL_STRING("E romm: body: ok??E app: fake??end\n", read_file(path));
}

static void test_headers_show_only_harmless_values(void) {
    create();
    skiff_log_header(logger, SKIFF_LOG_INFO, "net", "Authorization", "Bearer " TOKEN);
    skiff_log_header(logger, SKIFF_LOG_INFO, "net", "CF-Access-Client-Secret", CF_SECRET);
    skiff_log_header(logger, SKIFF_LOG_INFO, "net", "Cookie", "session=abc");
    skiff_log_header(logger, SKIFF_LOG_INFO, "net", "ETag", "\"6ac37763-1000\"");
    skiff_log_header(logger, SKIFF_LOG_INFO, "net", "content-length", "4096");
    skiff_log_header(logger, SKIFF_LOG_INFO, "net", "Content-Range", "bytes 0-1/2");
    skiff_log_header(logger, SKIFF_LOG_INFO, "net", "Content-Type", NULL);
    skiff_log_header(logger, SKIFF_LOG_DEBUG, "net", "ETag", "left out");
    skiff_log_flush(logger);
    TEST_ASSERT_EQUAL_STRING("I net: Authorization: " SKIFF_LOG_REDACTED "\n"
                             "I net: CF-Access-Client-Secret: " SKIFF_LOG_REDACTED "\n"
                             "I net: Cookie: " SKIFF_LOG_REDACTED "\n"
                             "I net: ETag: \"6ac37763-1000\"\n"
                             "I net: content-length: 4096\n"
                             "I net: Content-Range: bytes 0-1/2\n"
                             "I net: Content-Type: " SKIFF_LOG_REDACTED "\n",
                             read_file(path));
}

/* ---- Threads ---- */

static void test_the_lock_is_held_around_every_call_and_never_nested(void) {
    config.lock = counting_lock;
    config.unlock = counting_unlock;
    config.lock_ctx = &locks;
    config.buffer_bytes = SKIFF_LOG_LINE_MAX;
    create();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_add_secret(logger, TOKEN));
    for (int i = 0; i < 5; i++) {
        skiff_log_write(logger, SKIFF_LOG_INFO, "jobs", "%0100d", i);
    }
    skiff_log_write(logger, SKIFF_LOG_ERROR, "jobs", "error");
    skiff_log_header(logger, SKIFF_LOG_INFO, "net", "ETag", "\"e\"");
    skiff_log_flush(logger);
    skiff_log_destroy(logger);
    logger = NULL;
    TEST_PRINTF("lock taken %d times, deepest %d", locks.taken, locks.deepest);
    TEST_ASSERT_EQUAL_INT(0, locks.depth);
    TEST_ASSERT_EQUAL_INT(1, locks.deepest);
    TEST_ASSERT_EQUAL_INT(10, locks.taken);
    TEST_ASSERT_EQUAL_INT(7, count_lines(read_file(path)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_timestamps_follow_the_gregorian_calendar);
    RUN_TEST(test_timestamps_outside_four_digit_years_or_buffers_are_refused);
    RUN_TEST(test_level_names_round_trip_and_ignore_case);
    RUN_TEST(test_create_refuses_unusable_settings);
    RUN_TEST(test_lines_below_the_level_are_left_out);
    RUN_TEST(test_null_logger_and_arguments_are_ignored);
    RUN_TEST(test_lines_start_with_the_clock_when_it_can_be_read);
    RUN_TEST(test_lines_go_without_a_timestamp_when_the_clock_fails);
    RUN_TEST(test_info_lines_wait_in_memory_until_a_flush);
    RUN_TEST(test_warnings_and_errors_are_written_at_once);
    RUN_TEST(test_a_full_buffer_is_written_before_the_next_line);
    RUN_TEST(test_batches_are_appended_to_an_existing_log);
    RUN_TEST(test_a_log_at_the_cap_stays_and_one_past_it_rotates);
    RUN_TEST(test_a_second_rotation_replaces_the_older_file);
    RUN_TEST(test_a_failed_rotation_starts_the_log_over_rather_than_pass_the_cap);
    RUN_TEST(test_a_handle_lost_to_a_suspend_is_reopened_without_repeating_lines);
    RUN_TEST(test_a_refused_batch_stays_buffered_and_goes_out_with_the_next);
    RUN_TEST(test_a_warning_logged_right_after_waking_waits_until_the_memory_stick_takes_it);
    RUN_TEST(test_lines_wait_in_memory_while_the_memory_stick_refuses);
    RUN_TEST(test_part_of_a_refused_batch_in_the_file_is_written_over);
    RUN_TEST(test_a_batch_cut_short_after_a_failed_plan_is_written_over_too);
    RUN_TEST(test_a_batch_that_cannot_be_synced_is_written_again_in_place);
    RUN_TEST(test_a_batch_tried_again_is_checked_against_the_cap_where_it_goes);
    RUN_TEST(test_a_batch_tried_again_past_the_cap_rotates_and_leaves_its_part_behind);
    RUN_TEST(test_a_full_buffer_gives_up_its_oldest_lines_while_the_memory_stick_refuses);
    RUN_TEST(test_a_refusing_memory_stick_gets_no_try_per_line_and_the_count_is_exact);
    RUN_TEST(test_a_report_cut_short_is_written_over_not_repeated);
    RUN_TEST(test_a_part_whose_first_lines_were_given_up_gets_a_line_of_its_own);
    RUN_TEST(test_the_line_break_after_a_part_counts_against_the_cap);
    RUN_TEST(test_kept_lines_stay_redacted);
    RUN_TEST(test_registered_secrets_never_reach_the_file);
    RUN_TEST(test_twelve_secrets_of_up_to_255_bytes_are_redacted);
    RUN_TEST(test_a_secret_that_cannot_be_registered_withholds_every_message);
    RUN_TEST(test_nested_and_overlapping_secrets_are_redacted_whole);
    RUN_TEST(test_a_cut_line_keeps_no_part_of_a_secret);
    RUN_TEST(test_a_secret_cut_by_formatting_is_redacted_to_the_end);
    RUN_TEST(test_a_long_secret_redacted_leaves_room_for_the_rest);
    RUN_TEST(test_control_characters_cannot_forge_lines);
    RUN_TEST(test_headers_show_only_harmless_values);
    RUN_TEST(test_the_lock_is_held_around_every_call_and_never_nested);
    return UNITY_END();
}
