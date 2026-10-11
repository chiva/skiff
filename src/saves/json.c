/*
 * JSON for the saves layer's state files, the restore journal and saves.json: both are Skiff's own,
 * written crash-safely and read back tolerantly, so a damaged file is refused whole rather than
 * half read.
 */
#include <stdlib.h>
#include <string.h>

#include "saves_internal.h"

/* What cJSON can need for a text of `length` bytes within `tokens` structural characters: a node
 * per value and per key, plus copies of the strings. */
#define PARSE_HEADROOM_BYTES(length, tokens) ((2 * (tokens) + 2) * sizeof(cJSON) + 2 * (length))

int saves_hash_valid(const char *hash) {
    if (hash == NULL) {
        return 0;
    }
    size_t length = 0;
    for (; hash[length] != '\0'; length++) {
        const char c = hash[length];
        if (length >= SKIFF_MD5_HEX_SIZE - 1 ||
            !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return length == SKIFF_MD5_HEX_SIZE - 1;
}

int saves_json_number(const cJSON *object, const char *key, double limit, double *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) {
        return 0;
    }
    const double value = item->valuedouble;
    if (!(value >= 0.0 && value <= limit) || value != (double)(uint64_t)value) {
        return 0;
    }
    *out = value;
    return 1;
}

int saves_json_string(const cJSON *object, const char *key, int allow_empty, char *out,
                      size_t out_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return 0;
    }
    const size_t length = strlen(item->valuestring);
    if (length >= out_size || (length == 0 && !allow_empty)) {
        return 0;
    }
    memcpy(out, item->valuestring, length + 1);
    return 1;
}

/* Structural characters outside strings, escapes followed one at a time. */
static size_t count_tokens(const char *text, size_t length) {
    size_t tokens = 0;
    int in_string = 0;
    for (size_t i = 0; i < length; i++) {
        const char c = text[i];
        if (in_string) {
            if (c == '\\') {
                i++;
            } else if (c == '"') {
                in_string = 0;
            }
        } else if (c == '"') {
            in_string = 1;
        } else if (c == ':' || c == ',' || c == '[' || c == '{') {
            tokens++;
        }
    }
    return tokens;
}

static int only_blanks(const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n') {
            return 0;
        }
    }
    return 1;
}

skiff_err saves_json_parse(const char *text, size_t length, size_t tokens_max, cJSON **out) {
    *out = NULL;
    if (text == NULL || length == 0 || memchr(text, '\0', length) != NULL ||
        count_tokens(text, length) > tokens_max) {
        return SKIFF_ERR_INVALID_ARG;
    }
    void *headroom = malloc(PARSE_HEADROOM_BYTES(length, tokens_max));
    if (headroom == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    free(headroom);
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, length, &end, 0);
    if (root == NULL || end == NULL || end < text || end > text + length ||
        !only_blanks(end, (size_t)(text + length - end))) {
        cJSON_Delete(root);
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = root;
    return SKIFF_OK;
}

skiff_err saves_json_print(const cJSON *root, char *out, size_t out_size, size_t *length) {
    *length = 0;
    char *text = cJSON_PrintUnformatted(root);
    if (text == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    const size_t text_length = strlen(text);
    skiff_err err = SKIFF_ERR_BUFFER_TOO_SMALL;
    if (text_length < out_size) {
        memcpy(out, text, text_length + 1);
        *length = text_length;
        err = SKIFF_OK;
    }
    free(text);
    return err;
}
