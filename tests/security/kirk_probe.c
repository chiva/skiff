/*
 * KIRK random generator probe (Phase 1 hardware spike). Measures what Skiff's entropy design
 * depends on before that design is written:
 *
 *   - whether ARK's sctrlKernelRand() (the KIRK crypto engine's random generator) is available;
 *   - how long one Mbed TLS entropy request takes: 128 bytes, i.e. 32 calls, timed as a block,
 *     the first block separately since it includes any one-off setup;
 *   - the same timing for the baseline, the C library's getentropy(), the toolchain's default
 *     entropy source, so the cost of switching to KIRK is a ratio rather than a bare number;
 *   - whether 1024 consecutive values look random (no repeats, balanced bits, flat byte histogram);
 *   - whether the sequence repeats across runs and power cycles: each run compares its first values
 *     with every earlier run recorded in kirk-log.txt next to the EBOOT, fails on a match, then
 *     appends its own. Run it several times, rebooting in between; one run alone proves nothing;
 *   - end to end, with Skiff's real entropy hook (src/platform/psp/kirk_entropy.c) linked in: that
 *     psa_crypto_init() and curl_global_init() succeed, how long each takes, and that two
 *     psa_generate_random() draws differ.
 *
 * The in-run checks catch gross failures only; passing them does not prove the generator is strong.
 *
 * Without ARK (PPSSPP in CI, or a PSP running other firmware) there is nothing to measure. The
 * probe then checks that Skiff's hook refuses, so TLS fails closed: psa_crypto_init() and
 * curl_global_init() must both fail, and skiff_psp_entropy_status() must name the reason the player
 * sees (SKIFF_ERR_NET_NEEDS_ARK). That run ends with its own marker, SKIFF KIRK PROBE NO ARK OK.
 */
#include <curl/curl.h>
#include <mbedtls/platform.h>
#include <psa/crypto.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "skiff/selftest.h"

#include "ark_sysctrl.h"
#include "kirk_entropy.h"
#include "lifecycle.h"
#include "report.h"

#define PROBE_OK_MARKER "SKIFF KIRK PROBE OK"
#define PROBE_FAIL_MARKER "SKIFF KIRK PROBE FAIL"
#define PROBE_NO_ARK_OK_MARKER "SKIFF KIRK PROBE NO ARK OK"
#define PROBE_NO_ARK_FAIL_MARKER "SKIFF KIRK PROBE NO ARK FAIL"
#define PROBE_LOG_FILE "kirk-log.txt"

enum {
    PROBE_SAMPLES = 1024,
    /* MBEDTLS_ENTROPY_MAX_GATHER (128 bytes) in 4-byte calls. */
    PROBE_GATHER_WORDS = 32,
    PROBE_GATHERS = PROBE_SAMPLES / PROBE_GATHER_WORDS,
    PROBE_FINGERPRINT_WORDS = 4,
    PROBE_BYTE_VALUES = 256,
    PROBE_BITS_PER_WORD = 32,
    PROBE_BYTES_PER_WORD = 4,
    PROBE_PATH_MAX = 256,
    /* Longer than a fingerprint line, so fgets() reads one whole line at a time. */
    PROBE_LOG_LINE_MAX = 320,
    PROBE_RANDOM_DRAW_BYTES = 32,
};

/*
 * Bounds five standard deviations either side of what 4096 uniformly random bytes give: a working
 * generator lands well inside them, so crossing one means something is badly wrong.
 *   ones in 32768 bits: mean 16384, sd 90.5;  byte chi-squared, 255 dof: mean 255, sd 22.6.
 */
#define PROBE_ONES_MIN 15931U
#define PROBE_ONES_MAX 16837U
#define PROBE_CHI2_MIN 142.0
#define PROBE_CHI2_MAX 368.0

static unsigned int samples[PROBE_SAMPLES];
/* Only timed; the baseline's output is not analysed. */
static unsigned int baseline_samples[PROBE_SAMPLES];

typedef struct gather_timing {
    long long first_us;
    long long min_us;
    long long max_us;
    long long total_us;
    int failures;
} gather_timing;

/* Fills one Mbed TLS request's worth of words; returns 0 on success. */
typedef int (*gather_fn)(unsigned int *words);

static int gather_kirk(unsigned int *words) {
    for (size_t word = 0; word < PROBE_GATHER_WORDS; word++) {
        words[word] = sctrlKernelRand();
    }
    return 0;
}

static int gather_baseline(unsigned int *words) {
    return getentropy(words, PROBE_GATHER_WORDS * PROBE_BYTES_PER_WORD);
}

/* Fills out (PROBE_SAMPLES words) in Mbed TLS-sized blocks, timing each block. */
static gather_timing time_gathers(gather_fn gather_into, unsigned int *out) {
    gather_timing timing = {0, 0, 0, 0, 0};
    for (size_t gather = 0; gather < PROBE_GATHERS; gather++) {
        const long long start_us = sceKernelGetSystemTimeWide();
        if (gather_into(out + gather * PROBE_GATHER_WORDS) != 0) {
            timing.failures++;
        }
        const long long elapsed_us = sceKernelGetSystemTimeWide() - start_us;
        if (gather == 0) {
            timing.first_us = elapsed_us;
            timing.min_us = elapsed_us;
            timing.max_us = elapsed_us;
        } else if (elapsed_us < timing.min_us) {
            timing.min_us = elapsed_us;
        } else if (elapsed_us > timing.max_us) {
            timing.max_us = elapsed_us;
        }
        timing.total_us += elapsed_us;
    }
    return timing;
}

static void report_timing(skiff_psp_report *report, const char *source,
                          const gather_timing *timing) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line,
             "%s 128-byte gather: mean %.1f us, first %lld us, min %lld us, max %lld us", source,
             (double)timing->total_us / PROBE_GATHERS, timing->first_us, timing->min_us,
             timing->max_us);
    skiff_psp_report_line(report, line);
    if (timing->failures != 0) {
        snprintf(line, sizeof line, "%s failed %d of %d gathers", source, timing->failures,
                 PROBE_GATHERS);
        skiff_psp_report_line(report, line);
    }
}

static void report_ratio(skiff_psp_report *report, const gather_timing *kirk,
                         const gather_timing *baseline) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    if (baseline->failures != 0) {
        snprintf(line, sizeof line, "FAIL KIRK vs baseline: no ratio, the baseline failed");
    } else if (baseline->total_us == 0) {
        snprintf(line, sizeof line, "KIRK vs baseline: baseline below the 1 us timer resolution");
    } else {
        snprintf(line, sizeof line, "KIRK vs baseline: %.1fx the time per gather",
                 (double)kirk->total_us / (double)baseline->total_us);
    }
    skiff_psp_report_line(report, line);
}

typedef struct stack_timing {
    long long psa_init_us;
    long long curl_init_us;
} stack_timing;

/* What the network layer will report when TLS does not start, as the player would see it. */
static int check_entropy_status(skiff_psp_report *report, skiff_err expected) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    const skiff_err status = skiff_psp_entropy_status();
    snprintf(line, sizeof line, "%s entropy status %s (expect %s): \"%s [%d]\"",
             status == expected ? "ok  " : "FAIL", skiff_err_name(status), skiff_err_name(expected),
             skiff_err_message(status), (int)status);
    skiff_psp_report_line(report, line);
    return status == expected;
}

/* Skiff's TLS stack seeded by its real entropy hook: what the app will do at startup. */
static int check_skiff_entropy(skiff_psp_report *report, stack_timing *timing) {
    char line[SKIFF_SELFTEST_LINE_MAX];

    long long start_us = sceKernelGetSystemTimeWide();
    const psa_status_t init_status = psa_crypto_init();
    timing->psa_init_us = sceKernelGetSystemTimeWide() - start_us;
    snprintf(line, sizeof line, "%s psa_crypto_init() with Skiff's entropy = %d in %lld us",
             init_status == PSA_SUCCESS ? "ok  " : "FAIL", (int)init_status, timing->psa_init_us);
    skiff_psp_report_line(report, line);
    if (init_status != PSA_SUCCESS) {
        return 0;
    }

    unsigned char first[PROBE_RANDOM_DRAW_BYTES];
    unsigned char second[PROBE_RANDOM_DRAW_BYTES];
    const psa_status_t first_status = psa_generate_random(first, sizeof first);
    const psa_status_t second_status = psa_generate_random(second, sizeof second);
    const int both_drawn = first_status == PSA_SUCCESS && second_status == PSA_SUCCESS;
    /* Buffers are only defined after a successful draw. */
    const int draws_ok = both_drawn && memcmp(first, second, sizeof first) != 0;
    snprintf(line, sizeof line, "%s psa_generate_random() x2 = %d, %d, draws differ: %s",
             draws_ok ? "ok  " : "FAIL", (int)first_status, (int)second_status,
             both_drawn ? (draws_ok ? "yes" : "no") : "n/a");
    skiff_psp_report_line(report, line);

    start_us = sceKernelGetSystemTimeWide();
    const CURLcode curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    timing->curl_init_us = sceKernelGetSystemTimeWide() - start_us;
    snprintf(line, sizeof line, "%s curl_global_init() = %d in %lld us",
             curl_status == CURLE_OK ? "ok  " : "FAIL", (int)curl_status, timing->curl_init_us);
    skiff_psp_report_line(report, line);
    if (curl_status == CURLE_OK) {
        curl_global_cleanup();
    }
    const int status_ok = check_entropy_status(report, SKIFF_OK);
    return draws_ok && curl_status == CURLE_OK && status_ok;
}

/* A request with flags, which no supported Mbed TLS makes, must be refused and disable the source,
 * so the status matches what TLS sees. Run last: it leaves the hook refusing. */
static int check_refuses_unsupported_request(skiff_psp_report *report) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    unsigned char output[PROBE_RANDOM_DRAW_BYTES];
    size_t estimate_bits = 0;
    const int status = mbedtls_platform_get_entropy(1, &estimate_bits, output, sizeof output);
    const int refused = status == PSA_ERROR_INSUFFICIENT_ENTROPY && estimate_bits == 0;
    snprintf(line, sizeof line, "%s entropy request with flags = %d, %zu bits (expect %d, 0 bits)",
             refused ? "ok  " : "FAIL", status, estimate_bits, (int)PSA_ERROR_INSUFFICIENT_ENTROPY);
    skiff_psp_report_line(report, line);
    return refused && check_entropy_status(report, SKIFF_ERR_NET_ENTROPY);
}

static int check_refuses_without_ark(skiff_psp_report *report) {
    char line[SKIFF_SELFTEST_LINE_MAX];

    const psa_status_t init_status = psa_crypto_init();
    const int init_refused = init_status == PSA_ERROR_INSUFFICIENT_ENTROPY;
    snprintf(line, sizeof line, "%s psa_crypto_init() = %d (expect %d, insufficient entropy)",
             init_refused ? "ok  " : "FAIL", (int)init_status, (int)PSA_ERROR_INSUFFICIENT_ENTROPY);
    skiff_psp_report_line(report, line);

    const CURLcode curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    const int curl_refused = curl_status != CURLE_OK;
    snprintf(line, sizeof line, "%s curl_global_init() = %d (expect an error)",
             curl_refused ? "ok  " : "FAIL", (int)curl_status);
    skiff_psp_report_line(report, line);
    if (!curl_refused) {
        curl_global_cleanup();
    }
    const int status_ok = check_entropy_status(report, SKIFF_ERR_NET_NEEDS_ARK);
    return init_refused && curl_refused && status_ok;
}

static unsigned count_repeats(void) {
    unsigned repeats = 0;
    for (size_t i = 0; i < PROBE_SAMPLES; i++) {
        for (size_t j = i + 1; j < PROBE_SAMPLES; j++) {
            if (samples[i] == samples[j]) {
                repeats++;
            }
        }
    }
    return repeats;
}

static unsigned count_ones(void) {
    unsigned ones = 0;
    for (size_t i = 0; i < PROBE_SAMPLES; i++) {
        ones += (unsigned)__builtin_popcount(samples[i]);
    }
    return ones;
}

static double byte_chi_squared(void) {
    unsigned histogram[PROBE_BYTE_VALUES] = {0};
    for (size_t i = 0; i < PROBE_SAMPLES; i++) {
        for (unsigned shift = 0; shift < PROBE_BITS_PER_WORD; shift += 8) {
            histogram[(samples[i] >> shift) & 0xFFU]++;
        }
    }
    const double expected = (double)(PROBE_SAMPLES * PROBE_BYTES_PER_WORD) / PROBE_BYTE_VALUES;
    double chi2 = 0.0;
    for (size_t value = 0; value < PROBE_BYTE_VALUES; value++) {
        const double difference = (double)histogram[value] - expected;
        chi2 += difference * difference / expected;
    }
    return chi2;
}

/*
 * Compares this run's first values with every earlier run in the log. A match means KIRK produced
 * the same sequence twice, e.g. restarting it after a power cycle: the failure that would make it
 * unfit as the only TLS seed. Lines without a parsable first= field are skipped.
 */
static int differs_from_earlier_runs(skiff_psp_report *report, const char *program_path) {
    char path[PROBE_PATH_MAX];
    char line[SKIFF_SELFTEST_LINE_MAX];
    if (skiff_selftest_sibling_path(program_path, PROBE_LOG_FILE, path, sizeof path) != SKIFF_OK) {
        skiff_psp_report_line(report, "FAIL earlier runs: no EBOOT path to find " PROBE_LOG_FILE);
        return 0;
    }
    unsigned earlier_runs = 0;
    unsigned matches = 0;
    FILE *log = fopen(path, "r");
    if (log != NULL) {
        char entry[PROBE_LOG_LINE_MAX];
        while (fgets(entry, sizeof entry, log) != NULL) {
            const char *first = strstr(entry, "first=");
            unsigned int recorded[PROBE_FINGERPRINT_WORDS];
            if (first == NULL || sscanf(first, "first=%x %x %x %x", &recorded[0], &recorded[1],
                                        &recorded[2], &recorded[3]) != PROBE_FINGERPRINT_WORDS) {
                continue;
            }
            earlier_runs++;
            if (memcmp(recorded, samples, sizeof recorded) == 0) {
                matches++;
            }
        }
        fclose(log);
    }
    snprintf(line, sizeof line, "%s first values match %u of %u earlier runs in %s (expect 0)",
             matches == 0 ? "ok  " : "FAIL", matches, earlier_runs, PROBE_LOG_FILE);
    skiff_psp_report_line(report, line);
    return matches == 0;
}

/* One line per run, appended, so runs across reboots end up side by side in one file. The log is
 * what the power-cycle comparison reads, so failing to write it fails the probe. */
static int append_fingerprint(skiff_psp_report *report, const char *program_path,
                              long long uptime_us, const gather_timing *kirk,
                              const gather_timing *baseline, const stack_timing *stack) {
    char path[PROBE_PATH_MAX];
    if (skiff_selftest_sibling_path(program_path, PROBE_LOG_FILE, path, sizeof path) != SKIFF_OK) {
        skiff_psp_report_line(report, "FAIL fingerprint log: not written (no EBOOT path)");
        return 0;
    }
    FILE *log = fopen(path, "a");
    if (log == NULL) {
        skiff_psp_report_line(report, "FAIL fingerprint log: could not open " PROBE_LOG_FILE);
        return 0;
    }
    const int written = fprintf(
        log,
        "uptime_us=%lld first=%08x %08x %08x %08x kirk_total_us=%lld kirk_max_us=%lld "
        "baseline_total_us=%lld baseline_max_us=%lld psa_init_us=%lld curl_init_us=%lld\n",
        uptime_us, samples[0], samples[1], samples[2], samples[3], kirk->total_us, kirk->max_us,
        baseline->total_us, baseline->max_us, stack->psa_init_us, stack->curl_init_us);
    const int closed = fclose(log);
    if (written < 0 || closed != 0) {
        skiff_psp_report_line(report, "FAIL fingerprint log: write to " PROBE_LOG_FILE " failed");
        return 0;
    }
    skiff_psp_report_line(report, "fingerprint appended to " PROBE_LOG_FILE);
    return 1;
}

int main(int argc, char *argv[]) {
    skiff_psp_report report;
    const char *program_path = argc > 0 ? argv[0] : NULL;

    skiff_psp_install_exit_callback();
    skiff_psp_report_open(&report, program_path);

    const int version = sctrlHENGetVersion();
    if (version == SKIFF_PSP_IMPORT_NOT_LINKED) {
        skiff_psp_report_line(&report, "ARK SystemCtrlForUser not available: nothing to measure;");
        skiff_psp_report_line(&report, "checking that Skiff's TLS refuses to start instead");
        const int refused = check_refuses_without_ark(&report);
        skiff_psp_report_line(&report, refused ? PROBE_NO_ARK_OK_MARKER : PROBE_NO_ARK_FAIL_MARKER);
        skiff_psp_report_close(&report);
        sceKernelExitGame();
        return refused ? 0 : 1;
    }
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "ARK version 0x%08x", (unsigned)version);
    skiff_psp_report_line(&report, line);

    const long long uptime_us = sceKernelGetSystemTimeWide();
    const gather_timing kirk = time_gathers(gather_kirk, samples);
    const gather_timing baseline = time_gathers(gather_baseline, baseline_samples);
    snprintf(line, sizeof line, "%d KIRK calls in %lld us; uptime at start %lld us", PROBE_SAMPLES,
             kirk.total_us, uptime_us);
    skiff_psp_report_line(&report, line);
    report_timing(&report, "KIRK", &kirk);
    report_timing(&report, "baseline getentropy()", &baseline);
    report_ratio(&report, &kirk, &baseline);

    for (size_t i = 0; i < PROBE_FINGERPRINT_WORDS; i++) {
        snprintf(line, sizeof line, "sample[%zu] = 0x%08x", i, samples[i]);
        skiff_psp_report_line(&report, line);
    }

    const unsigned repeats = count_repeats();
    const unsigned ones = count_ones();
    const double chi2 = byte_chi_squared();

    const int repeats_ok = repeats == 0;
    snprintf(line, sizeof line, "%s repeated 32-bit values: %u (expect 0)",
             repeats_ok ? "ok  " : "FAIL", repeats);
    skiff_psp_report_line(&report, line);

    const int ones_ok = ones >= PROBE_ONES_MIN && ones <= PROBE_ONES_MAX;
    snprintf(line, sizeof line, "%s one bits: %u of %u (expect %u..%u)", ones_ok ? "ok  " : "FAIL",
             ones, PROBE_SAMPLES * PROBE_BITS_PER_WORD, PROBE_ONES_MIN, PROBE_ONES_MAX);
    skiff_psp_report_line(&report, line);

    const int chi2_ok = chi2 >= PROBE_CHI2_MIN && chi2 <= PROBE_CHI2_MAX;
    snprintf(line, sizeof line, "%s byte chi-squared: %.1f (expect %.0f..%.0f)",
             chi2_ok ? "ok  " : "FAIL", chi2, PROBE_CHI2_MIN, PROBE_CHI2_MAX);
    skiff_psp_report_line(&report, line);

    stack_timing stack = {0, 0};
    const int stack_ok = check_skiff_entropy(&report, &stack);

    const int unique = differs_from_earlier_runs(&report, program_path);
    const int logged =
        append_fingerprint(&report, program_path, uptime_us, &kirk, &baseline, &stack);

    const int unsupported_refused = check_refuses_unsupported_request(&report);

    const int passed = repeats_ok && ones_ok && chi2_ok && baseline.failures == 0 && stack_ok &&
                       unique && logged && unsupported_refused;
    skiff_psp_report_line(&report, passed ? PROBE_OK_MARKER : PROBE_FAIL_MARKER);
    skiff_psp_report_close(&report);
    sceKernelExitGame();
    return passed ? 0 : 1;
}
