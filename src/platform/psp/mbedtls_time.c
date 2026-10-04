/*
 * Millisecond clock for Mbed TLS (MBEDTLS_PLATFORM_MS_TIME_ALT, set by the toolchain image). TLS
 * 1.3 uses it only to measure session ticket ages, so it must be monotonic; it need not be
 * wall-clock time. Certificate validity uses time() separately.
 */
#include <mbedtls/platform_time.h>
#include <pspthreadman.h>

#define SKIFF_MICROSECONDS_PER_MILLISECOND 1000

mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)(sceKernelGetSystemTimeWide() / SKIFF_MICROSECONDS_PER_MILLISECOND);
}
