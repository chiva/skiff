/*
 * The self-test's checks as the host runs them, on the POSIX storage over a temporary directory
 * (app: is a folder in it), and its result file's path.
 */
#include <stdio.h>
#include <string.h>

#include "skiff/selftest.h"

#include "fake_storage.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

enum { CAPTURE_MAX_LINES = 32 };

typedef struct capture {
    char lines[CAPTURE_MAX_LINES][SKIFF_SELFTEST_LINE_MAX];
    int count;
} capture;

static capture captured;
static char dir[TEMP_DIR_PATH_MAX];
static char app_dir[TEMP_DIR_PATH_MAX];
static skiff_storage *storage;
static skiff_storage_roots roots;
static skiff_selftest_env env;

static void capture_line(void *ctx, const char *line) {
    capture *sink = ctx;
    printf("  selftest> %s\n", line);
    if (sink->count < CAPTURE_MAX_LINES) {
        snprintf(sink->lines[sink->count], SKIFF_SELFTEST_LINE_MAX, "%s", line);
        sink->count++;
    }
}

void setUp(void) {
    memset(&captured, 0, sizeof captured);
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    TEST_ASSERT_TRUE(temp_dir_path(dir, "Skiff", app_dir, sizeof app_dir));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&storage));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdir(storage, app_dir));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_init(dir, app_dir, &roots));
    env.storage = storage;
    env.roots = &roots;
}

void tearDown(void) {
    skiff_storage_destroy(storage);
    temp_dir_remove(dir);
}

/* The line a check printed, or NULL when it did not run. */
static const char *line_of(const char *check) {
    for (int i = 0; i < captured.count; i++) {
        const char *name = strchr(captured.lines[i], ' ');
        if (name != NULL && strncmp(name + 1, check, strlen(check)) == 0) {
            return captured.lines[i];
        }
    }
    return NULL;
}

static void assert_scratch_folder_gone(void) {
    char scratch[SKIFF_STORAGE_PATH_MAX];
    skiff_storage_info info;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, SKIFF_SELFTEST_SCRATCH_FOLDER,
                                                          scratch, sizeof scratch));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_STORAGE_NOT_FOUND,
                                  skiff_storage_stat(storage, scratch, &info),
                                  "the scratch folder is removed after the check");
}

static void test_all_checks_pass_on_host(void) {
    skiff_selftest_result result = skiff_selftest_run(&env, capture_line, &captured);
    TEST_ASSERT_EQUAL_INT(0, result.failed);
    TEST_ASSERT_GREATER_THAN_INT(0, result.passed);
    TEST_ASSERT_EQUAL_STRING("PASS storage-list", line_of("storage-list"));
    assert_scratch_folder_gone();
}

static void test_storage_checks_fail_without_a_storage(void) {
    const skiff_selftest_env none = {NULL, NULL};
    const skiff_selftest_env no_roots = {storage, NULL};
    skiff_selftest_result result = skiff_selftest_run(NULL, capture_line, &captured);
    TEST_ASSERT_EQUAL_INT(1, result.failed);
    TEST_ASSERT_EQUAL_STRING("FAIL storage-list: no storage to check", line_of("storage-list"));
    memset(&captured, 0, sizeof captured);
    TEST_ASSERT_EQUAL_INT(1, skiff_selftest_run(&none, capture_line, &captured).failed);
    memset(&captured, 0, sizeof captured);
    TEST_ASSERT_EQUAL_INT(1, skiff_selftest_run(&no_roots, capture_line, &captured).failed);
}

/* The scratch folder's paths, resolved under app:. */
static void scratch_path(const char *logical, char *out, size_t out_size) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, logical, out, out_size));
}

/* What a check that could not clean up left behind, removed through the real storage. */
static void remove_scratch_leftovers(void) {
    char path[SKIFF_STORAGE_PATH_MAX];
    scratch_path(SKIFF_SELFTEST_SCRATCH_FOLDER "/file.bin", path, sizeof path);
    (void)skiff_storage_remove(storage, path);
    scratch_path(SKIFF_SELFTEST_SCRATCH_FOLDER "/folder", path, sizeof path);
    (void)skiff_storage_rmdir(storage, path);
    scratch_path(SKIFF_SELFTEST_SCRATCH_FOLDER, path, sizeof path);
    (void)skiff_storage_rmdir(storage, path);
}

typedef enum storage_fault {
    FAULT_MKDIR,
    FAULT_LIST,
    FAULT_STAT,
    FAULT_RMDIR,
} storage_fault;

static void inject(fake_storage *fake, storage_fault fault, skiff_err error) {
    switch (fault) {
    case FAULT_MKDIR:
        fake->mkdir_error = error;
        break;
    case FAULT_LIST:
        fake->list_error = error;
        fake->list_error_after = 1;
        break;
    case FAULT_STAT:
        fake->stat_error = error;
        break;
    case FAULT_RMDIR:
    default:
        fake->rmdir_error = error;
        break;
    }
}

/* Each storage step failing in turn: the check names it and removes what it made, unless the
 * failing step is the removal itself. */
static void test_storage_list_reports_the_failing_step(void) {
    static const struct {
        const char *suffix;
        storage_fault fault;
        skiff_err error;
        int cleans_up;
        const char *expected;
    } CASES[] = {
        {"selftest-scratch", FAULT_MKDIR, SKIFF_ERR_STORAGE_IO, 1,
         "FAIL storage-list: could not create a scratch folder under app: (SKIFF_ERR_STORAGE_IO)"},
        {"folder", FAULT_MKDIR, SKIFF_ERR_STORAGE_IO, 1,
         "FAIL storage-list: could not fill the scratch folder (SKIFF_ERR_STORAGE_IO)"},
        {"selftest-scratch", FAULT_LIST, SKIFF_ERR_STORAGE_IO, 1,
         "FAIL storage-list: the scratch folder listed other entries than its file and folder "
         "(SKIFF_ERR_STORAGE_IO)"},
        {"file.bin", FAULT_STAT, SKIFF_ERR_STORAGE_IO, 1,
         "FAIL storage-list: a file's size or date disagreed between the listing and stat "
         "(SKIFF_ERR_STORAGE_IO)"},
        {"folder", FAULT_STAT, SKIFF_ERR_STORAGE_IO, 1,
         "FAIL storage-list: stat did not see a folder (SKIFF_ERR_STORAGE_IO)"},
        {"folder", FAULT_RMDIR, SKIFF_ERR_STORAGE_NO_MEDIA, 0,
         "FAIL storage-list: an empty folder was not removed (SKIFF_ERR_STORAGE_NO_MEDIA)"},
        {"selftest-scratch", FAULT_RMDIR, SKIFF_ERR_STORAGE_IO, 0,
         "FAIL storage-list: the emptied scratch folder was not removed (SKIFF_ERR_STORAGE_IO)"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        fake_storage fake;
        fake_storage_init(&fake, storage);
        fake.fail_suffix = CASES[i].suffix;
        inject(&fake, CASES[i].fault, CASES[i].error);
        const skiff_selftest_env faked = {&fake.base, &roots};
        memset(&captured, 0, sizeof captured);
        const skiff_selftest_result result = skiff_selftest_run(&faked, capture_line, &captured);
        TEST_PRINTF("fault %d on \"%s\": %s", (int)CASES[i].fault, CASES[i].suffix,
                    line_of("storage-list"));
        TEST_ASSERT_EQUAL_INT(1, result.failed);
        TEST_ASSERT_EQUAL_STRING(CASES[i].expected, line_of("storage-list"));
        if (CASES[i].cleans_up) {
            assert_scratch_folder_gone();
        } else {
            remove_scratch_leftovers();
        }
        skiff_storage_destroy(&fake.base);
    }
    assert_scratch_folder_gone();
}

/* An app: folder so deep the scratch folder's path passes SKIFF_STORAGE_PATH_MAX. */
static void test_storage_list_fails_when_the_scratch_folder_does_not_fit(void) {
    char deep[SKIFF_STORAGE_PATH_MAX - 8];
    memset(deep, 'd', sizeof deep - 1);
    deep[0] = '/';
    deep[sizeof deep - 1] = '\0';
    skiff_storage_roots deep_roots;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_init(dir, deep, &deep_roots));
    const skiff_selftest_env deep_env = {storage, &deep_roots};
    TEST_ASSERT_EQUAL_INT(1, skiff_selftest_run(&deep_env, capture_line, &captured).failed);
    TEST_ASSERT_EQUAL_STRING("FAIL storage-list: the scratch folder does not fit under app:",
                             line_of("storage-list"));
}

static void test_emits_banner_then_one_line_per_check_then_marker(void) {
    skiff_selftest_result result = skiff_selftest_run(&env, capture_line, &captured);
    int checks = result.passed + result.failed;
    TEST_ASSERT_EQUAL_INT(checks + 2, captured.count);
    TEST_ASSERT_EQUAL_INT(0, strncmp(captured.lines[0], "skiff ", strlen("skiff ")));
    for (int i = 1; i <= checks; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(captured.lines[i], "PASS ", strlen("PASS ")),
                                      captured.lines[i]);
    }
}

static void test_final_line_carries_ok_marker_for_ci_grep(void) {
    skiff_selftest_result result = skiff_selftest_run(&env, capture_line, &captured);
    char expected[SKIFF_SELFTEST_LINE_MAX];
    snprintf(expected, sizeof expected, "%s %d/%d", SKIFF_SELFTEST_OK_MARKER, result.passed,
             result.passed);
    TEST_ASSERT_EQUAL_STRING(expected, captured.lines[captured.count - 1]);
}

static void test_null_logger_is_allowed(void) {
    skiff_selftest_result result = skiff_selftest_run(&env, NULL, NULL);
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
    RUN_TEST(test_storage_checks_fail_without_a_storage);
    RUN_TEST(test_storage_list_reports_the_failing_step);
    RUN_TEST(test_storage_list_fails_when_the_scratch_folder_does_not_fit);
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
