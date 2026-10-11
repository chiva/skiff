/*
 * MD5 (skiff/md5.h) against RFC 1321's test suite (appendix A.5), fed whole and in pieces that
 * cross block boundaries, and around the padding edge where the length needs a second block.
 */
#include <string.h>

#include "skiff/md5.h"

#include "unity.h"

#define MILLION_A_REPEATS 1000000U
#define MILLION_A_DIGEST "7707d6ae4e027c70eea2a935c2296f21"
/* 55 bytes is the most that padding and the length fit into one block, 56 the least that needs a
 * second; 64 fills a block exactly. */
#define ONE_BLOCK_MAX 55
#define TWO_BLOCKS_MIN 56
#define BLOCK 64
#define EDGE_55_A "ef1772b6dff9a122358552954ad0df65"
#define EDGE_56_A "3b0c8ac703f828b04c6c197006d17218"
#define EDGE_64_A "014842d480b571495a4a0363793f7367"

typedef struct vector {
    const char *input;
    const char *digest;
} vector;

/* RFC 1321, A.5. */
static const vector RFC_1321_SUITE[] = {
    {"", "d41d8cd98f00b204e9800998ecf8427e"},
    {"a", "0cc175b9c0f1b6a831c399e269772661"},
    {"abc", "900150983cd24fb0d6963f7d28e17f72"},
    {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
    {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
    {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
     "d174ab98d277d9f5a5611c2c9f419d9f"},
    {"1234567890"
     "1234567890"
     "1234567890"
     "1234567890"
     "1234567890"
     "1234567890"
     "1234567890"
     "1234567890",
     "57edf4a22be3c955ac49da2e2107b67a"},
};

static void hex_of(const void *data, size_t size, size_t piece, char out[SKIFF_MD5_HEX_SIZE]) {
    const unsigned char *bytes = data;
    skiff_md5 md5;
    skiff_md5_init(&md5);
    for (size_t at = 0; at < size; at += piece) {
        skiff_md5_update(&md5, bytes + at, size - at < piece ? size - at : piece);
    }
    unsigned char digest[SKIFF_MD5_DIGEST_BYTES];
    skiff_md5_final(&md5, digest);
    skiff_md5_hex(digest, out);
}

void setUp(void) {}

void tearDown(void) {}

static void test_the_rfc_1321_suite_hashes_to_its_published_digests(void) {
    for (size_t i = 0; i < sizeof RFC_1321_SUITE / sizeof RFC_1321_SUITE[0]; i++) {
        char hex[SKIFF_MD5_HEX_SIZE];
        const size_t size = strlen(RFC_1321_SUITE[i].input);
        hex_of(RFC_1321_SUITE[i].input, size, size > 0 ? size : 1, hex);
        TEST_PRINTF("MD5(\"%s\") = %s", RFC_1321_SUITE[i].input, hex);
        TEST_ASSERT_EQUAL_STRING(RFC_1321_SUITE[i].digest, hex);
    }
}

static void test_pieces_of_any_size_give_the_same_digest(void) {
    static const size_t PIECES[] = {1, 3, 7, 63, 64, 65};
    const vector *longest = &RFC_1321_SUITE[sizeof RFC_1321_SUITE / sizeof RFC_1321_SUITE[0] - 1];
    for (size_t i = 0; i < sizeof PIECES / sizeof PIECES[0]; i++) {
        char hex[SKIFF_MD5_HEX_SIZE];
        hex_of(longest->input, strlen(longest->input), PIECES[i], hex);
        TEST_PRINTF("80 digits in pieces of %zu: %s", PIECES[i], hex);
        TEST_ASSERT_EQUAL_STRING(longest->digest, hex);
    }
}

static void test_lengths_around_the_padding_edge(void) {
    char a[BLOCK];
    memset(a, 'a', sizeof a);
    char hex[SKIFF_MD5_HEX_SIZE];
    hex_of(a, ONE_BLOCK_MAX, ONE_BLOCK_MAX, hex);
    TEST_PRINTF("55 x 'a' (padding fits one block): %s", hex);
    TEST_ASSERT_EQUAL_STRING(EDGE_55_A, hex);
    hex_of(a, TWO_BLOCKS_MIN, TWO_BLOCKS_MIN, hex);
    TEST_PRINTF("56 x 'a' (length spills into a second block): %s", hex);
    TEST_ASSERT_EQUAL_STRING(EDGE_56_A, hex);
    hex_of(a, BLOCK, BLOCK, hex);
    TEST_PRINTF("64 x 'a' (a whole block, padding alone in the next): %s", hex);
    TEST_ASSERT_EQUAL_STRING(EDGE_64_A, hex);
}

static void test_a_million_bytes_in_one_call(void) {
    static char million[MILLION_A_REPEATS];
    memset(million, 'a', sizeof million);
    char hex[SKIFF_MD5_HEX_SIZE];
    hex_of(million, sizeof million, sizeof million, hex);
    TEST_PRINTF("1,000,000 x 'a': %s", hex);
    TEST_ASSERT_EQUAL_STRING(MILLION_A_DIGEST, hex);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_rfc_1321_suite_hashes_to_its_published_digests);
    RUN_TEST(test_pieces_of_any_size_give_the_same_digest);
    RUN_TEST(test_lengths_around_the_padding_edge);
    RUN_TEST(test_a_million_bytes_in_one_call);
    return UNITY_END();
}
