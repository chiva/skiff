#include "skiff/param_sfo.h"

#include <string.h>

#include "saves_internal.h"

/* Header fields. */
#define HEADER_VERSION 4
#define HEADER_KEY_TABLE 8
#define HEADER_DATA_TABLE 12
#define HEADER_COUNT 16
/* Index entry fields. */
#define ENTRY_KEY_OFFSET 0
#define ENTRY_FORMAT 2
#define ENTRY_LENGTH 4
#define ENTRY_RESERVED 8
#define ENTRY_DATA_OFFSET 12
#define NUMBER_BYTES 4

typedef struct sfo_entry {
    const char *key;
    size_t key_length;
    uint16_t format;
    uint32_t length;
    uint32_t reserved;
    uint32_t data_offset;
} sfo_entry;

static const unsigned char *index_entry(const skiff_sfo *sfo, uint32_t i) {
    return sfo->data + SKIFF_SFO_HEADER_BYTES + (size_t)i * SKIFF_SFO_INDEX_ENTRY_BYTES;
}

static int known_format(uint16_t format) {
    return format == SKIFF_SFO_FORMAT_BYTES || format == SKIFF_SFO_FORMAT_TEXT ||
           format == SKIFF_SFO_FORMAT_NUMBER;
}

/* Reads entry i and checks it lies inside its tables; 0 when it does not. */
static int read_entry(const skiff_sfo *sfo, uint32_t i, sfo_entry *out) {
    const unsigned char *entry = index_entry(sfo, i);
    const size_t key_table_bytes = (size_t)sfo->data_table - sfo->key_table;
    const size_t data_table_bytes = sfo->size - sfo->data_table;
    const size_t key_offset = saves_read_le16(entry + ENTRY_KEY_OFFSET);
    out->format = saves_read_le16(entry + ENTRY_FORMAT);
    out->length = saves_read_le32(entry + ENTRY_LENGTH);
    out->reserved = saves_read_le32(entry + ENTRY_RESERVED);
    out->data_offset = saves_read_le32(entry + ENTRY_DATA_OFFSET);
    if (key_offset >= key_table_bytes || !known_format(out->format) ||
        out->length > out->reserved || out->data_offset > data_table_bytes ||
        out->reserved > data_table_bytes - out->data_offset ||
        (out->format == SKIFF_SFO_FORMAT_NUMBER && out->length != NUMBER_BYTES)) {
        return 0;
    }
    out->key = (const char *)sfo->data + sfo->key_table + key_offset;
    const char *end = memchr(out->key, '\0', key_table_bytes - key_offset);
    if (end == NULL || end == out->key) {
        return 0;
    }
    out->key_length = (size_t)(end - out->key);
    return 1;
}

skiff_err skiff_sfo_parse(const void *data, size_t size, skiff_sfo *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (data == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const unsigned char *bytes = data;
    if (size < SKIFF_SFO_HEADER_BYTES || size > SKIFF_SFO_BYTES_MAX ||
        memcmp(bytes, SKIFF_SFO_MAGIC, SKIFF_SFO_MAGIC_BYTES) != 0 ||
        saves_read_le32(bytes + HEADER_VERSION) != SKIFF_SFO_VERSION_1_1) {
        return SKIFF_ERR_STORAGE_SAVE_DAMAGED;
    }
    skiff_sfo sfo = {bytes, size, saves_read_le32(bytes + HEADER_KEY_TABLE),
                     saves_read_le32(bytes + HEADER_DATA_TABLE),
                     saves_read_le32(bytes + HEADER_COUNT)};
    if (sfo.count > SKIFF_SFO_ENTRIES_MAX ||
        SKIFF_SFO_HEADER_BYTES + (size_t)sfo.count * SKIFF_SFO_INDEX_ENTRY_BYTES > sfo.key_table ||
        sfo.key_table > sfo.data_table || sfo.data_table > size) {
        return SKIFF_ERR_STORAGE_SAVE_DAMAGED;
    }
    for (uint32_t i = 0; i < sfo.count; i++) {
        sfo_entry entry;
        if (!read_entry(&sfo, i, &entry)) {
            return SKIFF_ERR_STORAGE_SAVE_DAMAGED;
        }
    }
    *out = sfo;
    return SKIFF_OK;
}

skiff_err skiff_sfo_find(const skiff_sfo *sfo, const char *key, skiff_sfo_value *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (sfo == NULL || sfo->data == NULL || key == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const size_t key_length = strlen(key);
    for (uint32_t i = 0; i < sfo->count; i++) {
        sfo_entry entry;
        if (!read_entry(sfo, i, &entry)) {
            return SKIFF_ERR_STORAGE_SAVE_DAMAGED;
        }
        if (entry.key_length != key_length || memcmp(entry.key, key, key_length) != 0) {
            continue;
        }
        out->format = (skiff_sfo_format)entry.format;
        out->bytes = sfo->data + sfo->data_table + entry.data_offset;
        out->length = entry.length;
        if (entry.format == SKIFF_SFO_FORMAT_TEXT) {
            const unsigned char *end = memchr(out->bytes, '\0', entry.length);
            out->length = end != NULL ? (size_t)(end - out->bytes) : entry.length;
        } else if (entry.format == SKIFF_SFO_FORMAT_NUMBER) {
            out->number = saves_read_le32(out->bytes);
        }
        return SKIFF_OK;
    }
    return SKIFF_ERR_STORAGE_NOT_FOUND;
}

skiff_err skiff_sfo_text(const skiff_sfo *sfo, const char *key, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return SKIFF_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    skiff_sfo_value value;
    const skiff_err err = skiff_sfo_find(sfo, key, &value);
    if (err != SKIFF_OK) {
        return err;
    }
    if (value.format == SKIFF_SFO_FORMAT_NUMBER) {
        return SKIFF_ERR_STORAGE_SAVE_DAMAGED;
    }
    if (value.length >= out_size) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(out, value.bytes, value.length);
    out[value.length] = '\0';
    return SKIFF_OK;
}

skiff_err skiff_sfo_number(const skiff_sfo *sfo, const char *key, uint32_t *out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = 0;
    skiff_sfo_value value;
    const skiff_err err = skiff_sfo_find(sfo, key, &value);
    if (err != SKIFF_OK) {
        return err;
    }
    if (value.format != SKIFF_SFO_FORMAT_NUMBER) {
        return SKIFF_ERR_STORAGE_SAVE_DAMAGED;
    }
    *out = value.number;
    return SKIFF_OK;
}

int skiff_sfo_is_save(const skiff_sfo *sfo) {
    skiff_sfo_value value;
    return skiff_sfo_find(sfo, SKIFF_SFO_KEY_SAVEDATA_PARAMS, &value) == SKIFF_OK ||
           skiff_sfo_find(sfo, SKIFF_SFO_KEY_SAVEDATA_FILE_LIST, &value) == SKIFF_OK;
}
