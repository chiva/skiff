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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_all_checks_pass_on_host);
    RUN_TEST(test_emits_banner_then_one_line_per_check_then_marker);
    RUN_TEST(test_final_line_carries_ok_marker_for_ci_grep);
    RUN_TEST(test_null_logger_is_allowed);
    return UNITY_END();
}
