/*
 * Mbed TLS's link-time contracts for host test binaries (never an EBOOT), so unit and integration
 * tests run the same TLS stack the PSP links (see docs/development/toolchain.md):
 *
 *   - mbedtls_platform_get_entropy(): the operating system's generator through getrandom(). On the
 *     host that is a sound source; the PSP's equivalent is src/platform/psp/kirk_entropy.c. Same
 *     contract: full entropy or PSA_ERROR_INSUFFICIENT_ENTROPY, never a partial credit.
 *   - mbedtls_ms_time(): a monotonic millisecond clock for TLS 1.3 session ticket ages.
 *
 * The wall clock for certificate dates stays Mbed TLS's default, the C library's time(), so a test
 * can replace it with mbedtls_platform_set_time() to simulate a PSP whose date is wrong.
 */
#include <errno.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_time.h>
#include <psa/crypto.h>
#include <sys/random.h>
#include <time.h>

enum { BITS_PER_BYTE = 8 };

#define HOST_MILLISECONDS_PER_SECOND 1000LL
#define HOST_NANOSECONDS_PER_MILLISECOND 1000000L

int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *estimate_bits,
                                 unsigned char *output, size_t output_size) {
    if (estimate_bits == NULL) {
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    *estimate_bits = 0;
    if (flags != 0) {
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    size_t filled = 0;
    while (filled < output_size) {
        const ssize_t got = getrandom(output + filled, output_size - filled, 0);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return PSA_ERROR_INSUFFICIENT_ENTROPY;
        }
        filled += (size_t)got;
    }
    *estimate_bits = output_size * BITS_PER_BYTE;
    return 0;
}

mbedtls_ms_time_t mbedtls_ms_time(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (mbedtls_ms_time_t)now.tv_sec * HOST_MILLISECONDS_PER_SECOND +
           now.tv_nsec / HOST_NANOSECONDS_PER_MILLISECOND;
}
