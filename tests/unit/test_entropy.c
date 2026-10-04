#include <stdio.h>
#include <string.h>

#include "skiff/entropy.h"

#include "unity.h"

/* 32 words: one Mbed TLS request (MBEDTLS_ENTROPY_MAX_GATHER, 128 bytes). */
enum { SCRIPT_MAX_WORDS = 32, MBEDTLS_GATHER_BYTES = 128, OUT_MAX = 16, POISON = 0xAA };

/* A generator that replays a fixed list of words, then fails with SKIFF_ERR_STORAGE_IO. */
typedef struct scripted_generator {
    uint32_t words[SCRIPT_MAX_WORDS];
    size_t count;
    size_t reads;
    skiff_err fail_with;
    size_t fail_at;
} scripted_generator;

static scripted_generator generator;
static skiff_entropy_source source;
static unsigned char out[OUT_MAX];

static skiff_err read_scripted(void *ctx, uint32_t *word) {
    scripted_generator *script = ctx;
    if (script->fail_with != SKIFF_OK && script->reads == script->fail_at) {
        printf("  generator> read %zu fails with %s\n", script->reads,
               skiff_err_name(script->fail_with));
        return script->fail_with;
    }
    if (script->reads >= script->count) {
        printf("  generator> read %zu past the end of the script\n", script->reads);
        return SKIFF_ERR_STORAGE_IO;
    }
    *word = script->words[script->reads];
    printf("  generator> read %zu = 0x%08x\n", script->reads, (unsigned)*word);
    script->reads++;
    return SKIFF_OK;
}

static void load_script(const uint32_t *words, size_t count) {
    TEST_ASSERT_LESS_OR_EQUAL_size_t(SCRIPT_MAX_WORDS, count);
    memcpy(generator.words, words, count * sizeof words[0]);
    generator.count = count;
}

static void assert_zeroed(const unsigned char *buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, buffer[i], "output must be wiped on failure");
    }
}

void setUp(void) {
    memset(&generator, 0, sizeof generator);
    memset(out, POISON, sizeof out);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_entropy_source_init(&source, read_scripted, &generator));
}

void tearDown(void) {}

static void test_init_rejects_null_arguments(void) {
    skiff_entropy_source local;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_entropy_source_init(NULL, read_scripted, &generator));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_entropy_source_init(&local, NULL, NULL));
}

static void test_init_accepts_a_null_context(void) {
    skiff_entropy_source local;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_entropy_source_init(&local, read_scripted, NULL));
}

static void test_fills_whole_words_in_little_endian_order(void) {
    const uint32_t words[] = {0x04030201U, 0x08070605U};
    const unsigned char expected[] = {1, 2, 3, 4, 5, 6, 7, 8};
    load_script(words, 2);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_entropy_fill(&source, out, sizeof expected));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof expected);
    TEST_ASSERT_EQUAL_size_t(2, generator.reads);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(POISON, out[sizeof expected], "wrote past out_size");
}

static void test_partial_final_word_reads_one_more_word_and_writes_only_out_size(void) {
    const uint32_t words[] = {0x04030201U, 0x08070605U};
    const unsigned char expected[] = {1, 2, 3, 4, 5};
    load_script(words, 2);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_entropy_fill(&source, out, sizeof expected));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof expected);
    TEST_ASSERT_EQUAL_size_t(2, generator.reads);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(POISON, out[sizeof expected], "wrote past out_size");
}

static void test_zero_size_reads_nothing(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_entropy_fill(&source, out, 0));
    TEST_ASSERT_EQUAL_size_t(0, generator.reads);
    TEST_ASSERT_EQUAL_HEX8(POISON, out[0]);
}

static void test_fill_rejects_null_arguments(void) {
    skiff_entropy_source empty;
    memset(&empty, 0, sizeof empty);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_entropy_fill(NULL, out, 4));
    assert_zeroed(out, 4);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_entropy_fill(&source, NULL, 4));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_entropy_fill(&empty, out, 4));
    TEST_ASSERT_EQUAL_size_t(0, generator.reads);
}

static void test_consecutive_repeat_fails_and_wipes_output(void) {
    const uint32_t words[] = {0x11111111U, 0x22222222U, 0x22222222U};
    load_script(words, 3);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_ENTROPY, skiff_entropy_fill(&source, out, 12));
    assert_zeroed(out, 12);
}

static void test_repeat_across_fills_is_detected(void) {
    const uint32_t words[] = {0x11111111U, 0x22222222U, 0x22222222U};
    load_script(words, 3);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_entropy_fill(&source, out, 8));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_ENTROPY, skiff_entropy_fill(&source, out, 4));
    assert_zeroed(out, 4);
}

static void test_alternating_values_fail_the_proportion_test(void) {
    const uint32_t words[] = {0x11111111U, 0x22222222U, 0x11111111U, 0x22222222U};
    load_script(words, 4);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_ENTROPY, skiff_entropy_fill(&source, out, 16));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(3, generator.reads, "the window's first word came back");
    assert_zeroed(out, 16);
}

static void test_short_cycle_fails_the_proportion_test(void) {
    const uint32_t words[] = {0xA0A0A0A0U, 0xB1B1B1B1U, 0xC2C2C2C2U, 0xA0A0A0A0U};
    load_script(words, 4);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_ENTROPY, skiff_entropy_fill(&source, out, 16));
    TEST_ASSERT_EQUAL_size_t(4, generator.reads);
}

static void test_repeat_of_a_word_other_than_the_window_start_passes(void) {
    const uint32_t words[] = {0x11111111U, 0x22222222U, 0x33333333U, 0x22222222U};
    load_script(words, 4);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_entropy_fill(&source, out, 16));
}

/* Distinct words, except that word SKIFF_ENTROPY_WINDOW_WORDS (the first of the second window)
 * repeats word 0, the first of the first window. */
static size_t counter_reads;

static skiff_err read_counter(void *ctx, uint32_t *word) {
    (void)ctx;
    *word =
        counter_reads == SKIFF_ENTROPY_WINDOW_WORDS ? 0x1000U : 0x1000U + (uint32_t)counter_reads;
    counter_reads++;
    return SKIFF_OK;
}

static void test_window_start_may_recur_in_the_next_window(void) {
    enum { REQUEST_BYTES = MBEDTLS_GATHER_BYTES, WINDOW_BYTES = SKIFF_ENTROPY_WINDOW_WORDS * 4 };
    unsigned char request[REQUEST_BYTES];
    skiff_entropy_source counted;
    counter_reads = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_entropy_source_init(&counted, read_counter, NULL));
    for (size_t filled = 0; filled < WINDOW_BYTES + REQUEST_BYTES; filled += REQUEST_BYTES) {
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_entropy_fill(&counted, request, sizeof request));
    }
    TEST_PRINTF("read %zu words across two windows", counter_reads);
    TEST_ASSERT_GREATER_THAN_size_t(SKIFF_ENTROPY_WINDOW_WORDS, counter_reads);
}

static void test_health_failure_is_permanent(void) {
    const uint32_t words[] = {0x33333333U, 0x33333333U, 0x44444444U, 0x55555555U};
    load_script(words, 4);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_ENTROPY, skiff_entropy_fill(&source, out, 8));
    const size_t reads_at_failure = generator.reads;
    memset(out, POISON, sizeof out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_ENTROPY, skiff_entropy_fill(&source, out, 8));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(reads_at_failure, generator.reads,
                                     "a failed source must not be read again");
    assert_zeroed(out, 8);
}

static void test_read_error_propagates_wipes_output_and_is_permanent(void) {
    const uint32_t words[] = {0x11111111U, 0x22222222U, 0x33333333U};
    load_script(words, 3);
    generator.fail_with = SKIFF_ERR_NOT_IMPLEMENTED;
    generator.fail_at = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NOT_IMPLEMENTED, skiff_entropy_fill(&source, out, 12));
    assert_zeroed(out, 12);
    generator.fail_with = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_ENTROPY, skiff_entropy_fill(&source, out, 4));
}

static void test_mbedtls_sized_request_reads_32_words(void) {
    unsigned char request[MBEDTLS_GATHER_BYTES];
    for (size_t i = 0; i < SCRIPT_MAX_WORDS; i++) {
        generator.words[i] = 0x1000U + (uint32_t)i;
    }
    generator.count = SCRIPT_MAX_WORDS;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_entropy_fill(&source, request, sizeof request));
    TEST_ASSERT_EQUAL_size_t(SCRIPT_MAX_WORDS, generator.reads);
    TEST_ASSERT_EQUAL_HEX8(0x1F, request[MBEDTLS_GATHER_BYTES - 4]);
    TEST_ASSERT_EQUAL_HEX8(0x10, request[MBEDTLS_GATHER_BYTES - 3]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_null_arguments);
    RUN_TEST(test_init_accepts_a_null_context);
    RUN_TEST(test_fills_whole_words_in_little_endian_order);
    RUN_TEST(test_partial_final_word_reads_one_more_word_and_writes_only_out_size);
    RUN_TEST(test_zero_size_reads_nothing);
    RUN_TEST(test_fill_rejects_null_arguments);
    RUN_TEST(test_consecutive_repeat_fails_and_wipes_output);
    RUN_TEST(test_repeat_across_fills_is_detected);
    RUN_TEST(test_alternating_values_fail_the_proportion_test);
    RUN_TEST(test_short_cycle_fails_the_proportion_test);
    RUN_TEST(test_repeat_of_a_word_other_than_the_window_start_passes);
    RUN_TEST(test_window_start_may_recur_in_the_next_window);
    RUN_TEST(test_health_failure_is_permanent);
    RUN_TEST(test_read_error_propagates_wipes_output_and_is_permanent);
    RUN_TEST(test_mbedtls_sized_request_reads_32_words);
    return UNITY_END();
}
