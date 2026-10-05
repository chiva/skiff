/*
 * Clocks for Mbed TLS, linked into every EBOOT that uses TLS (skiff_psp_tls):
 *
 *   - mbedtls_ms_time() (MBEDTLS_PLATFORM_MS_TIME_ALT, a link-time contract): milliseconds for TLS
 *     1.3 session ticket ages. It must be monotonic, not wall-clock time.
 *   - the wall clock certificate dates are checked against (MBEDTLS_PLATFORM_TIME_ALT): seconds
 *     since 1970 in UTC, from the PSP's real-time clock. The C library's time() cannot serve: on
 * the PSP it returns only the time of day. Installed by a constructor, so it is in place before any
 *     code can start a TLS session.
 */
#include <mbedtls/platform_time.h>
#include <psprtc.h>
#include <pspthreadman.h>

#define SKIFF_MICROSECONDS_PER_MILLISECOND 1000
#define SKIFF_MICROSECONDS_PER_SECOND 1000000ULL
/* sceRtc ticks are microseconds since 0001-01-01 00:00 UTC; this many seconds precede 1970. */
#define SKIFF_RTC_SECONDS_BEFORE_UNIX_EPOCH 62135596800ULL
/* What the clock reports when the RTC cannot be read: 1970, which no certificate accepts. */
#define SKIFF_CLOCK_UNKNOWN 0

mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)(sceKernelGetSystemTimeWide() / SKIFF_MICROSECONDS_PER_MILLISECOND);
}

static mbedtls_time_t rtc_time(mbedtls_time_t *out) {
    u64 tick = 0;
    mbedtls_time_t now = SKIFF_CLOCK_UNKNOWN;
    if (sceRtcGetCurrentTick(&tick) >= 0) {
        const unsigned long long seconds = tick / SKIFF_MICROSECONDS_PER_SECOND;
        if (seconds > SKIFF_RTC_SECONDS_BEFORE_UNIX_EPOCH) {
            now = (mbedtls_time_t)(seconds - SKIFF_RTC_SECONDS_BEFORE_UNIX_EPOCH);
        }
    }
    if (out != NULL) {
        *out = now;
    }
    return now;
}

__attribute__((constructor)) static void install_tls_clock(void) {
    mbedtls_platform_set_time(rtc_time);
}
