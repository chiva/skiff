/*
 * The host build links the same TLS stack as the EBOOTs (docker/toolchain/build-tls.sh) with the
 * host's link-time contracts (src/platform/host/tls_hooks.c). These checks prove the pieces fit:
 * libcurl runs over Mbed TLS 4.1, PSA crypto seeds from the hook, and the clocks behave.
 */
#include <curl/curl.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_time.h>
#include <psa/crypto.h>
#include <string.h>
#include <time.h>

#include "unity.h"

enum { RANDOM_BYTES = 64, ENTROPY_REQUEST_BYTES = 128, BITS_PER_BYTE = 8 };

#define EXPECTED_TLS_BACKEND "mbedTLS/4.1."
/* Allowed gap between Mbed TLS's wall clock and time(): the default must be time() itself. */
#define WALL_CLOCK_TOLERANCE_S 2

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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_libcurl_uses_mbedtls_4_1);
    RUN_TEST(test_curl_global_init_seeds_tls);
    RUN_TEST(test_psa_random_comes_from_the_hook);
    RUN_TEST(test_hook_credits_full_entropy);
    RUN_TEST(test_hook_refuses_requests_it_cannot_satisfy);
    RUN_TEST(test_millisecond_clock_is_monotonic);
    RUN_TEST(test_certificate_clock_defaults_to_time);
    return UNITY_END();
}
