/*
 * The UI's models (skiff/ui.h): buttons become actions with the console's confirm button and a
 * held d-pad repeats; a list scrolls and wraps; text is cut to a width between UTF-8 characters;
 * UTF-8 to and from the system dialogs' UTF-16, with invalid input replaced;
 * progress, rate and time left; sizes and durations as the player reads them.
 */
#include <stdio.h>
#include <string.h>

#include "skiff/ui.h"

#include "unity.h"

#define MIB (1024ULL * 1024ULL)
#define GIB (1024ULL * MIB)

void setUp(void) {}

void tearDown(void) {}

/* ---- Buttons and actions ---- */

static void test_confirm_and_back_follow_the_console_setting(void) {
    skiff_ui_input western;
    skiff_ui_input japanese;
    skiff_ui_input_init(&western, 1);
    skiff_ui_input_init(&japanese, 0);
    TEST_ASSERT_EQUAL_HEX(SKIFF_UI_ACTION_CONFIRM,
                          skiff_ui_input_update(&western, SKIFF_UI_BUTTON_CROSS));
    TEST_ASSERT_EQUAL_HEX(SKIFF_UI_ACTION_BACK,
                          skiff_ui_input_update(&japanese, SKIFF_UI_BUTTON_CROSS));
    skiff_ui_input_update(&western, 0);
    skiff_ui_input_update(&japanese, 0);
    TEST_ASSERT_EQUAL_HEX(SKIFF_UI_ACTION_BACK,
                          skiff_ui_input_update(&western, SKIFF_UI_BUTTON_CIRCLE));
    TEST_ASSERT_EQUAL_HEX(SKIFF_UI_ACTION_CONFIRM,
                          skiff_ui_input_update(&japanese, SKIFF_UI_BUTTON_CIRCLE));
}

static void test_every_button_has_its_action(void) {
    const unsigned buttons[] = {SKIFF_UI_BUTTON_UP,       SKIFF_UI_BUTTON_DOWN,
                                SKIFF_UI_BUTTON_LEFT,     SKIFF_UI_BUTTON_RIGHT,
                                SKIFF_UI_BUTTON_L,        SKIFF_UI_BUTTON_R,
                                SKIFF_UI_BUTTON_TRIANGLE, SKIFF_UI_BUTTON_SQUARE,
                                SKIFF_UI_BUTTON_START,    SKIFF_UI_BUTTON_SELECT};
    const unsigned actions[] = {SKIFF_UI_ACTION_UP,      SKIFF_UI_ACTION_DOWN,
                                SKIFF_UI_ACTION_LEFT,    SKIFF_UI_ACTION_RIGHT,
                                SKIFF_UI_ACTION_PAGE_UP, SKIFF_UI_ACTION_PAGE_DOWN,
                                SKIFF_UI_ACTION_MENU,    SKIFF_UI_ACTION_EXTRA,
                                SKIFF_UI_ACTION_START,   SKIFF_UI_ACTION_SELECT};
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++) {
        skiff_ui_input input;
        skiff_ui_input_init(&input, 1);
        TEST_ASSERT_EQUAL_HEX(actions[i], skiff_ui_input_update(&input, buttons[i]));
    }
    TEST_ASSERT_EQUAL_HEX(0, skiff_ui_input_update(NULL, SKIFF_UI_BUTTON_UP));
}

/* Frames until the action comes again, holding buttons, up to limit. */
static int frames_until_action(skiff_ui_input *input, unsigned buttons, unsigned action,
                               int limit) {
    for (int frame = 1; frame <= limit; frame++) {
        if ((skiff_ui_input_update(input, buttons) & action) != 0) {
            return frame;
        }
    }
    return -1;
}

static void test_a_held_direction_repeats_after_a_delay(void) {
    skiff_ui_input input;
    skiff_ui_input_init(&input, 1);
    TEST_ASSERT_EQUAL_HEX(SKIFF_UI_ACTION_DOWN,
                          skiff_ui_input_update(&input, SKIFF_UI_BUTTON_DOWN));
    const int first = frames_until_action(&input, SKIFF_UI_BUTTON_DOWN, SKIFF_UI_ACTION_DOWN, 100);
    const int second = frames_until_action(&input, SKIFF_UI_BUTTON_DOWN, SKIFF_UI_ACTION_DOWN, 100);
    TEST_PRINTF("first repeat after %d frames, then every %d", first, second);
    TEST_ASSERT_EQUAL_INT(SKIFF_UI_REPEAT_DELAY_FRAMES, first);
    TEST_ASSERT_EQUAL_INT(SKIFF_UI_REPEAT_INTERVAL_FRAMES, second);
    TEST_PRINTF("a shoulder button repeats too: paging through a long list");
    skiff_ui_input_init(&input, 1);
    skiff_ui_input_update(&input, SKIFF_UI_BUTTON_R);
    TEST_ASSERT_EQUAL_INT(
        SKIFF_UI_REPEAT_DELAY_FRAMES,
        frames_until_action(&input, SKIFF_UI_BUTTON_R, SKIFF_UI_ACTION_PAGE_DOWN, 100));
}

static void test_buttons_that_are_not_moves_never_repeat(void) {
    skiff_ui_input input;
    skiff_ui_input_init(&input, 1);
    TEST_ASSERT_EQUAL_HEX(SKIFF_UI_ACTION_CONFIRM,
                          skiff_ui_input_update(&input, SKIFF_UI_BUTTON_CROSS));
    TEST_ASSERT_EQUAL_INT(
        -1, frames_until_action(&input, SKIFF_UI_BUTTON_CROSS, SKIFF_UI_ACTION_CONFIRM, 200));
}

static void test_changing_direction_restarts_the_delay(void) {
    skiff_ui_input input;
    skiff_ui_input_init(&input, 1);
    skiff_ui_input_update(&input, SKIFF_UI_BUTTON_DOWN);
    for (int frame = 0; frame < SKIFF_UI_REPEAT_DELAY_FRAMES - 2; frame++) {
        skiff_ui_input_update(&input, SKIFF_UI_BUTTON_DOWN);
    }
    TEST_ASSERT_EQUAL_HEX(SKIFF_UI_ACTION_UP, skiff_ui_input_update(&input, SKIFF_UI_BUTTON_UP));
    TEST_ASSERT_EQUAL_INT(SKIFF_UI_REPEAT_DELAY_FRAMES,
                          frames_until_action(&input, SKIFF_UI_BUTTON_UP, SKIFF_UI_ACTION_UP, 100));
    TEST_PRINTF("pressing another button while holding a direction does not restart it");
    skiff_ui_input_init(&input, 1);
    skiff_ui_input_update(&input, SKIFF_UI_BUTTON_DOWN);
    for (int frame = 0; frame < SKIFF_UI_REPEAT_DELAY_FRAMES - 2; frame++) {
        skiff_ui_input_update(&input, SKIFF_UI_BUTTON_DOWN);
    }
    TEST_ASSERT_EQUAL_HEX(
        SKIFF_UI_ACTION_EXTRA,
        skiff_ui_input_update(&input, SKIFF_UI_BUTTON_DOWN | SKIFF_UI_BUTTON_SQUARE));
    TEST_ASSERT_EQUAL_HEX(
        SKIFF_UI_ACTION_DOWN,
        skiff_ui_input_update(&input, SKIFF_UI_BUTTON_DOWN | SKIFF_UI_BUTTON_SQUARE));
}

/* ---- A scrolling list ---- */

static void assert_list(const skiff_ui_list *list, size_t selected, size_t first) {
    TEST_PRINTF("selected %zu, first %zu (count %zu, rows %zu)", list->selected, list->first,
                list->count, list->rows);
    TEST_ASSERT_EQUAL_size_t(selected, list->selected);
    TEST_ASSERT_EQUAL_size_t(first, list->first);
}

static void test_up_and_down_wrap_and_scroll(void) {
    skiff_ui_list list;
    skiff_ui_list_init(&list, 30, 10);
    TEST_ASSERT_TRUE(skiff_ui_list_apply(&list, SKIFF_UI_ACTION_UP));
    assert_list(&list, 29, 20);
    TEST_ASSERT_TRUE(skiff_ui_list_apply(&list, SKIFF_UI_ACTION_DOWN));
    assert_list(&list, 0, 0);
    for (int i = 0; i < 10; i++) {
        skiff_ui_list_apply(&list, SKIFF_UI_ACTION_DOWN);
    }
    assert_list(&list, 10, 1);
    skiff_ui_list_apply(&list, SKIFF_UI_ACTION_UP);
    assert_list(&list, 9, 1);
}

static void test_pages_stop_at_the_ends(void) {
    skiff_ui_list list;
    skiff_ui_list_init(&list, 25, 10);
    skiff_ui_list_apply(&list, SKIFF_UI_ACTION_PAGE_DOWN);
    assert_list(&list, 10, 1);
    skiff_ui_list_apply(&list, SKIFF_UI_ACTION_PAGE_DOWN);
    assert_list(&list, 20, 11);
    skiff_ui_list_apply(&list, SKIFF_UI_ACTION_PAGE_DOWN);
    assert_list(&list, 24, 15);
    TEST_ASSERT_FALSE(skiff_ui_list_apply(&list, SKIFF_UI_ACTION_PAGE_DOWN));
    skiff_ui_list_apply(&list, SKIFF_UI_ACTION_PAGE_UP);
    assert_list(&list, 14, 14);
    skiff_ui_list_apply(&list, SKIFF_UI_ACTION_PAGE_UP);
    skiff_ui_list_apply(&list, SKIFF_UI_ACTION_PAGE_UP);
    assert_list(&list, 0, 0);
    TEST_ASSERT_FALSE(skiff_ui_list_apply(&list, SKIFF_UI_ACTION_PAGE_UP));
}

static void test_a_list_shorter_than_the_screen_never_scrolls(void) {
    skiff_ui_list list;
    skiff_ui_list_init(&list, 3, 10);
    skiff_ui_list_apply(&list, SKIFF_UI_ACTION_PAGE_DOWN);
    assert_list(&list, 2, 0);
    skiff_ui_list_apply(&list, SKIFF_UI_ACTION_DOWN);
    assert_list(&list, 0, 0);
}

static void test_a_list_that_shrinks_keeps_the_selection_on_screen(void) {
    skiff_ui_list list;
    skiff_ui_list_init(&list, 50, 10);
    skiff_ui_list_select(&list, 45);
    assert_list(&list, 45, 36);
    skiff_ui_list_set_count(&list, 20);
    assert_list(&list, 19, 10);
    skiff_ui_list_set_count(&list, 0);
    assert_list(&list, 0, 0);
    TEST_ASSERT_FALSE(skiff_ui_list_apply(&list, SKIFF_UI_ACTION_DOWN));
    skiff_ui_list_set_count(&list, 5);
    skiff_ui_list_select(&list, 99);
    assert_list(&list, 4, 0);
}

static void test_a_list_needs_a_row_and_ignores_null(void) {
    skiff_ui_list list;
    skiff_ui_list_init(&list, 5, 0);
    TEST_ASSERT_EQUAL_size_t(1, list.rows);
    skiff_ui_list_apply(&list, SKIFF_UI_ACTION_DOWN);
    assert_list(&list, 1, 1);
    skiff_ui_list_init(NULL, 1, 1);
    skiff_ui_list_set_count(NULL, 1);
    skiff_ui_list_select(NULL, 1);
    TEST_ASSERT_FALSE(skiff_ui_list_apply(NULL, SKIFF_UI_ACTION_DOWN));
}

/* ---- Text cut to a width ---- */

/* One unit per UTF-8 character, as a fixed-width font draws; counts calls. */
typedef struct measure_counter {
    int calls;
} measure_counter;

static float measure_characters(void *ctx, const char *text) {
    measure_counter *counter = ctx;
    if (counter != NULL) {
        counter->calls++;
    }
    float width = 0.0f;
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; c++) {
        if ((*c & 0xC0U) != 0x80U) {
            width += 1.0f;
        }
    }
    return width;
}

static void fit(const char *text, float width, size_t out_size, const char *expected) {
    char out[64];
    measure_counter counter = {0};
    TEST_ASSERT_TRUE(out_size <= sizeof out);
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_ui_fit_text(text, width, measure_characters, &counter, out, out_size));
    TEST_PRINTF("\"%s\" in %.0f (buffer %zu): \"%s\" after %d measurements", text, (double)width,
                out_size, out, counter.calls);
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

static void test_text_that_fits_is_left_alone(void) {
    fit("Lumines", 7.0f, 64, "Lumines");
    fit("", 0.0f, 64, "");
}

static void test_text_too_wide_ends_in_an_ellipsis(void) {
    fit("Lumines II", 9.0f, 64, "Lumine...");
    TEST_PRINTF("blanks before the ellipsis are dropped");
    fit("Ape Escape", 7.0f, 64, "Ape...");
    fit("Ape Escape", 8.0f, 64, "Ape E...");
    TEST_PRINTF("a cut never splits a character: ñ and é are two bytes each");
    fit("Señor café", 7.0f, 64, "Seño...");
    fit("ñññññ", 4.0f, 64, "ñ...");
}

static void test_too_narrow_for_the_ellipsis_leaves_nothing(void) {
    fit("Lumines", 2.0f, 64, "");
    fit("Lumines", 3.0f, 64, "...");
}

static void test_the_buffer_limits_the_text_too(void) {
    fit("Daxter", 100.0f, 7, "Daxter");
    fit("Daxter!", 100.0f, 7, "Dax...");
    fit("ñññññ", 100.0f, 8, "ññ...");
    fit("ñññññ", 100.0f, 7, "ñ...");
}

static void test_fitting_measures_a_few_times_not_once_per_character(void) {
    char text[57];
    memset(text, 'x', sizeof text - 1);
    text[sizeof text - 1] = '\0';
    char out[64];
    measure_counter counter = {0};
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_ui_fit_text(text, 20.0f, measure_characters, &counter, out, sizeof out));
    TEST_PRINTF("56 characters into 20: %d measurements", counter.calls);
    TEST_ASSERT_EQUAL_size_t(20, strlen(out));
    TEST_ASSERT_TRUE(counter.calls <= 10);
}

static void test_fitting_refuses_bad_arguments(void) {
    char out[8] = "xyz";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_ui_fit_text(NULL, 1.0f, measure_characters, NULL, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_ui_fit_text("a", 1.0f, NULL, NULL, out, sizeof out));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_ui_fit_text("a", 1.0f, measure_characters, NULL, NULL, 8));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_ui_fit_text("a", 1.0f, measure_characters, NULL, out, 3));
}

/* ---- UTF-16, the system dialogs' text ---- */

#define UTF16_BUFFER_UNITS 32
#define UTF8_BUFFER_BYTES 64

/* Converts text to UTF-16 and checks the units, then back, checking the UTF-8 it returns. */
static void check_utf16(const char *text, const uint16_t *expected, size_t expected_length,
                        const char *back) {
    uint16_t units[UTF16_BUFFER_UNITS];
    size_t length = 99;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_ui_utf8_to_utf16(text, units, UTF16_BUFFER_UNITS, &length));
    TEST_PRINTF("\"%s\": %zu UTF-16 units", text, length);
    TEST_ASSERT_EQUAL_size_t(expected_length, length);
    if (expected_length > 0) {
        TEST_ASSERT_EQUAL_HEX16_ARRAY(expected, units, expected_length);
    }
    TEST_ASSERT_EQUAL_HEX16(0, units[length]);
    char utf8[UTF8_BUFFER_BYTES];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_ui_utf16_to_utf8(units, UTF16_BUFFER_UNITS, utf8, sizeof utf8));
    TEST_PRINTF("back to UTF-8: \"%s\"", utf8);
    TEST_ASSERT_EQUAL_STRING(back, utf8);
}

static void test_utf8_and_utf16_round_trip(void) {
    TEST_PRINTF("ASCII, two-byte (é, ñ) and three-byte (日本) characters are one unit each");
    const uint16_t latin[] = {'C', 'a', 'f', 0x00E9, ' ', 0x00F1};
    check_utf16("Caf\xC3\xA9 \xC3\xB1", latin, 6, "Caf\xC3\xA9 \xC3\xB1");
    const uint16_t japanese[] = {0x65E5, 0x672C};
    check_utf16("\xE6\x97\xA5\xE6\x9C\xAC", japanese, 2, "\xE6\x97\xA5\xE6\x9C\xAC");
    TEST_PRINTF("a character above U+FFFF (U+1F3AE) becomes a surrogate pair and comes back");
    const uint16_t pair[] = {'a', 0xD83C, 0xDFAE, 'b'};
    check_utf16("a\xF0\x9F\x8E\xAE"
                "b",
                pair, 4,
                "a\xF0\x9F\x8E\xAE"
                "b");
    TEST_PRINTF("the edges of each length: U+0080, U+07FF, U+0800, U+FFFF, U+10000, U+10FFFF");
    const uint16_t edges[] = {0x0080, 0x07FF, 0x0800, 0xFFFF, 0xD800, 0xDC00, 0xDBFF, 0xDFFF};
    const char *edges_utf8 = "\xC2\x80\xDF\xBF\xE0\xA0\x80\xEF\xBF\xBF\xF0\x90\x80\x80"
                             "\xF4\x8F\xBF\xBF";
    check_utf16(edges_utf8, edges, 8, edges_utf8);
    check_utf16("", NULL, 0, "");
}

static void test_invalid_utf8_becomes_the_replacement_character(void) {
    TEST_PRINTF("a stray continuation byte and a byte no UTF-8 uses");
    const uint16_t stray[] = {'a', 0xFFFD, 'b', 0xFFFD, 'c'};
    check_utf16("a\x80"
                "b\xFF"
                "c",
                stray, 5,
                "a\xEF\xBF\xBD"
                "b\xEF\xBF\xBD"
                "c");
    TEST_PRINTF("a cut sequence is one replacement, and the byte that broke it is kept");
    const uint16_t cut[] = {0xFFFD, 'x', 0xFFFD};
    check_utf16("\xE6\x97"
                "x\xF0\x9F\x8E",
                cut, 3,
                "\xEF\xBF\xBD"
                "x\xEF\xBF\xBD");
    TEST_PRINTF("overlong forms, an encoded surrogate and a code point above U+10FFFF");
    const uint16_t overlong[] = {0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD,
                                 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD};
    /* C0 AF: two bad bytes; E0 80 AF: E0 then two stray continuations; ED A0 80 (U+D800): ED,
     * then two strays; F4 90 80 80 (U+110000): F4, then three strays. */
    check_utf16("\xC0\xAF\xE0\x80\xAF\xED\xA0\x80\xF4\x90\x80\x80", overlong, 12,
                "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
                "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
}

static void test_a_lone_surrogate_becomes_the_replacement_character(void) {
    char utf8[UTF8_BUFFER_BYTES];
    const uint16_t lone_high[] = {'a', 0xD83C, 'b', 0};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_utf16_to_utf8(lone_high, 4, utf8, sizeof utf8));
    TEST_PRINTF("high surrogate then 'b': \"%s\"", utf8);
    TEST_ASSERT_EQUAL_STRING("a\xEF\xBF\xBD"
                             "b",
                             utf8);
    const uint16_t lone_low[] = {0xDFAE, 0xDFAE, 0};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_utf16_to_utf8(lone_low, 3, utf8, sizeof utf8));
    TEST_ASSERT_EQUAL_STRING("\xEF\xBF\xBD\xEF\xBF\xBD", utf8);
    TEST_PRINTF("a high surrogate cut off by max_units is lone too, and nothing past it is read");
    const uint16_t cut_pair[] = {'a', 0xD83C, 0xDFAE};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_utf16_to_utf8(cut_pair, 2, utf8, sizeof utf8));
    TEST_ASSERT_EQUAL_STRING("a\xEF\xBF\xBD", utf8);
}

static void test_utf16_stops_at_max_units_or_the_first_zero(void) {
    char utf8[UTF8_BUFFER_BYTES];
    const uint16_t unterminated[] = {'P', 'S', 'P'};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_utf16_to_utf8(unterminated, 3, utf8, sizeof utf8));
    TEST_ASSERT_EQUAL_STRING("PSP", utf8);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_utf16_to_utf8(unterminated, 0, utf8, sizeof utf8));
    TEST_ASSERT_EQUAL_STRING("", utf8);
    const uint16_t early_end[] = {'G', 'o', 0, 'X'};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_utf16_to_utf8(early_end, 4, utf8, sizeof utf8));
    TEST_ASSERT_EQUAL_STRING("Go", utf8);
}

static void test_utf16_conversion_refuses_bad_arguments_and_small_buffers(void) {
    uint16_t units[4] = {'x', 'x', 'x', 'x'};
    size_t length = 99;
    TEST_PRINTF("\"abcd\" needs 5 units with the end: 4 is too few, and out is left empty");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_ui_utf8_to_utf16("abcd", units, 4, &length));
    TEST_ASSERT_EQUAL_HEX16(0, units[0]);
    TEST_ASSERT_EQUAL_size_t(0, length);
    TEST_PRINTF("a pair never goes in half: \"ab\" + U+1F3AE needs 5 units");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_ui_utf8_to_utf16("ab\xF0\x9F\x8E\xAE", units, 4, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_utf8_to_utf16("abc", units, 4, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_ui_utf8_to_utf16(NULL, units, 4, &length));
    TEST_ASSERT_EQUAL_HEX16(0, units[0]);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_ui_utf8_to_utf16("a", NULL, 4, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_ui_utf8_to_utf16("a", units, 0, NULL));

    char utf8[4] = "xyz";
    const uint16_t accented[] = {'a', 'b', 0x00E9, 0};
    TEST_PRINTF("\"ab\" + é needs 5 bytes with the end: 4 is too few, and out is left empty");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_ui_utf16_to_utf8(accented, 4, utf8, sizeof utf8));
    TEST_ASSERT_EQUAL_STRING("", utf8);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_ui_utf16_to_utf8(NULL, 0, utf8, 4));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_ui_utf16_to_utf8(accented, 4, NULL, 4));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_ui_utf16_to_utf8(accented, 4, utf8, 0));
}

static void test_the_utf8_size_bound_holds_for_the_worst_case(void) {
    TEST_PRINTF("8 units of U+FFFF (3 bytes each) fit in 8 * 3 + 1 bytes exactly");
    uint16_t units[8];
    for (size_t i = 0; i < 8; i++) {
        units[i] = 0xFFFF;
    }
    char utf8[8 * SKIFF_UI_UTF8_BYTES_PER_UTF16_UNIT + 1];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_utf16_to_utf8(units, 8, utf8, sizeof utf8));
    TEST_ASSERT_EQUAL_size_t(sizeof utf8 - 1, strlen(utf8));
}

/* ---- Progress ---- */

static void test_percent_rounds_down_and_reaches_100_only_at_the_end(void) {
    skiff_ui_progress progress;
    skiff_ui_progress_start(&progress, 0, 1000, 0);
    TEST_ASSERT_EQUAL_UINT(0, skiff_ui_progress_percent(&progress));
    skiff_ui_progress_update(&progress, 999, 10);
    TEST_ASSERT_EQUAL_UINT(99, skiff_ui_progress_percent(&progress));
    skiff_ui_progress_update(&progress, 1000, 20);
    TEST_ASSERT_EQUAL_UINT(100, skiff_ui_progress_percent(&progress));
    skiff_ui_progress_start(&progress, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT(0, skiff_ui_progress_percent(&progress));
    TEST_ASSERT_EQUAL_UINT(0, skiff_ui_progress_percent(NULL));
    TEST_PRINTF("a 4 GB file does not overflow");
    skiff_ui_progress_start(&progress, 4 * GIB - 1, 4 * GIB, 0);
    TEST_ASSERT_EQUAL_UINT(99, skiff_ui_progress_percent(&progress));
}

static void test_rate_and_time_left_come_after_a_window(void) {
    skiff_ui_progress progress;
    uint64_t seconds = 0;
    skiff_ui_progress_start(&progress, 10 * MIB, 64 * MIB, 1000);
    skiff_ui_progress_update(&progress, 10 * MIB + 400ULL * 1024, 2000);
    TEST_ASSERT_FALSE(skiff_ui_progress_eta(&progress, &seconds));
    skiff_ui_progress_update(&progress, 10 * MIB + 800ULL * 1024, 3000);
    TEST_ASSERT_TRUE(skiff_ui_progress_eta(&progress, &seconds));
    TEST_PRINTF("rate %llu B/s, %llu s left", (unsigned long long)progress.rate,
                (unsigned long long)seconds);
    TEST_ASSERT_EQUAL_UINT64(400ULL * 1024, progress.rate);
    TEST_ASSERT_EQUAL_UINT64((54 * MIB - 800ULL * 1024 + 400ULL * 1024 - 1) / (400ULL * 1024),
                             seconds);
    TEST_PRINTF("a slower window moves the rate a quarter of the way");
    skiff_ui_progress_update(&progress, 10 * MIB + 1000ULL * 1024, 5000);
    TEST_ASSERT_EQUAL_UINT64((400ULL * 1024 * 3 + 100ULL * 1024) / 4, progress.rate);
}

static void test_a_restart_starts_the_rate_again(void) {
    skiff_ui_progress progress;
    uint64_t seconds = 0;
    skiff_ui_progress_start(&progress, 0, 64 * MIB, 0);
    skiff_ui_progress_update(&progress, 2 * MIB, 4000);
    TEST_ASSERT_TRUE(progress.has_rate);
    skiff_ui_progress_update(&progress, 0, 5000);
    TEST_ASSERT_FALSE(progress.has_rate);
    TEST_ASSERT_FALSE(skiff_ui_progress_eta(&progress, &seconds));
    TEST_ASSERT_EQUAL_UINT64(64 * MIB, progress.total);
    TEST_PRINTF("a clock that goes back starts the window again too");
    skiff_ui_progress_update(&progress, 1 * MIB, 4000);
    TEST_ASSERT_EQUAL_UINT64(4000, progress.window_start_ms);
    skiff_ui_progress_start(NULL, 0, 0, 0);
    skiff_ui_progress_update(NULL, 0, 0);
    TEST_ASSERT_FALSE(skiff_ui_progress_eta(NULL, &seconds));
}

static void test_an_unknown_size_has_no_time_left(void) {
    skiff_ui_progress progress;
    uint64_t seconds = 7;
    skiff_ui_progress_start(&progress, 0, 0, 0);
    skiff_ui_progress_update(&progress, MIB, 3000);
    TEST_ASSERT_TRUE(progress.has_rate);
    TEST_ASSERT_FALSE(skiff_ui_progress_eta(&progress, &seconds));
    TEST_ASSERT_EQUAL_UINT64(7, seconds);
}

static void test_a_stalled_download_has_no_time_left(void) {
    skiff_ui_progress progress;
    uint64_t seconds = 7;
    skiff_ui_progress_start(&progress, 0, MIB, 0);
    skiff_ui_progress_update(&progress, 0, 3000);
    TEST_ASSERT_TRUE(progress.has_rate);
    TEST_ASSERT_FALSE(skiff_ui_progress_eta(&progress, &seconds));
    TEST_ASSERT_EQUAL_UINT64(7, seconds);
}

/* ---- Sizes and durations ---- */

static void assert_bytes(uint64_t bytes, const char *separator, const char *expected) {
    char out[32];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_format_bytes(bytes, separator, out, sizeof out));
    TEST_PRINTF("%llu bytes -> \"%s\"", (unsigned long long)bytes, out);
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

static void test_sizes_read_as_the_xmb_shows_them(void) {
    assert_bytes(0, ".", "0 B");
    assert_bytes(1023, ".", "1023 B");
    assert_bytes(1024, ".", "1 KB");
    assert_bytes(MIB - 1, ".", "1023 KB");
    assert_bytes(MIB + MIB / 2, ".", "1.5 MB");
    assert_bytes(MIB + MIB / 2, ",", "1,5 MB");
    assert_bytes(100 * MIB - 1, ".", "99.9 MB");
    assert_bytes(640 * MIB, ".", "640 MB");
    TEST_PRINTF("sizes round down: 1.25 GB shows as 1.2");
    assert_bytes(GIB + GIB / 4, ",", "1,2 GB");
    assert_bytes(4 * GIB - 1, ".", "3.9 GB");
}

static void test_durations_read_as_a_player_says_them(void) {
    const struct {
        uint64_t seconds;
        const char *expected;
    } cases[] = {{0, "0 s"},           {59, "59 s"},         {60, "1 min"},
                 {61, "2 min"},        {3540, "59 min"},     {3541, "1 h 00 min"},
                 {3600, "1 h 00 min"}, {7500, "2 h 05 min"}, {36000, "10 h 00 min"}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char out[32];
        TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                              skiff_ui_format_duration(cases[i].seconds, out, sizeof out));
        TEST_PRINTF("%llu s -> \"%s\"", (unsigned long long)cases[i].seconds, out);
        TEST_ASSERT_EQUAL_STRING(cases[i].expected, out);
    }
}

static void test_formatting_refuses_bad_arguments_and_small_buffers(void) {
    char out[4] = "xyz";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_ui_format_bytes(640 * MIB, ".", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_ui_format_duration(7500, out, sizeof out));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_ui_format_bytes(1, NULL, out, sizeof out));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_ui_format_bytes(1, ".", NULL, 4));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_ui_format_duration(1, out, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_confirm_and_back_follow_the_console_setting);
    RUN_TEST(test_every_button_has_its_action);
    RUN_TEST(test_a_held_direction_repeats_after_a_delay);
    RUN_TEST(test_buttons_that_are_not_moves_never_repeat);
    RUN_TEST(test_changing_direction_restarts_the_delay);
    RUN_TEST(test_up_and_down_wrap_and_scroll);
    RUN_TEST(test_pages_stop_at_the_ends);
    RUN_TEST(test_a_list_shorter_than_the_screen_never_scrolls);
    RUN_TEST(test_a_list_that_shrinks_keeps_the_selection_on_screen);
    RUN_TEST(test_a_list_needs_a_row_and_ignores_null);
    RUN_TEST(test_text_that_fits_is_left_alone);
    RUN_TEST(test_text_too_wide_ends_in_an_ellipsis);
    RUN_TEST(test_too_narrow_for_the_ellipsis_leaves_nothing);
    RUN_TEST(test_the_buffer_limits_the_text_too);
    RUN_TEST(test_fitting_measures_a_few_times_not_once_per_character);
    RUN_TEST(test_fitting_refuses_bad_arguments);
    RUN_TEST(test_utf8_and_utf16_round_trip);
    RUN_TEST(test_invalid_utf8_becomes_the_replacement_character);
    RUN_TEST(test_a_lone_surrogate_becomes_the_replacement_character);
    RUN_TEST(test_utf16_stops_at_max_units_or_the_first_zero);
    RUN_TEST(test_utf16_conversion_refuses_bad_arguments_and_small_buffers);
    RUN_TEST(test_the_utf8_size_bound_holds_for_the_worst_case);
    RUN_TEST(test_percent_rounds_down_and_reaches_100_only_at_the_end);
    RUN_TEST(test_rate_and_time_left_come_after_a_window);
    RUN_TEST(test_a_restart_starts_the_rate_again);
    RUN_TEST(test_an_unknown_size_has_no_time_left);
    RUN_TEST(test_a_stalled_download_has_no_time_left);
    RUN_TEST(test_sizes_read_as_the_xmb_shows_them);
    RUN_TEST(test_durations_read_as_a_player_says_them);
    RUN_TEST(test_formatting_refuses_bad_arguments_and_small_buffers);
    return UNITY_END();
}
