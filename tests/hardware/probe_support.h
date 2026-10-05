#ifndef SKIFF_PROBE_SUPPORT_H
#define SKIFF_PROBE_SUPPORT_H

/*
 * Helpers shared by the hardware probes (tests/hardware/): the configuration file that
 * scripts/memstick.sh writes next to each probe, medians, rates and CRC-32. Plain C, so the host
 * unit tests cover them (tests/unit/test_probe_support.c).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
    SKIFF_PROBE_HOST_MAX = 64,
    SKIFF_PROBE_TOKEN_MAX = 128,
    SKIFF_PROBE_ROM_ID_MAX = 16,
    /* URL-encoded */
    SKIFF_PROBE_FILE_NAME_MAX = 128,
    SKIFF_PROBE_LINE_MAX = 160,
    SKIFF_PROBE_DEFAULT_PROFILE = 1,
    SKIFF_PROBE_DEFAULT_RUNS = 3,
    SKIFF_PROBE_RUNS_MAX = 9,
    /* The CPU clocks "clock_mhz=" accepts: the homebrew default and the PSP's highest. */
    SKIFF_PROBE_CLOCK_DEFAULT_MHZ = 222,
    SKIFF_PROBE_CLOCK_FAST_MHZ = 333,
};

/* What a probe measures, chosen with "sections=" (comma-separated names, or "all"). */
typedef enum skiff_probe_section {
    SKIFF_PROBE_SECTION_LATENCY = 1U << 0,
    SKIFF_PROBE_SECTION_CPU = 1U << 1,
    SKIFF_PROBE_SECTION_NET = 1U << 2,
    SKIFF_PROBE_SECTION_MS = 1U << 3,
    SKIFF_PROBE_SECTION_APP = 1U << 4,
    SKIFF_PROBE_SECTION_CLOCK = 1U << 5,
    SKIFF_PROBE_SECTION_ALL = (1U << 6) - 1,
} skiff_probe_section;

typedef struct skiff_probe_config {
    int profile;
    char host[SKIFF_PROBE_HOST_MAX];
    /* romm-lan keeps plain HTTP off the LAN unless asked; memstick.sh says which. */
    int plain_http;
    /* The test server's seeded file and API token (romm.json), for the download checks. */
    char token[SKIFF_PROBE_TOKEN_MAX];
    char rom_id[SKIFF_PROBE_ROM_ID_MAX];
    char file_name[SKIFF_PROBE_FILE_NAME_MAX];
    unsigned long long size;
    uint32_t crc32;
    int has_crc32;
    /* Repetitions per measurement, 1 to SKIFF_PROBE_RUNS_MAX. */
    int runs;
    unsigned sections;
    /* CPU clock to set before the network modules load, or 0 to leave it. */
    int clock_mhz;
    /* Lines whose value could not be used; the default stays in place for them. */
    int invalid_values;
} skiff_probe_config;

void skiff_probe_config_defaults(skiff_probe_config *config);

/*
 * Reads "key=value" lines ('#' starts a comment, surrounding blanks and CR are ignored) into
 * config, which must hold the defaults or earlier values. Unknown keys are ignored, so one file can
 * serve several probes; a known key with an unusable value counts in invalid_values.
 */
void skiff_probe_config_read(FILE *file, skiff_probe_config *config);

/* 1 if config names a server, a profile, the token and the seeded file with its size and CRC-32. */
int skiff_probe_config_complete(const skiff_probe_config *config);

/* Sorts values in place and returns the median (the mean of the middle two for an even count); 0
 * for none. values[0] and values[count - 1] are then the minimum and maximum. */
long long skiff_probe_median(long long *values, size_t count);

/* KB/s (1 KB = 1024 bytes) for bytes over elapsed_us; 0 when no time elapsed. */
unsigned long long skiff_probe_kb_per_s(unsigned long long bytes, long long elapsed_us);

/*
 * CRC-32 (IEEE 802.3, reflected), what RomM records as crc_hash. Both continue a previous result
 * like zlib's crc32(): start with 0, feed chunks, the last return value is the CRC. The bitwise
 * version is the smallest code; the table version (1 KB table) is the candidate for downloads.
 */
uint32_t skiff_probe_crc32_bitwise(uint32_t crc, const unsigned char *data, size_t size);
uint32_t skiff_probe_crc32_table(uint32_t crc, const unsigned char *data, size_t size);

#endif
