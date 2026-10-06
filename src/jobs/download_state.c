#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "skiff/download.h"

#define KEY_VERSION "version"
#define KEY_SIZE "size"
#define KEY_EXPECTED_CRC32 "crc32_expected"
#define KEY_ETAG "etag"
#define KEY_OFFSET "offset"
#define KEY_CRC32 "crc32"
#define KEY_CHECK "check"
#define LINE_FORMAT_HEX "%s=%08lx\n"
#define HEX_DIGITS 8
#define DECIMAL_BASE 10
#define HEX_BASE 16

/* The lines before "check=", in the order they are written. */
enum { LINE_VERSION, LINE_SIZE, LINE_EXPECTED_CRC32, LINE_ETAG, LINE_OFFSET, LINE_CRC32, LINES };

static const char *const KEYS[LINES] = {KEY_VERSION, KEY_SIZE,   KEY_EXPECTED_CRC32,
                                        KEY_ETAG,    KEY_OFFSET, KEY_CRC32};

static uint32_t crc32_of(const char *text, size_t length) {
    return (uint32_t)crc32(0L, (const Bytef *)text, (uInt)length);
}

/* Counts what snprintf() wrote at out + *used; 0 when it did not fit. */
static int advance(size_t out_size, size_t *used, int written) {
    if (written < 0 || (size_t)written >= out_size - *used) {
        return 0;
    }
    *used += (size_t)written;
    return 1;
}

skiff_err skiff_download_state_format(const skiff_download_state *state, char *out, size_t out_size,
                                      size_t *length) {
    if (state == NULL || out == NULL || length == NULL || out_size == 0 ||
        memchr(state->etag, '\0', sizeof state->etag) == NULL ||
        strpbrk(state->etag, "\r\n") != NULL || state->offset > state->size) {
        return SKIFF_ERR_INVALID_ARG;
    }
    char expected[HEX_DIGITS + 1] = "";
    if (state->has_expected_crc32) {
        snprintf(expected, sizeof expected, "%08lx", (unsigned long)state->expected_crc32);
    }
    size_t used = 0;
    int fits =
        advance(out_size, &used,
                snprintf(out, out_size, "%s=%d\n", KEY_VERSION, SKIFF_DOWNLOAD_STATE_VERSION));
    fits = fits && advance(out_size, &used,
                           snprintf(out + used, out_size - used, "%s=%llu\n", KEY_SIZE,
                                    (unsigned long long)state->size));
    fits = fits &&
           advance(out_size, &used,
                   snprintf(out + used, out_size - used, "%s=%s\n", KEY_EXPECTED_CRC32, expected));
    fits = fits && advance(out_size, &used,
                           snprintf(out + used, out_size - used, "%s=%s\n", KEY_ETAG, state->etag));
    fits = fits && advance(out_size, &used,
                           snprintf(out + used, out_size - used, "%s=%llu\n", KEY_OFFSET,
                                    (unsigned long long)state->offset));
    fits = fits && advance(out_size, &used,
                           snprintf(out + used, out_size - used, LINE_FORMAT_HEX, KEY_CRC32,
                                    (unsigned long)state->crc32));
    fits = fits && advance(out_size, &used,
                           snprintf(out + used, out_size - used, LINE_FORMAT_HEX, KEY_CHECK,
                                    (unsigned long)crc32_of(out, used)));
    if (!fits) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    *length = used;
    return SKIFF_OK;
}

/* A decimal number with nothing else around it: no sign, no blanks, no overflow. */
static int parse_decimal(const char *text, uint64_t *out) {
    if (text[0] < '0' || text[0] > '9') {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, DECIMAL_BASE);
    if (errno != 0 || *end != '\0') {
        return 0;
    }
    *out = (uint64_t)value;
    return 1;
}

/* Exactly eight hexadecimal digits. */
static int parse_hex32(const char *text, uint32_t *out) {
    if (strlen(text) != HEX_DIGITS || strspn(text, "0123456789abcdefABCDEF") != HEX_DIGITS) {
        return 0;
    }
    *out = (uint32_t)strtoul(text, NULL, HEX_BASE);
    return 1;
}

/* Splits the next "key=value\n" line off *cursor (in place); returns its value, or NULL when the
 * line is missing, unterminated or has another key. */
static char *next_value(char **cursor, const char *key) {
    char *line = *cursor;
    char *line_end = strchr(line, '\n');
    const size_t key_length = strlen(key);
    if (line_end == NULL || strncmp(line, key, key_length) != 0 || line[key_length] != '=') {
        return NULL;
    }
    *line_end = '\0';
    *cursor = line_end + 1;
    return line + key_length + 1;
}

static int parse_lines(char *text, skiff_download_state *state) {
    char *cursor = text;
    char *values[LINES];
    for (int i = 0; i < LINES; i++) {
        values[i] = next_value(&cursor, KEYS[i]);
        if (values[i] == NULL) {
            return 0;
        }
    }
    if (*cursor != '\0') {
        return 0; /* something between the last line and the check */
    }
    uint64_t version = 0;
    if (!parse_decimal(values[LINE_VERSION], &version) || version != SKIFF_DOWNLOAD_STATE_VERSION ||
        !parse_decimal(values[LINE_SIZE], &state->size) ||
        !parse_decimal(values[LINE_OFFSET], &state->offset) ||
        !parse_hex32(values[LINE_CRC32], &state->crc32) ||
        strlen(values[LINE_ETAG]) >= sizeof state->etag ||
        strpbrk(values[LINE_ETAG], "\r") != NULL || state->offset > state->size) {
        return 0;
    }
    state->has_expected_crc32 = values[LINE_EXPECTED_CRC32][0] != '\0';
    if (state->has_expected_crc32 &&
        !parse_hex32(values[LINE_EXPECTED_CRC32], &state->expected_crc32)) {
        return 0;
    }
    snprintf(state->etag, sizeof state->etag, "%s", values[LINE_ETAG]);
    return 1;
}

skiff_err skiff_download_state_parse(const char *text, size_t length, skiff_download_state *out) {
    if (text == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    char copy[SKIFF_DOWNLOAD_STATE_MAX + 1];
    if (length > SKIFF_DOWNLOAD_STATE_MAX || memchr(text, '\0', length) != NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    /* The check line is last and covers everything before it. */
    char *check_line = strstr(copy, "\n" KEY_CHECK "=");
    if (check_line == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    check_line++;
    const size_t covered = (size_t)(check_line - copy);
    char *cursor = check_line;
    const char *check_text = next_value(&cursor, KEY_CHECK);
    uint32_t check = 0;
    if (check_text == NULL || *cursor != '\0' || !parse_hex32(check_text, &check) ||
        check != crc32_of(copy, covered)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    copy[covered] = '\0';
    skiff_download_state parsed;
    memset(&parsed, 0, sizeof parsed);
    if (!parse_lines(copy, &parsed)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = parsed;
    return SKIFF_OK;
}
