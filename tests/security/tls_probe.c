/*
 * TLS toolchain probe. Proves, on the emulator and on hardware, what the Skiff toolchain image
 * promises (docs/development/toolchain.md):
 *
 *   1. libcurl is built against Mbed TLS 4.1 and speaks HTTP and HTTPS only;
 *   2. every random byte TLS needs comes from mbedtls_platform_get_entropy(), the hook Skiff
 *      implements, and from nothing else.
 *
 * For (2) this probe supplies an entropy callback that REFUSES every request and counts the calls.
 * If anything other than Skiff's hook could seed TLS, PSA and curl would initialise anyway. They
 * must instead fail, after asking the hook. The refusing callback never produces bytes, so it is
 * safe to ship in a test binary.
 */
#include <curl/curl.h>
#include <mbedtls/platform.h>
#include <psa/crypto.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

#include "lifecycle.h"
#include "report.h"

#define PROBE_OK_MARKER "SKIFF TLS PROBE OK"
#define PROBE_FAIL_MARKER "SKIFF TLS PROBE FAIL"
#define PROBE_EXPECTED_SSL_PREFIX "mbedTLS/4.1."

static unsigned entropy_requests;

int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *estimate_bits,
                                 unsigned char *output, size_t output_size) {
    (void)flags;
    (void)output;
    (void)output_size;
    entropy_requests++;
    *estimate_bits = 0;
    return PSA_ERROR_INSUFFICIENT_ENTROPY;
}

static int report_check(skiff_psp_report *report, int passed, const char *check,
                        const char *detail) {
    char line[160];
    snprintf(line, sizeof line, "%s %s: %s", passed ? "ok  " : "FAIL", check, detail);
    skiff_psp_report_line(report, line);
    return passed;
}

static int check_curl_build(skiff_psp_report *report) {
    const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    const char *ssl = info->ssl_version != NULL ? info->ssl_version : "(none)";
    char detail[120];

    snprintf(detail, sizeof detail, "libcurl %s with %s", info->version, ssl);
    const int expected_backend =
        strncmp(ssl, PROBE_EXPECTED_SSL_PREFIX, strlen(PROBE_EXPECTED_SSL_PREFIX)) == 0;
    int passed = report_check(report, expected_backend, "TLS backend is Mbed TLS 4.1", detail);

    int only_http = 1;
    int has_https = 0;
    for (const char *const *protocol = info->protocols; *protocol != NULL; protocol++) {
        if (strcmp(*protocol, "https") == 0) {
            has_https = 1;
        } else if (strcmp(*protocol, "http") != 0) {
            only_http = 0;
            snprintf(detail, sizeof detail, "unexpected protocol %s", *protocol);
        }
    }
    if (only_http) {
        snprintf(detail, sizeof detail, "%s", has_https ? "http, https" : "https missing");
    }
    passed &=
        report_check(report, only_http && has_https, "protocols are HTTP and HTTPS only", detail);
    return passed;
}

static int check_psa_needs_skiff_entropy(skiff_psp_report *report) {
    const unsigned before = entropy_requests;
    const psa_status_t status = psa_crypto_init();
    char detail[120];

    snprintf(detail, sizeof detail, "psa_crypto_init() = %d after %u entropy request(s)",
             (int)status, entropy_requests - before);
    return report_check(report,
                        status == PSA_ERROR_INSUFFICIENT_ENTROPY && entropy_requests > before,
                        "PSA RNG is seeded only through Skiff's hook", detail);
}

static int check_curl_refuses_without_entropy(skiff_psp_report *report) {
    const unsigned before = entropy_requests;
    const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    char detail[120];

    snprintf(detail, sizeof detail, "curl_global_init() = %d after %u entropy request(s)",
             (int)code, entropy_requests - before);
    const int passed = report_check(report, code != CURLE_OK && entropy_requests > before,
                                    "libcurl will not start TLS without entropy", detail);
    if (code == CURLE_OK) {
        curl_global_cleanup();
    }
    return passed;
}

int main(int argc, char *argv[]) {
    skiff_psp_report report;

    skiff_psp_install_exit_callback();
    skiff_psp_report_open(&report, argc > 0 ? argv[0] : NULL);

    int passed = check_curl_build(&report);
    passed &= check_psa_needs_skiff_entropy(&report);
    passed &= check_curl_refuses_without_entropy(&report);

    skiff_psp_report_line(&report, passed ? PROBE_OK_MARKER : PROBE_FAIL_MARKER);
    skiff_psp_report_close(&report);
    sceKernelExitGame();
    return passed ? 0 : 1;
}
