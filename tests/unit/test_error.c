#include <string.h>

#include "skiff/error.h"

#include "unity.h"

#define SKIFF_ERROR_ROW(name, value, message) {name, #name, message},

typedef struct error_row {
    skiff_err code;
    const char *name;
    const char *message;
} error_row;

static const error_row ROWS[] = {SKIFF_ERROR_TABLE(SKIFF_ERROR_ROW)};
static const size_t ROW_COUNT = sizeof ROWS / sizeof ROWS[0];

void setUp(void) {}

void tearDown(void) {}

static void test_every_code_resolves_to_its_own_name_and_message(void) {
    for (size_t i = 0; i < ROW_COUNT; i++) {
        TEST_PRINTF("code %d -> %s", (int)ROWS[i].code, ROWS[i].name);
        TEST_ASSERT_EQUAL_STRING(ROWS[i].name, skiff_err_name(ROWS[i].code));
        TEST_ASSERT_EQUAL_STRING(ROWS[i].message, skiff_err_message(ROWS[i].code));
    }
}

static void test_codes_are_unique(void) {
    for (size_t i = 0; i < ROW_COUNT; i++) {
        for (size_t j = i + 1; j < ROW_COUNT; j++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(ROWS[i].code, ROWS[j].code, ROWS[j].name);
        }
    }
}

static void test_messages_are_player_facing_sentences(void) {
    for (size_t i = 0; i < ROW_COUNT; i++) {
        size_t length = strlen(ROWS[i].message);
        TEST_ASSERT_GREATER_THAN_size_t_MESSAGE(0, length, ROWS[i].name);
        TEST_ASSERT_NOT_EQUAL_MESSAGE('.', ROWS[i].message[length - 1], ROWS[i].name);
    }
}

static void test_unknown_code_falls_back_to_generic_text(void) {
    /* Deliberately out of range: simulates a corrupted or future code reaching the lookup. */
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const skiff_err unknown = (skiff_err)-12345;
    TEST_ASSERT_EQUAL_STRING("SKIFF_ERR_UNKNOWN", skiff_err_name(unknown));
    TEST_ASSERT_EQUAL_STRING("Unexpected error", skiff_err_message(unknown));
}

static void test_ok_is_zero(void) { TEST_ASSERT_EQUAL_INT(0, SKIFF_OK); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_code_resolves_to_its_own_name_and_message);
    RUN_TEST(test_codes_are_unique);
    RUN_TEST(test_messages_are_player_facing_sentences);
    RUN_TEST(test_unknown_code_falls_back_to_generic_text);
    RUN_TEST(test_ok_is_zero);
    return UNITY_END();
}
