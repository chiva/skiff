/*
 * What Skiff says to the player (skiff/i18n.h): both languages have every string, use the same
 * placeholders, and only characters the PSP's Latin firmware font can draw; errors read as error.h
 * says in English and have their own Spanish sentence; placeholders fill safely.
 */
#include <stdio.h>
#include <string.h>

#include "skiff/i18n.h"

#include "unity.h"

/* The highest code point the Latin firmware font (ltn0.pgf) covers: ASCII and Latin-1. */
#define LATIN_FONT_LAST_CODE_POINT 0xFFU
#define PLACEHOLDERS_MAX 9

static const skiff_language LANGUAGES[] = {SKIFF_LANGUAGE_ENGLISH, SKIFF_LANGUAGE_SPANISH};

static const skiff_err ERRORS[] = {
#define TEST_ERROR_ROW(name, value, message) name,
    SKIFF_ERROR_TABLE(TEST_ERROR_ROW)
#undef TEST_ERROR_ROW
};

void setUp(void) {}

void tearDown(void) {}

/*
 * 1 if text is UTF-8 without control characters, every character within the Latin font's range;
 * the decoder accepts the one- and two-byte forms the range needs and nothing longer.
 */
static int drawable_by_latin_font(const char *text) {
    const unsigned char *byte = (const unsigned char *)text;
    while (*byte != '\0') {
        unsigned code_point = 0;
        if (*byte < 0x80U) {
            code_point = *byte;
            byte++;
        } else if ((*byte & 0xE0U) == 0xC0U && (byte[1] & 0xC0U) == 0x80U) {
            code_point = ((*byte & 0x1FU) << 6) | (byte[1] & 0x3FU);
            if (code_point < 0x80U) {
                return 0; /* overlong */
            }
            byte += 2;
        } else {
            return 0;
        }
        if (code_point < 0x20U || (code_point >= 0x7FU && code_point < 0xA0U) ||
            code_point > LATIN_FONT_LAST_CODE_POINT) {
            return 0;
        }
    }
    return 1;
}

/* Bit n-1 set for every {n} in text; *stray set for a brace that is not a placeholder. */
static unsigned placeholders_used(const char *text, int *stray) {
    unsigned used = 0;
    *stray = 0;
    for (const char *c = text; *c != '\0'; c++) {
        if (*c == '{' && c[1] >= '1' && c[1] <= '9' && c[2] == '}') {
            used |= 1U << (unsigned)(c[1] - '1');
            c += 2;
        } else if (*c == '{' || *c == '}') {
            *stray = 1;
        }
    }
    return used;
}

static void test_every_text_exists_in_both_languages_with_its_placeholders(void) {
    for (int id = 0; id < SKIFF_TEXT_COUNT; id++) {
        const char *name = skiff_text_name((skiff_text_id)id);
        const int placeholders = skiff_text_placeholders((skiff_text_id)id);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_EQUAL_INT(0, strncmp(name, "SKIFF_TEXT_", strlen("SKIFF_TEXT_")));
        TEST_ASSERT_TRUE(placeholders >= 0 && placeholders <= PLACEHOLDERS_MAX);
        const unsigned expected = (1U << (unsigned)placeholders) - 1U;
        for (size_t l = 0; l < sizeof LANGUAGES / sizeof LANGUAGES[0]; l++) {
            const char *text = skiff_text(LANGUAGES[l], (skiff_text_id)id);
            int stray = 0;
            const unsigned used = placeholders_used(text, &stray);
            TEST_PRINTF("%s [%zu]: \"%s\" placeholders %d", name, l, text, placeholders);
            TEST_ASSERT_TRUE_MESSAGE(text[0] != '\0', name);
            TEST_ASSERT_EQUAL_HEX_MESSAGE(expected, used, name);
            TEST_ASSERT_FALSE_MESSAGE(stray, name);
            TEST_ASSERT_TRUE_MESSAGE(drawable_by_latin_font(text), name);
        }
    }
    TEST_PRINTF("%d texts in each language", (int)SKIFF_TEXT_COUNT);
}

static void test_every_error_has_a_spanish_sentence_and_english_is_error_h(void) {
    for (size_t i = 0; i < sizeof ERRORS / sizeof ERRORS[0]; i++) {
        const char *english = skiff_error_text(SKIFF_LANGUAGE_ENGLISH, ERRORS[i]);
        const char *spanish = skiff_error_text(SKIFF_LANGUAGE_SPANISH, ERRORS[i]);
        TEST_PRINTF("%s: \"%s\" / \"%s\"", skiff_err_name(ERRORS[i]), english, spanish);
        TEST_ASSERT_EQUAL_STRING(skiff_err_message(ERRORS[i]), english);
        TEST_ASSERT_TRUE_MESSAGE(strcmp(english, spanish) != 0, skiff_err_name(ERRORS[i]));
        TEST_ASSERT_TRUE_MESSAGE(drawable_by_latin_font(spanish), skiff_err_name(ERRORS[i]));
        TEST_ASSERT_TRUE_MESSAGE(drawable_by_latin_font(english), skiff_err_name(ERRORS[i]));
    }
}

static void test_unknown_errors_and_languages_fall_back_to_english(void) {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const skiff_err unknown = (skiff_err)999;
    TEST_ASSERT_EQUAL_STRING(skiff_err_message(unknown),
                             skiff_error_text(SKIFF_LANGUAGE_SPANISH, unknown));
    TEST_ASSERT_EQUAL_STRING(skiff_err_message(SKIFF_ERR_NET_DNS),
                             skiff_error_text(SKIFF_LANGUAGE_COUNT, SKIFF_ERR_NET_DNS));
    TEST_ASSERT_EQUAL_STRING(
        "Library", skiff_text((skiff_language)SKIFF_LANGUAGE_COUNT, SKIFF_TEXT_TITLE_LIBRARY));
    TEST_ASSERT_EQUAL_STRING("", skiff_text(SKIFF_LANGUAGE_SPANISH, SKIFF_TEXT_COUNT));
    TEST_ASSERT_NULL(skiff_text_name(SKIFF_TEXT_COUNT));
    TEST_ASSERT_EQUAL_INT(-1, skiff_text_placeholders(SKIFF_TEXT_COUNT));
}

static void test_spanish_only_for_a_spanish_psp(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_LANGUAGE_SPANISH,
                          skiff_language_from_psp(SKIFF_PSP_SYSTEM_LANGUAGE_SPANISH));
    const int others[] = {0, 1, 2, 4, 7, 11, -1, 99};
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
        TEST_ASSERT_EQUAL_INT(SKIFF_LANGUAGE_ENGLISH, skiff_language_from_psp(others[i]));
    }
    TEST_ASSERT_EQUAL_STRING("Biblioteca",
                             skiff_text(skiff_language_from_psp(SKIFF_PSP_SYSTEM_LANGUAGE_SPANISH),
                                        SKIFF_TEXT_TITLE_LIBRARY));
}

static void test_placeholders_fill_in_any_order(void) {
    char out[SKIFF_TEXT_MAX];
    const char *args[] = {"1,5 GB", "3,2 GB", "46"};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_text_format(skiff_text(SKIFF_LANGUAGE_SPANISH, SKIFF_TEXT_PROGRESS),
                                            args, 3, out, sizeof out));
    TEST_PRINTF("%s", out);
    TEST_ASSERT_EQUAL_STRING("1,5 GB de 3,2 GB (46 %)", out);
    const char *pair[] = {"first", "second"};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_text_format("{2} then {1}, {1} again", pair, 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("second then first, first again", out);
}

static void test_braces_that_are_not_placeholders_stay_as_they_are(void) {
    char out[SKIFF_TEXT_MAX];
    const char *args[] = {"x"};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_text_format("{0} {10} {a} { {1 } {1}{", args, 1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("{0} {10} {a} { {1 } x{", out);
    TEST_PRINTF("a placeholder past the arguments, or a NULL argument, expands to nothing");
    const char *with_null[] = {NULL};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_text_format("[{1}][{2}]", with_null, 1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("[][]", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_text_format("no args {1}", NULL, 0, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("no args ", out);
}

static void test_a_text_too_long_is_cut_between_characters(void) {
    char out[8];
    const char *args[] = {"ñññ"};
    TEST_PRINTF("\"ab\" + \"ñññ\" is 8 bytes; 7 fit, the last ñ would be split");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_text_format("ab{1}", args, 1, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("abññ", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_text_format("abcdefghij", NULL, 0, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("abcdefg", out);
    char one[1];
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL, skiff_text_format("a", NULL, 0, one, 1));
    TEST_ASSERT_EQUAL_STRING("", one);
}

static void test_format_refuses_bad_arguments(void) {
    char out[4] = "xyz";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_text_format(NULL, NULL, 0, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_text_format("a", NULL, 1, out, sizeof out));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_text_format("a", NULL, 0, NULL, 4));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_text_format("a", NULL, 0, out, 0));
}

static void test_an_error_line_carries_its_code(void) {
    char out[SKIFF_TEXT_MAX];
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_error_line(SKIFF_LANGUAGE_ENGLISH, SKIFF_ERR_NET_TIMEOUT, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("The RomM server took too long to answer [103]", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_error_line(SKIFF_LANGUAGE_SPANISH,
                                                     SKIFF_ERR_STORAGE_NO_SPACE, out, sizeof out));
    TEST_PRINTF("%s", out);
    TEST_ASSERT_EQUAL_STRING("No hay suficiente espacio libre en el Memory Stick [301]", out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_text_exists_in_both_languages_with_its_placeholders);
    RUN_TEST(test_every_error_has_a_spanish_sentence_and_english_is_error_h);
    RUN_TEST(test_unknown_errors_and_languages_fall_back_to_english);
    RUN_TEST(test_spanish_only_for_a_spanish_psp);
    RUN_TEST(test_placeholders_fill_in_any_order);
    RUN_TEST(test_braces_that_are_not_placeholders_stay_as_they_are);
    RUN_TEST(test_a_text_too_long_is_cut_between_characters);
    RUN_TEST(test_format_refuses_bad_arguments);
    RUN_TEST(test_an_error_line_carries_its_code);
    return UNITY_END();
}
