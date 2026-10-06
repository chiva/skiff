/*
 * The .resume file (skiff/download.h): it is rewritten in place on a FAT Memory Stick, so a power
 * cut can leave it half-written. Anything but a complete file with a matching check line must be
 * refused, or a resumed download would continue from the wrong offset or CRC-32.
 */
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "skiff/download.h"

#include "unity.h"

#define ETAG "\"6ac37763-1000\""
#define ABOVE_4_GIB 4294967296ULL

static skiff_download_state state;
static skiff_download_state parsed;
static char text[SKIFF_DOWNLOAD_STATE_MAX];
static size_t length;

void setUp(void) {
    memset(&state, 0, sizeof state);
    memset(&parsed, 0xAB, sizeof parsed);
    state.size = 3 * ABOVE_4_GIB / 2;
    state.has_expected_crc32 = 1;
    state.expected_crc32 = 0xCBF43926U;
    snprintf(state.etag, sizeof state.etag, "%s", ETAG);
    state.offset = ABOVE_4_GIB + 1;
    state.crc32 = 0x0000ABCDU;
    length = 0;
}

void tearDown(void) {}

static void format(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_download_state_format(&state, text, sizeof text, &length));
    TEST_PRINTF("formatted (%u bytes):\n%s", (unsigned)length, text);
}

static void expect_refused(const char *edited, const char *why) {
    TEST_PRINTF("refused: %s", why);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_INVALID_ARG,
                                  skiff_download_state_parse(edited, strlen(edited), &parsed), why);
}

/* text with the first occurrence of from replaced by to (the check line left as it was). */
static const char *edited(const char *from, const char *to) {
    static char copy[2 * SKIFF_DOWNLOAD_STATE_MAX];
    const char *at = strstr(text, from);
    TEST_ASSERT_NOT_NULL_MESSAGE(at, from);
    snprintf(copy, sizeof copy, "%.*s%s%s", (int)(at - text), text, to, at + strlen(from));
    return copy;
}

static void test_round_trip_with_64_bit_values(void) {
    format();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_download_state_parse(text, length, &parsed));
    TEST_ASSERT_EQUAL_UINT64(state.size, parsed.size);
    TEST_ASSERT_EQUAL_UINT64(state.offset, parsed.offset);
    TEST_ASSERT_EQUAL_HEX32(state.crc32, parsed.crc32);
    TEST_ASSERT_TRUE(parsed.has_expected_crc32);
    TEST_ASSERT_EQUAL_HEX32(state.expected_crc32, parsed.expected_crc32);
    TEST_ASSERT_EQUAL_STRING(ETAG, parsed.etag);
    TEST_ASSERT_NOT_NULL(strstr(text, "offset=4294967297\n"));
    TEST_ASSERT_NOT_NULL(strstr(text, "crc32=0000abcd\n"));
}

static void test_round_trip_without_crc_or_etag(void) {
    state.has_expected_crc32 = 0;
    state.expected_crc32 = 0;
    state.etag[0] = '\0';
    state.offset = 0;
    format();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_download_state_parse(text, length, &parsed));
    TEST_ASSERT_FALSE(parsed.has_expected_crc32);
    TEST_ASSERT_EQUAL_STRING("", parsed.etag);
    TEST_ASSERT_EQUAL_UINT64(0, parsed.offset);
}

static void test_longest_state_fits(void) {
    memset(state.etag, 'e', sizeof state.etag - 1);
    state.etag[sizeof state.etag - 1] = '\0';
    state.size = UINT64_MAX;
    state.offset = UINT64_MAX;
    format();
    TEST_ASSERT_LESS_OR_EQUAL_size_t(SKIFF_DOWNLOAD_STATE_MAX, length);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_download_state_parse(text, length, &parsed));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, parsed.offset);
}

static void test_a_write_cut_short_is_refused_at_every_length(void) {
    format();
    for (size_t cut = 0; cut < length; cut++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_INVALID_ARG,
                                      skiff_download_state_parse(text, cut, &parsed),
                                      "a prefix of the file must never parse");
    }
    TEST_PRINTF("all %u shorter prefixes refused", (unsigned)length);
}

static void test_a_changed_value_breaks_the_check(void) {
    format();
    expect_refused(edited("offset=4294967297", "offset=4294967296"), "offset changed");
    expect_refused(edited("crc32=0000abcd", "crc32=0000abce"), "crc32 changed");
    expect_refused(edited(ETAG, "\"other\""), "ETag changed");
}

static void test_malformed_files_are_refused(void) {
    format();
    expect_refused(edited("check=", "check=x"), "check not hex");
    expect_refused(edited("\ncheck=", "\nchecks="), "no check line");
    expect_refused(edited("version=1\n", ""), "missing line");
    expect_refused(edited("version=1\n", "version=1\nversion=1\n"), "repeated line");
    expect_refused(edited("\n", "\nextra=1\n"), "unknown line");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_download_state_parse(text, 0, &parsed));
    char with_nul[SKIFF_DOWNLOAD_STATE_MAX];
    memcpy(with_nul, text, length);
    with_nul[3] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_download_state_parse(with_nul, length, &parsed));
    char trailing[2 * SKIFF_DOWNLOAD_STATE_MAX];
    snprintf(trailing, sizeof trailing, "%.*sjunk", (int)length, text);
    expect_refused(trailing, "bytes after the check line");
}

/* body plus a matching check line: values a buggy writer or a future version might produce get
 * past the check, so the parser must still judge them. */
static skiff_err parse_with_valid_check(const char *body) {
    static char file[2 * SKIFF_DOWNLOAD_STATE_MAX];
    const unsigned long check = crc32(0L, (const Bytef *)body, (uInt)strlen(body));
    snprintf(file, sizeof file, "%scheck=%08lx\n", body, check);
    return skiff_download_state_parse(file, strlen(file), &parsed);
}

static void expect_refused_with_valid_check(const char *body, const char *why) {
    TEST_PRINTF("refused: %s", why);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_INVALID_ARG, parse_with_valid_check(body), why);
}

static void test_values_are_checked_even_with_a_valid_check(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        SKIFF_OK,
        parse_with_valid_check(
            "version=1\nsize=10\ncrc32_expected=\netag=\"e\"\noffset=5\ncrc32=00000001\n"),
        "a well-formed body passes");
    TEST_ASSERT_EQUAL_UINT64(5, parsed.offset);
    expect_refused_with_valid_check(
        "version=1\nsize=10\ncrc32_expected=\netag=\nOffset=5\ncrc32=00000001\n", "key case");
    expect_refused_with_valid_check(
        "version=2\nsize=10\ncrc32_expected=\netag=\noffset=5\ncrc32=00000001\n",
        "unknown version");
    expect_refused_with_valid_check(
        "version=1\nsize=10\ncrc32_expected=\netag=\noffset=11\ncrc32=00000001\n",
        "offset past size");
    expect_refused_with_valid_check(
        "version=1\nsize=-1\ncrc32_expected=\netag=\noffset=0\ncrc32=00000001\n", "signed size");
    expect_refused_with_valid_check(
        "version=1\nsize=99999999999999999999\ncrc32_expected=\netag=\noffset=0\ncrc32=00000001\n",
        "size overflows 64 bits");
    expect_refused_with_valid_check(
        "version=1\nsize=10\ncrc32_expected=\netag=\noffset=5 \ncrc32=00000001\n",
        "trailing blank");
    expect_refused_with_valid_check(
        "version=1\nsize=10\ncrc32_expected=1234\netag=\noffset=5\ncrc32=00000001\n",
        "short expected CRC");
    expect_refused_with_valid_check(
        "version=1\nsize=10\ncrc32_expected=\netag=\noffset=5\ncrc32=0000000g\n", "CRC not hex");
}

static void test_format_refuses_what_it_cannot_write(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_download_state_format(NULL, text, sizeof text, &length));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_download_state_format(&state, NULL, sizeof text, &length));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_download_state_format(&state, text, sizeof text, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_download_state_format(&state, text, 0, &length));
    snprintf(state.etag, sizeof state.etag, "\"a\nb\"");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_download_state_format(&state, text, sizeof text, &length));
    snprintf(state.etag, sizeof state.etag, "%s", ETAG);
    state.offset = state.size + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_download_state_format(&state, text, sizeof text, &length));
    state.offset = 0;
    for (size_t size = 1; size < 40; size++) {
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                              skiff_download_state_format(&state, text, size, &length));
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_download_state_parse(NULL, 1, &parsed));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_download_state_parse(text, 1, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_download_state_parse(text, SKIFF_DOWNLOAD_STATE_MAX + 1, &parsed));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_with_64_bit_values);
    RUN_TEST(test_round_trip_without_crc_or_etag);
    RUN_TEST(test_longest_state_fits);
    RUN_TEST(test_a_write_cut_short_is_refused_at_every_length);
    RUN_TEST(test_a_changed_value_breaks_the_check);
    RUN_TEST(test_malformed_files_are_refused);
    RUN_TEST(test_values_are_checked_even_with_a_valid_check);
    RUN_TEST(test_format_refuses_what_it_cannot_write);
    return UNITY_END();
}
