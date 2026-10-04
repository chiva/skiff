/*
 * Skiff's TLS entropy hook: the only seed Mbed TLS's random generator gets (see
 * docs/development/tls.md). Words come from the KIRK crypto engine's random generator through
 * ARK's sctrlKernelRand(), checked by skiff_entropy_fill()'s health test. Without ARK, or after a
 * health failure, every request is refused and TLS cannot start.
 *
 * Not thread-safe: Skiff makes its network calls from one thread.
 */
#include <mbedtls/platform.h>
#include <psa/crypto.h>

#include "skiff/entropy.h"

#include "ark_sysctrl.h"

enum { BITS_PER_BYTE = 8 };

typedef enum kirk_state { KIRK_UNCHECKED, KIRK_READY, KIRK_UNAVAILABLE } kirk_state;

static kirk_state state = KIRK_UNCHECKED;
static skiff_entropy_source kirk_source;

static skiff_err read_kirk(void *ctx, uint32_t *word) {
    (void)ctx;
    *word = sctrlKernelRand();
    return SKIFF_OK;
}

/* Without ARK every ARK import returns SKIFF_PSP_IMPORT_NOT_LINKED. The health test would reject
 * that constant at the second word; checking once up front refuses before reading any. */
static kirk_state check_kirk(void) {
    if (sctrlHENGetVersion() == SKIFF_PSP_IMPORT_NOT_LINKED ||
        skiff_entropy_source_init(&kirk_source, read_kirk, NULL) != SKIFF_OK) {
        return KIRK_UNAVAILABLE;
    }
    return KIRK_READY;
}

int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *estimate_bits,
                                 unsigned char *output, size_t output_size) {
    if (estimate_bits == NULL) {
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    *estimate_bits = 0;
    if (flags != 0) {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    if (state == KIRK_UNCHECKED) {
        state = check_kirk();
    }
    if (state != KIRK_READY || skiff_entropy_fill(&kirk_source, output, output_size) != SKIFF_OK) {
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    *estimate_bits = output_size * BITS_PER_BYTE;
    return 0;
}
