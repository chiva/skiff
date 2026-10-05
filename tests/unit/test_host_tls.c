/*
 * The host build links the same TLS stack as the EBOOTs (docker/toolchain/build-tls.sh) with the
 * host's link-time contracts (src/platform/host/tls_hooks.c). These checks prove the pieces fit:
 * libcurl runs over Mbed TLS 4.1, PSA crypto seeds from the hook, the clocks behave, and the cipher
 * order suits the PSP.
 */
#include <curl/curl.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/ssl.h>
#include <psa/crypto.h>
#include <string.h>
#include <time.h>

#include "unity.h"

enum { RANDOM_BYTES = 64, ENTROPY_REQUEST_BYTES = 128, BITS_PER_BYTE = 8 };

#define EXPECTED_TLS_BACKEND "mbedTLS/4.1."
/* Allowed gap between Mbed TLS's wall clock and time(): the default must be time() itself. */
#define WALL_CLOCK_TOLERANCE_S 2
/* Mbed TLS names its TLS 1.3 suites with this prefix; every other suite is TLS 1.2. */
#define TLS13_SUITE_PREFIX "TLS1-3-"
#define CHACHA20_POLY1305 "CHACHA20-POLY1305"
#define AES_128_GCM "AES-128-GCM"

/* The first suite of one TLS version in Mbed TLS's default order, and whether it offers one with
 * `cipher` at all. */
typedef struct suite_order {
    const char *first;
    int offers_cipher;
} suite_order;

void setUp(void) {}

void tearDown(void) {}

static void test_libcurl_uses_mbedtls_4_1(void) {
    const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    TEST_PRINTF("libcurl %s over %s", info->version, info->ssl_version);
    TEST_ASSERT_EQUAL_STRING_LEN(EXPECTED_TLS_BACKEND, info->ssl_version,
                                 strlen(EXPECTED_TLS_BACKEND));
}

static void test_curl_global_init_seeds_tls(void) {
    TEST_ASSERT_EQUAL_INT(CURLE_OK, curl_global_init(CURL_GLOBAL_DEFAULT));
    curl_global_cleanup();
}

static void test_psa_random_comes_from_the_hook(void) {
    unsigned char first[RANDOM_BYTES];
    unsigned char second[RANDOM_BYTES];
    TEST_ASSERT_EQUAL_INT(PSA_SUCCESS, psa_crypto_init());
    TEST_ASSERT_EQUAL_INT(PSA_SUCCESS, psa_generate_random(first, sizeof first));
    TEST_ASSERT_EQUAL_INT(PSA_SUCCESS, psa_generate_random(second, sizeof second));
    TEST_ASSERT_FALSE_MESSAGE(memcmp(first, second, sizeof first) == 0,
                              "two random draws must differ");
    mbedtls_psa_crypto_free();
}

static void test_hook_credits_full_entropy(void) {
    unsigned char output[ENTROPY_REQUEST_BYTES];
    size_t estimate_bits = 0;
    TEST_ASSERT_EQUAL_INT(0,
                          mbedtls_platform_get_entropy(0, &estimate_bits, output, sizeof output));
    TEST_PRINTF("hook credited %zu bits for %zu bytes", estimate_bits, sizeof output);
    TEST_ASSERT_EQUAL_size_t(sizeof output * BITS_PER_BYTE, estimate_bits);
}

static void test_hook_refuses_requests_it_cannot_satisfy(void) {
    unsigned char output[ENTROPY_REQUEST_BYTES];
    size_t estimate_bits = 1;
    TEST_ASSERT_EQUAL_INT(PSA_ERROR_INSUFFICIENT_ENTROPY,
                          mbedtls_platform_get_entropy(1, &estimate_bits, output, sizeof output));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, estimate_bits, "a refusal must credit nothing");
    TEST_ASSERT_EQUAL_INT(PSA_ERROR_INSUFFICIENT_ENTROPY,
                          mbedtls_platform_get_entropy(0, NULL, output, sizeof output));
}

static void test_millisecond_clock_is_monotonic(void) {
    const mbedtls_ms_time_t first = mbedtls_ms_time();
    const mbedtls_ms_time_t second = mbedtls_ms_time();
    TEST_PRINTF("mbedtls_ms_time() %lld then %lld", (long long)first, (long long)second);
    TEST_ASSERT_GREATER_THAN_INT64(0, first);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(first, second);
}

static void test_certificate_clock_defaults_to_time(void) {
    const long long now = (long long)time(NULL);
    const long long tls_now = (long long)mbedtls_time(NULL);
    TEST_PRINTF("time() %lld, mbedtls_time() %lld", now, tls_now);
    TEST_ASSERT_INT64_WITHIN(WALL_CLOCK_TOLERANCE_S, now, tls_now);
}

static suite_order default_order(int tls13, const char *cipher) {
    suite_order order = {NULL, 0};
    for (const int *id = mbedtls_ssl_list_ciphersuites(); *id != 0; id++) {
        const char *name = mbedtls_ssl_get_ciphersuite_name(*id);
        if ((strncmp(name, TLS13_SUITE_PREFIX, strlen(TLS13_SUITE_PREFIX)) == 0) != tls13) {
            continue;
        }
        order.first = order.first != NULL ? order.first : name;
        order.offers_cipher |= strstr(name, cipher) != NULL;
    }
    return order;
}

/*
 * The curl transport sets no cipher list, so it offers Mbed TLS's default order. On a PSP-1000
 * ChaCha20-Poly1305 decrypts at 3 MB/s and AES-128-GCM at 0.37 MB/s, which halves HTTPS downloads
 * (about 350 against 175 KB/s), so ChaCha20 must come first for both TLS versions; AES-GCM must
 * still be offered for servers without ChaCha20. A server that honours the client's order, or that
 * takes a client listing ChaCha20 first as one without AES hardware (Go's, so Caddy and Traefik),
 * then picks ChaCha20.
 */
static void test_chacha20_is_offered_first_with_aes_gcm_after(void) {
    for (int tls13 = 1; tls13 >= 0; tls13--) {
        const suite_order chacha = default_order(tls13, CHACHA20_POLY1305);
        const suite_order aes = default_order(tls13, AES_128_GCM);
        TEST_PRINTF("TLS 1.%d: first suite %s, AES-128-GCM offered: %s", tls13 ? 3 : 2,
                    chacha.first != NULL ? chacha.first : "(none)",
                    aes.offers_cipher ? "yes" : "no");
        TEST_ASSERT_NOT_NULL(chacha.first);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(chacha.first, CHACHA20_POLY1305),
                                     "ChaCha20-Poly1305 must be the first suite offered");
        TEST_ASSERT_TRUE_MESSAGE(aes.offers_cipher, "AES-128-GCM must stay offered as a fallback");
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_libcurl_uses_mbedtls_4_1);
    RUN_TEST(test_curl_global_init_seeds_tls);
    RUN_TEST(test_psa_random_comes_from_the_hook);
    RUN_TEST(test_hook_credits_full_entropy);
    RUN_TEST(test_hook_refuses_requests_it_cannot_satisfy);
    RUN_TEST(test_millisecond_clock_is_monotonic);
    RUN_TEST(test_certificate_clock_defaults_to_time);
    RUN_TEST(test_chacha20_is_offered_first_with_aes_gcm_after);
    return UNITY_END();
}
