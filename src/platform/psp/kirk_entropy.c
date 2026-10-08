/*
 * Skiff's TLS entropy hook: the only seed Mbed TLS's random generator gets (see
 * docs/development/tls.md). Words come from the KIRK crypto engine's random generator through
 * ARK's sctrlKernelRand(), checked by skiff_entropy_fill()'s health test. Without ARK, or after a
 * health failure, every request is refused and TLS cannot start; skiff_psp_entropy_status() says
 * which.
 *
 * PSA crypto calls it with its random generator's mutex held (MBEDTLS_THREADING_C, see
 * docker/toolchain/configure-mbedtls.sh), so one call runs at a time even though the UI and the
 * download worker both use TLS.
 */
#include "kirk_entropy.h"

#include <mbedtls/platform.h>
#include <psa/crypto.h>

#include "skiff/entropy.h"

#include "ark_sysctrl.h"

enum { BITS_PER_BYTE = 8 };

typedef enum ark_state { ARK_UNCHECKED, ARK_PRESENT, ARK_MISSING } ark_state;

static skiff_err read_kirk(void *ctx, uint32_t *word) {
    (void)ctx;
    *word = sctrlKernelRand();
    return SKIFF_OK;
}

static ark_state ark = ARK_UNCHECKED;
static skiff_entropy_source kirk_source = {.read = read_kirk};

/* Without ARK every ARK import returns SKIFF_PSP_IMPORT_NOT_LINKED. The health test would reject
 * that constant at the second word; checking once up front refuses before reading any. */
static int ark_present(void) {
    if (ark == ARK_UNCHECKED) {
        ark = sctrlHENGetVersion() == SKIFF_PSP_IMPORT_NOT_LINKED ? ARK_MISSING : ARK_PRESENT;
    }
    return ark == ARK_PRESENT;
}

skiff_err skiff_psp_entropy_status(void) {
    if (!ark_present()) {
        return SKIFF_ERR_NET_NEEDS_ARK;
    }
    return kirk_source.failed ? SKIFF_ERR_NET_ENTROPY : SKIFF_OK;
}

/* A request Skiff cannot satisfy as asked (non-zero flags, which TF-PSA-Crypto 1.x never passes, or
 * no estimate_bits) means an Mbed TLS it was not written for: refuse it and every later request, so
 * the status reports SKIFF_ERR_NET_ENTROPY instead of SKIFF_OK while TLS fails. */
static int refuse_permanently(void) {
    kirk_source.failed = 1;
    return PSA_ERROR_INSUFFICIENT_ENTROPY;
}

int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *estimate_bits,
                                 unsigned char *output, size_t output_size) {
    if (estimate_bits == NULL) {
        return refuse_permanently();
    }
    *estimate_bits = 0;
    if (flags != 0) {
        return refuse_permanently();
    }
    if (!ark_present() || skiff_entropy_fill(&kirk_source, output, output_size) != SKIFF_OK) {
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    *estimate_bits = output_size * BITS_PER_BYTE;
    return 0;
}
