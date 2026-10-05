#include <stdio.h>
#include <string.h>

#include "probe_support.h"
#include "unity.h"

/* CRC-32 check value from the catalogue of parametrised CRC algorithms (CRC-32/ISO-HDLC). */
#define CRC32_CHECK_INPUT "123456789"
#define CRC32_CHECK_VALUE 0xCBF43926U

enum { PATTERN_BYTES = 4096, CHUNK_BYTES = 1000, NARRATION_MAX = 256 };

static skiff_probe_config config;

void setUp(void) { skiff_probe_config_defaults(&config); }

void tearDown(void) {}

/* Unity's TEST_PRINTF knows only a few conversions, so lines are formatted with snprintf first. */
#define NARRATE(...)                                                                               \
    do {                                                                                           \
        char narration[NARRATION_MAX];                                                             \
        snprintf(narration, sizeof narration, __VA_ARGS__);                                        \
        TEST_PRINTF("%s", narration);                                                              \
    } while (0)

/* Feeds text to skiff_probe_config_read() through a temporary file, as a probe reads its .ini. */
static void read_text(const char *text) {
    FILE *file = tmpfile();
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fputs(text, file) < 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_SET));
    TEST_PRINTF("config text:\n%s", text);
    skiff_probe_config_read(file, &config);
    fclose(file);
    NARRATE("-> host '%s' profile %d plain_http %d size %llu crc32 %08lx (has %d) runs %d "
            "sections 0x%x invalid %d",
            config.host, config.profile, config.plain_http, config.size,
            (unsigned long)config.crc32, config.has_crc32, config.runs, config.sections,
            config.invalid_values);
}

static void test_defaults_select_every_section_and_three_runs(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_PROBE_DEFAULT_PROFILE, config.profile);
    TEST_ASSERT_EQUAL_INT(SKIFF_PROBE_DEFAULT_RUNS, config.runs);
    TEST_ASSERT_EQUAL_UINT(SKIFF_PROBE_SECTION_ALL, config.sections);
    TEST_ASSERT_FALSE(skiff_probe_config_complete(&config));
}

static void test_reads_what_memstick_install_writes(void) {
    read_text("# Written by scripts/memstick.sh install\n"
              "host=192.168.5.20\n"
              "profile=2\n"
              "plain_http=1\n"
              "token=rmm_secret\n"
              "rom_id=7\n"
              "file_name=Skiff%20Test%20Payload.iso\n"
              "size=4194304\n"
              "crc32=0a1b2c3d\n");
    TEST_ASSERT_EQUAL_STRING("192.168.5.20", config.host);
    TEST_ASSERT_EQUAL_INT(2, config.profile);
    TEST_ASSERT_EQUAL_INT(1, config.plain_http);
    TEST_ASSERT_EQUAL_STRING("rmm_secret", config.token);
    TEST_ASSERT_EQUAL_STRING("7", config.rom_id);
    TEST_ASSERT_EQUAL_STRING("Skiff%20Test%20Payload.iso", config.file_name);
    TEST_ASSERT_EQUAL_UINT64(4194304ULL, config.size);
    TEST_ASSERT_EQUAL_HEX32(0x0a1b2c3dU, config.crc32);
    TEST_ASSERT_TRUE(config.has_crc32);
    TEST_ASSERT_EQUAL_INT(0, config.invalid_values);
    TEST_ASSERT_TRUE(skiff_probe_config_complete(&config));
}

static void test_ignores_comments_blank_lines_crlf_and_unknown_keys(void) {
    read_text("# comment=with an equals sign\r\n"
              "\r\n"
              "host=10.0.0.1  \r\n"
              "colour=blue\n");
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", config.host);
    TEST_ASSERT_EQUAL_INT(0, config.invalid_values);
}

static void test_reads_runs_and_sections(void) {
    read_text("runs=5\nsections=latency, net,ms\n");
    TEST_ASSERT_EQUAL_INT(5, config.runs);
    TEST_ASSERT_EQUAL_UINT(SKIFF_PROBE_SECTION_LATENCY | SKIFF_PROBE_SECTION_NET |
                               SKIFF_PROBE_SECTION_MS,
                           config.sections);
    TEST_ASSERT_EQUAL_INT(0, config.invalid_values);
}

static void test_all_selects_every_section(void) {
    read_text("sections=cpu\nsections=all\n");
    TEST_ASSERT_EQUAL_UINT(SKIFF_PROBE_SECTION_ALL, config.sections);
}

static void test_plain_http_takes_only_0_or_1(void) {
    read_text("plain_http=1\nplain_http=yes\nplain_http=\n");
    TEST_ASSERT_EQUAL_INT(1, config.plain_http);
    TEST_ASSERT_EQUAL_INT(2, config.invalid_values);
    read_text("plain_http=0\n");
    TEST_ASSERT_EQUAL_INT(0, config.plain_http);
}

static void test_reads_the_clock_and_its_section(void) {
    TEST_ASSERT_EQUAL_INT(0, config.clock_mhz);
    read_text("clock_mhz=333\nsections=clock\n");
    TEST_ASSERT_EQUAL_INT(SKIFF_PROBE_CLOCK_FAST_MHZ, config.clock_mhz);
    TEST_ASSERT_EQUAL_UINT(SKIFF_PROBE_SECTION_CLOCK, config.sections);
    read_text("clock_mhz=222\n");
    TEST_ASSERT_EQUAL_INT(SKIFF_PROBE_CLOCK_DEFAULT_MHZ, config.clock_mhz);
    TEST_ASSERT_EQUAL_INT(0, config.invalid_values);
}

static void test_a_clock_other_than_222_or_333_is_invalid(void) {
    read_text("clock_mhz=300\nclock_mhz=100\nclock_mhz=fast\n");
    TEST_ASSERT_EQUAL_INT(0, config.clock_mhz);
    TEST_ASSERT_EQUAL_INT(3, config.invalid_values);
}

static void test_unusable_values_keep_the_default_and_are_counted(void) {
    read_text("runs=0\nruns=10\nruns=two\nprofile=0\nprofile=1x\nsize=-1\nsize=\n"
              "crc32=xyz\nsections=net,disk\nsections=\n");
    TEST_ASSERT_EQUAL_INT(SKIFF_PROBE_DEFAULT_RUNS, config.runs);
    TEST_ASSERT_EQUAL_INT(SKIFF_PROBE_DEFAULT_PROFILE, config.profile);
    TEST_ASSERT_EQUAL_UINT64(0ULL, config.size);
    TEST_ASSERT_FALSE(config.has_crc32);
    TEST_ASSERT_EQUAL_UINT(SKIFF_PROBE_SECTION_ALL, config.sections);
    TEST_ASSERT_EQUAL_INT(10, config.invalid_values);
}

static void test_an_empty_crc32_is_absent_not_invalid(void) {
    read_text("crc32=\n");
    TEST_ASSERT_FALSE(config.has_crc32);
    TEST_ASSERT_EQUAL_INT(0, config.invalid_values);
}

static void test_a_crc32_wider_than_32_bits_is_invalid(void) {
    read_text("crc32=1ffffffff\n");
    TEST_ASSERT_FALSE(config.has_crc32);
    TEST_ASSERT_EQUAL_INT(1, config.invalid_values);
}

static void test_a_value_too_long_for_its_field_is_invalid(void) {
    char text[SKIFF_PROBE_LINE_MAX];
    char host[SKIFF_PROBE_HOST_MAX + 1];
    memset(host, 'h', sizeof host - 1);
    host[sizeof host - 1] = '\0';
    snprintf(text, sizeof text, "host=%s\n", host);
    read_text(text);
    TEST_ASSERT_EQUAL_INT(1, config.invalid_values);
}

static void test_complete_needs_every_download_field(void) {
    read_text("host=h\ntoken=t\nrom_id=1\nfile_name=f\nsize=10\ncrc32=1\n");
    TEST_ASSERT_TRUE(skiff_probe_config_complete(&config));
    const skiff_probe_config full = config;
    config.size = 0;
    TEST_ASSERT_FALSE(skiff_probe_config_complete(&config));
    config = full;
    config.has_crc32 = 0;
    TEST_ASSERT_FALSE(skiff_probe_config_complete(&config));
    config = full;
    config.token[0] = '\0';
    TEST_ASSERT_FALSE(skiff_probe_config_complete(&config));
    config = full;
    config.host[0] = '\0';
    TEST_ASSERT_FALSE(skiff_probe_config_complete(&config));
}

static void test_median_of_an_odd_count_sorts_and_takes_the_middle(void) {
    long long values[] = {30, 10, 50, 20, 40};
    const long long median = skiff_probe_median(values, 5);
    NARRATE("median %lld, min %lld, max %lld", median, values[0], values[4]);
    TEST_ASSERT_EQUAL_INT64(30, median);
    TEST_ASSERT_EQUAL_INT64(10, values[0]);
    TEST_ASSERT_EQUAL_INT64(50, values[4]);
}

static void test_median_of_an_even_count_is_the_mean_of_the_middle_two(void) {
    long long values[] = {40, 10, 20, 30};
    TEST_ASSERT_EQUAL_INT64(25, skiff_probe_median(values, 4));
}

static void test_median_of_one_and_of_none(void) {
    long long one[] = {7};
    TEST_ASSERT_EQUAL_INT64(7, skiff_probe_median(one, 1));
    TEST_ASSERT_EQUAL_INT64(0, skiff_probe_median(one, 0));
}

static void test_rate_is_kilobytes_per_second(void) {
    TEST_ASSERT_EQUAL_UINT64(1024ULL, skiff_probe_kb_per_s(1024ULL * 1024ULL, 1000000LL));
    /* T1 on the PSP: 1 MiB in 5297 ms. */
    TEST_ASSERT_EQUAL_UINT64(193ULL, skiff_probe_kb_per_s(1048576ULL, 5297000LL));
    /* A 4 GB file must not overflow the intermediate product. */
    TEST_ASSERT_EQUAL_UINT64(4194304ULL, skiff_probe_kb_per_s(4294967296ULL, 1000000LL));
}

static void test_rate_without_elapsed_time_is_zero(void) {
    TEST_ASSERT_EQUAL_UINT64(0ULL, skiff_probe_kb_per_s(1000ULL, 0));
    TEST_ASSERT_EQUAL_UINT64(0ULL, skiff_probe_kb_per_s(1000ULL, -5));
}

static void test_both_crc32_versions_match_the_check_value(void) {
    const unsigned char *input = (const unsigned char *)CRC32_CHECK_INPUT;
    const size_t size = strlen(CRC32_CHECK_INPUT);
    const uint32_t bitwise = skiff_probe_crc32_bitwise(0, input, size);
    const uint32_t table = skiff_probe_crc32_table(0, input, size);
    NARRATE("CRC-32(\"%s\"): bitwise %08lx, table %08lx", CRC32_CHECK_INPUT, (unsigned long)bitwise,
            (unsigned long)table);
    TEST_ASSERT_EQUAL_HEX32(CRC32_CHECK_VALUE, bitwise);
    TEST_ASSERT_EQUAL_HEX32(CRC32_CHECK_VALUE, table);
}

static void test_crc32_of_nothing_is_zero(void) {
    TEST_ASSERT_EQUAL_HEX32(0, skiff_probe_crc32_bitwise(0, NULL, 0));
    TEST_ASSERT_EQUAL_HEX32(0, skiff_probe_crc32_table(0, NULL, 0));
}

static void test_crc32_in_chunks_equals_crc32_in_one_go(void) {
    unsigned char data[PATTERN_BYTES];
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (unsigned char)(i * 31U + 7U);
    }
    const uint32_t whole = skiff_probe_crc32_table(0, data, sizeof data);
    uint32_t bitwise = 0;
    uint32_t table = 0;
    for (size_t offset = 0; offset < sizeof data; offset += CHUNK_BYTES) {
        const size_t size = sizeof data - offset < CHUNK_BYTES ? sizeof data - offset : CHUNK_BYTES;
        bitwise = skiff_probe_crc32_bitwise(bitwise, data + offset, size);
        table = skiff_probe_crc32_table(table, data + offset, size);
    }
    NARRATE("whole %08lx, chunked bitwise %08lx, chunked table %08lx", (unsigned long)whole,
            (unsigned long)bitwise, (unsigned long)table);
    TEST_ASSERT_EQUAL_HEX32(whole, bitwise);
    TEST_ASSERT_EQUAL_HEX32(whole, table);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_select_every_section_and_three_runs);
    RUN_TEST(test_reads_what_memstick_install_writes);
    RUN_TEST(test_ignores_comments_blank_lines_crlf_and_unknown_keys);
    RUN_TEST(test_reads_runs_and_sections);
    RUN_TEST(test_all_selects_every_section);
    RUN_TEST(test_plain_http_takes_only_0_or_1);
    RUN_TEST(test_reads_the_clock_and_its_section);
    RUN_TEST(test_a_clock_other_than_222_or_333_is_invalid);
    RUN_TEST(test_unusable_values_keep_the_default_and_are_counted);
    RUN_TEST(test_an_empty_crc32_is_absent_not_invalid);
    RUN_TEST(test_a_crc32_wider_than_32_bits_is_invalid);
    RUN_TEST(test_a_value_too_long_for_its_field_is_invalid);
    RUN_TEST(test_complete_needs_every_download_field);
    RUN_TEST(test_median_of_an_odd_count_sorts_and_takes_the_middle);
    RUN_TEST(test_median_of_an_even_count_is_the_mean_of_the_middle_two);
    RUN_TEST(test_median_of_one_and_of_none);
    RUN_TEST(test_rate_is_kilobytes_per_second);
    RUN_TEST(test_rate_without_elapsed_time_is_zero);
    RUN_TEST(test_both_crc32_versions_match_the_check_value);
    RUN_TEST(test_crc32_of_nothing_is_zero);
    RUN_TEST(test_crc32_in_chunks_equals_crc32_in_one_go);
    return UNITY_END();
}
