#include <stdio.h>
#include <string.h>

#include "skiff/selftest.h"

#include "unity.h"

enum { CAPTURE_MAX_LINES = 32 };

typedef struct capture {
    char lines[CAPTURE_MAX_LINES][SKIFF_SELFTEST_LINE_MAX];
    int count;
} capture;

static capture captured;

static void capture_line(void *ctx, const char *line) {
    capture *sink = ctx;
    printf("  selftest> %s\n", line);
    if (sink->count < CAPTURE_MAX_LINES) {
        snprintf(sink->lines[sink->count], SKIFF_SELFTEST_LINE_MAX, "%s", line);
        sink->count++;
    }
}

void setUp(void) { memset(&captured, 0, sizeof captured); }

void tearDown(void) {}

static void test_all_checks_pass_on_host(void) {
    skiff_selftest_result result = skiff_selftest_run(capture_line, &captured);
    TEST_ASSERT_EQUAL_INT(0, result.failed);
    TEST_ASSERT_GREATER_THAN_INT(0, result.passed);
}

static void test_emits_banner_then_one_line_per_check_then_marker(void) {
    skiff_selftest_result result = skiff_selftest_run(capture_line, &captured);
    int checks = result.passed + result.failed;
    TEST_ASSERT_EQUAL_INT(checks + 2, captured.count);
    TEST_ASSERT_EQUAL_INT(0, strncmp(captured.lines[0], "skiff ", strlen("skiff ")));
    for (int i = 1; i <= checks; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(captured.lines[i], "PASS ", strlen("PASS ")),
                                      captured.lines[i]);
    }
}

static void test_final_line_carries_ok_marker_for_ci_grep(void) {
    skiff_selftest_result result = skiff_selftest_run(capture_line, &captured);
    char expected[SKIFF_SELFTEST_LINE_MAX];
    snprintf(expected, sizeof expected, "%s %d/%d", SKIFF_SELFTEST_OK_MARKER, result.passed,
             result.passed);
    TEST_ASSERT_EQUAL_STRING(expected, captured.lines[captured.count - 1]);
}

static void test_null_logger_is_allowed(void) {
    skiff_selftest_result result = skiff_selftest_run(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, result.failed);
}

enum { RESULT_PATH_MAX = 128 };

static void assert_result_path(const char *program_path, const char *expected) {
    char out[RESULT_PATH_MAX];
    const skiff_err err = skiff_selftest_result_path(program_path, out, sizeof out);
    TEST_PRINTF("%s -> %s (%s)", program_path, out, skiff_err_name(err));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, err);
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

static void test_result_path_sits_next_to_an_eboot_on_the_memory_stick(void) {
    assert_result_path("ms0:/PSP/GAME/SkiffSelftest/EBOOT.PBP",
                       "ms0:/PSP/GAME/SkiffSelftest/result.txt");
}

static void test_result_path_sits_next_to_a_prx_run_over_psplink(void) {
    assert_result_path("host0:/build/psp/skiff_selftest.prx", "host0:/build/psp/result.txt");
}

static void test_result_path_keeps_the_root_separator(void) {
    assert_result_path("ms0:/EBOOT.PBP", "ms0:/result.txt");
}

static void test_result_path_rejects_a_path_without_a_directory(void) {
    char out[RESULT_PATH_MAX] = "stale";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_selftest_result_path("EBOOT.PBP", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_result_path_rejects_null_arguments(void) {
    char out[RESULT_PATH_MAX] = "stale";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_selftest_result_path(NULL, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_selftest_result_path("ms0:/EBOOT.PBP", NULL, RESULT_PATH_MAX));
}

/* "ms0:/" + "result.txt" + NUL is exactly 16 bytes: 16 fits, 15 does not. */
static void test_result_path_needs_room_for_the_terminator(void) {
    char out[RESULT_PATH_MAX];
    const size_t exact = strlen("ms0:/result.txt") + 1;

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_selftest_result_path("ms0:/EBOOT.PBP", out, exact));
    TEST_ASSERT_EQUAL_STRING("ms0:/result.txt", out);

    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_selftest_result_path("ms0:/EBOOT.PBP", out, exact - 1));
    TEST_ASSERT_EQUAL_STRING("", out);

    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_selftest_result_path("ms0:/EBOOT.PBP", out, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_all_checks_pass_on_host);
    RUN_TEST(test_emits_banner_then_one_line_per_check_then_marker);
    RUN_TEST(test_final_line_carries_ok_marker_for_ci_grep);
    RUN_TEST(test_null_logger_is_allowed);
    RUN_TEST(test_result_path_sits_next_to_an_eboot_on_the_memory_stick);
    RUN_TEST(test_result_path_sits_next_to_a_prx_run_over_psplink);
    RUN_TEST(test_result_path_keeps_the_root_separator);
    RUN_TEST(test_result_path_rejects_a_path_without_a_directory);
    RUN_TEST(test_result_path_rejects_null_arguments);
    RUN_TEST(test_result_path_needs_room_for_the_terminator);
    return UNITY_END();
}
