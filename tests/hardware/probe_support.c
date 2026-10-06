#include "probe_support.h"

#include <stdlib.h>
#include <string.h>

#define CRC32_POLYNOMIAL 0xEDB88320U
#define SECTION_SEPARATORS ", "

enum {
    DECIMAL = 10,
    HEXADECIMAL = 16,
    BITS_PER_BYTE = 8,
    CRC32_TABLE_SIZE = 256,
    BYTE_MASK = 0xFF,
    US_PER_S = 1000000,
    BYTES_PER_KB = 1024,
};

typedef struct section_name {
    const char *name;
    unsigned bit;
} section_name;

static const section_name SECTION_NAMES[] = {
    {"latency", SKIFF_PROBE_SECTION_LATENCY}, {"cpu", SKIFF_PROBE_SECTION_CPU},
    {"net", SKIFF_PROBE_SECTION_NET},         {"ms", SKIFF_PROBE_SECTION_MS},
    {"app", SKIFF_PROBE_SECTION_APP},         {"clock", SKIFF_PROBE_SECTION_CLOCK},
    {"all", SKIFF_PROBE_SECTION_ALL},
};

static const section_name SCENARIO_NAMES[] = {
    {"restart", SKIFF_PROBE_SCENARIO_RESTART}, {"wifi", SKIFF_PROBE_SCENARIO_WIFI},
    {"suspend", SKIFF_PROBE_SCENARIO_SUSPEND}, {"home", SKIFF_PROBE_SCENARIO_HOME},
    {"sleep", SKIFF_PROBE_SCENARIO_SLEEP},     {"all", SKIFF_PROBE_SCENARIO_ALL},
};

void skiff_probe_config_defaults(skiff_probe_config *config) {
    memset(config, 0, sizeof *config);
    config->profile = SKIFF_PROBE_DEFAULT_PROFILE;
    config->runs = SKIFF_PROBE_DEFAULT_RUNS;
    config->sections = SKIFF_PROBE_SECTION_ALL;
    config->scenarios = SKIFF_PROBE_SCENARIO_ALL;
    config->wait_s = SKIFF_PROBE_DEFAULT_WAIT_S;
    config->awake_s = SKIFF_PROBE_DEFAULT_AWAKE_S;
}

static void trim_line(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r' ||
                          text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
}

/* A whole decimal number in [min, max], or 0 (and *value untouched). */
static int parse_bounded(const char *text, long min, long max, long *value) {
    char *end = NULL;
    const long parsed = strtol(text, &end, DECIMAL);
    if (text[0] == '\0' || *end != '\0' || parsed < min || parsed > max) {
        return 0;
    }
    *value = parsed;
    return 1;
}

/* "net,ms" -> their bits from names; 0 for an empty list or an unknown name. */
static unsigned parse_names(const char *text, const section_name *names, size_t count) {
    char copy[SKIFF_PROBE_LINE_MAX];
    snprintf(copy, sizeof copy, "%s", text);
    unsigned bits = 0;
    char *rest = copy;
    for (const char *name = strtok_r(rest, SECTION_SEPARATORS, &rest); name != NULL;
         name = strtok_r(NULL, SECTION_SEPARATORS, &rest)) {
        unsigned bit = 0;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(name, names[i].name) == 0) {
                bit = names[i].bit;
            }
        }
        if (bit == 0) {
            return 0;
        }
        bits |= bit;
    }
    return bits;
}

static int copy_value(char *out, size_t out_size, const char *value) {
    return snprintf(out, out_size, "%s", value) < (int)out_size;
}

/* 0 when a known key has an unusable value. */
static int apply(skiff_probe_config *config, const char *key, const char *value) {
    long number = 0;
    if (strcmp(key, "host") == 0) {
        return copy_value(config->host, sizeof config->host, value);
    }
    if (strcmp(key, "plain_http") == 0) {
        /* Anything but 0 or 1 is a typo, which would otherwise skip the plain-HTTP comparisons. */
        const int on = strcmp(value, "1") == 0;
        const int ok = on || strcmp(value, "0") == 0;
        config->plain_http = ok ? on : config->plain_http;
        return ok;
    }
    if (strcmp(key, "profile") == 0) {
        const int ok = parse_bounded(value, SKIFF_PROBE_DEFAULT_PROFILE, INT32_MAX, &number);
        config->profile = ok ? (int)number : config->profile;
        return ok;
    }
    if (strcmp(key, "token") == 0) {
        return copy_value(config->token, sizeof config->token, value);
    }
    if (strcmp(key, "rom_id") == 0) {
        return copy_value(config->rom_id, sizeof config->rom_id, value);
    }
    if (strcmp(key, "file_name") == 0) {
        return copy_value(config->file_name, sizeof config->file_name, value);
    }
    if (strcmp(key, "size") == 0) {
        char *end = NULL;
        const unsigned long long size = strtoull(value, &end, DECIMAL);
        const int ok = value[0] >= '0' && value[0] <= '9' && *end == '\0';
        config->size = ok ? size : config->size;
        return ok;
    }
    if (strcmp(key, "crc32") == 0) {
        char *end = NULL;
        const unsigned long long crc = strtoull(value, &end, HEXADECIMAL);
        config->has_crc32 = value[0] != '\0' && *end == '\0' && crc <= UINT32_MAX;
        config->crc32 = config->has_crc32 ? (uint32_t)crc : 0;
        return config->has_crc32 || value[0] == '\0';
    }
    if (strcmp(key, "runs") == 0) {
        const int ok = parse_bounded(value, 1, SKIFF_PROBE_RUNS_MAX, &number);
        config->runs = ok ? (int)number : config->runs;
        return ok;
    }
    if (strcmp(key, "clock_mhz") == 0) {
        const int ok =
            parse_bounded(value, SKIFF_PROBE_CLOCK_DEFAULT_MHZ, SKIFF_PROBE_CLOCK_FAST_MHZ,
                          &number) &&
            (number == SKIFF_PROBE_CLOCK_DEFAULT_MHZ || number == SKIFF_PROBE_CLOCK_FAST_MHZ);
        config->clock_mhz = ok ? (int)number : config->clock_mhz;
        return ok;
    }
    if (strcmp(key, "sections") == 0) {
        const unsigned sections =
            parse_names(value, SECTION_NAMES, sizeof SECTION_NAMES / sizeof SECTION_NAMES[0]);
        config->sections = sections != 0 ? sections : config->sections;
        return sections != 0;
    }
    if (strcmp(key, "scenarios") == 0) {
        const unsigned scenarios =
            parse_names(value, SCENARIO_NAMES, sizeof SCENARIO_NAMES / sizeof SCENARIO_NAMES[0]);
        config->scenarios = scenarios != 0 ? scenarios : config->scenarios;
        return scenarios != 0;
    }
    if (strcmp(key, "wait_s") == 0) {
        const int ok =
            parse_bounded(value, SKIFF_PROBE_WAIT_MIN_S, SKIFF_PROBE_WAIT_MAX_S, &number);
        config->wait_s = ok ? (int)number : config->wait_s;
        return ok;
    }
    if (strcmp(key, "awake_s") == 0) {
        const int ok =
            parse_bounded(value, SKIFF_PROBE_AWAKE_MIN_S, SKIFF_PROBE_AWAKE_MAX_S, &number);
        config->awake_s = ok ? (int)number : config->awake_s;
        return ok;
    }
    return 1;
}

void skiff_probe_config_read(FILE *file, skiff_probe_config *config) {
    char line[SKIFF_PROBE_LINE_MAX];
    while (fgets(line, sizeof line, file) != NULL) {
        trim_line(line);
        char *equals = strchr(line, '=');
        if (line[0] == '#' || equals == NULL) {
            continue;
        }
        *equals = '\0';
        if (!apply(config, line, equals + 1)) {
            config->invalid_values++;
        }
    }
}

int skiff_probe_config_complete(const skiff_probe_config *config) {
    return config->host[0] != '\0' && config->profile >= SKIFF_PROBE_DEFAULT_PROFILE &&
           config->token[0] != '\0' && config->rom_id[0] != '\0' && config->file_name[0] != '\0' &&
           config->size > 0 && config->has_crc32;
}

static int compare_long_long(const void *a, const void *b) {
    const long long left = *(const long long *)a;
    const long long right = *(const long long *)b;
    return (left > right) - (left < right);
}

long long skiff_probe_median(long long *values, size_t count) {
    if (count == 0) {
        return 0;
    }
    qsort(values, count, sizeof values[0], compare_long_long);
    if (count % 2 == 1) {
        return values[count / 2];
    }
    return (values[count / 2 - 1] + values[count / 2]) / 2;
}

unsigned long long skiff_probe_kb_per_s(unsigned long long bytes, long long elapsed_us) {
    if (elapsed_us <= 0) {
        return 0;
    }
    return bytes * US_PER_S / (unsigned long long)elapsed_us / BYTES_PER_KB;
}

uint32_t skiff_probe_crc32_bitwise(uint32_t crc, const unsigned char *data, size_t size) {
    crc = ~crc;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < BITS_PER_BYTE; bit++) {
            crc = (crc & 1U) != 0 ? (crc >> 1) ^ CRC32_POLYNOMIAL : crc >> 1;
        }
    }
    return ~crc;
}

static const uint32_t *crc32_table(void) {
    static uint32_t table[CRC32_TABLE_SIZE];
    static int built = 0;
    if (!built) {
        for (uint32_t byte = 0; byte < CRC32_TABLE_SIZE; byte++) {
            const unsigned char one = (unsigned char)byte;
            table[byte] = ~skiff_probe_crc32_bitwise(UINT32_MAX, &one, 1);
        }
        built = 1;
    }
    return table;
}

uint32_t skiff_probe_crc32_table(uint32_t crc, const unsigned char *data, size_t size) {
    const uint32_t *table = crc32_table();
    crc = ~crc;
    for (size_t i = 0; i < size; i++) {
        crc = table[(crc ^ data[i]) & BYTE_MASK] ^ (crc >> BITS_PER_BYTE);
    }
    return ~crc;
}
