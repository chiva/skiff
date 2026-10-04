/*
 * Determinism probe for the PSP's getentropy(). Demonstrates the pspdev/pspsdk weakness this
 * project works around: getentropy() reseeds a Mersenne Twister with time(NULL) on every call, so
 * two calls within the same second return identical bytes (see docs/development/architecture.md).
 *
 * This is a DEFENSIVE check, not an attack: it reads only its own random bytes and compares them.
 * It prints the two samples and a verdict, then exits. It runs in PPSSPPHeadless and on real
 * hardware over PSPLINK, the same way as the self-test.
 *
 * Skiff does not repair getentropy(); its TLS stack is built so it cannot reach it (Mbed TLS takes
 * entropy only from Skiff's mbedtls_platform_get_entropy(), verified by tls_probe.c). This probe
 * therefore keeps reporting DETERMINISTIC until pspsdk itself is fixed upstream.
 */
#include <pspdebug.h>
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lifecycle.h"

#define PROBE_SAMPLE_BYTES 32
#define PROBE_DETERMINISTIC_MARKER "SKIFF ENTROPY PROBE: DETERMINISTIC (weak getentropy present)"
#define PROBE_OK_MARKER "SKIFF ENTROPY PROBE: VARYING (entropy source looks live)"
#define PROBE_ERROR_MARKER "SKIFF ENTROPY PROBE: ERROR"

static void log_line(const char *line) {
    printf("%s\n", line);
    pspDebugScreenPrintf("%s\n", line);
}

static void log_hex(const char *label, const unsigned char *bytes, size_t len) {
    char line[16 + PROBE_SAMPLE_BYTES * 2 + 1];
    int offset = snprintf(line, sizeof line, "%s ", label);
    for (size_t i = 0; i < len && offset < (int)sizeof line - 2; i++) {
        offset += snprintf(line + offset, (size_t)((int)sizeof line - offset), "%02x", bytes[i]);
    }
    log_line(line);
}

int main(void) {
    skiff_psp_install_exit_callback();
    pspDebugScreenInit();

    unsigned char first[PROBE_SAMPLE_BYTES];
    unsigned char second[PROBE_SAMPLE_BYTES];

    /* No sleep between the calls: the point is that two calls in the same second must differ for a
     * real entropy source, yet here they do not. */
    const int first_rc = getentropy(first, sizeof first);
    const int second_rc = getentropy(second, sizeof second);

    if (first_rc != 0 || second_rc != 0) {
        log_line(PROBE_ERROR_MARKER);
        fflush(stdout);
        sceKernelExitGame();
        return 1;
    }

    log_hex("sample-1", first, sizeof first);
    log_hex("sample-2", second, sizeof second);

    const int identical = memcmp(first, second, sizeof first) == 0;
    log_line(identical ? PROBE_DETERMINISTIC_MARKER : PROBE_OK_MARKER);

    fflush(stdout);
    sceKernelExitGame();
    /* Exit 0 means "ran and reported"; the marker line carries the finding. */
    return 0;
}
