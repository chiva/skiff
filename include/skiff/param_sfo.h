#ifndef SKIFF_PARAM_SFO_H
#define SKIFF_PARAM_SFO_H

/*
 * PARAM.SFO, the key/value file the PSP keeps in every save folder, game and PS1 classic (PBP),
 * read from bytes the caller loaded. A save folder on the Memory Stick or a downloaded file is
 * untrusted, so skiff_sfo_parse() checks every count and offset against the file before any value
 * is read.
 *
 * Layout, little-endian throughout: a header ("\0PSF", version 1.1, where the key table and the
 * data table start, how many entries), then one index entry per key (where its key starts in the
 * key table, its format, the bytes its value uses and reserves, where the value starts in the data
 * table). Keys are NUL-terminated ASCII; a value is UTF-8 text or a 32-bit number.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"

#define SKIFF_SFO_MAGIC "\0PSF"
#define SKIFF_SFO_MAGIC_BYTES 4
#define SKIFF_SFO_VERSION_1_1 0x00000101U
#define SKIFF_SFO_HEADER_BYTES 20
#define SKIFF_SFO_INDEX_ENTRY_BYTES 16
/* Real files hold up to about 20 keys. */
#define SKIFF_SFO_ENTRIES_MAX 256
/* The largest file the reader takes: a save's is about 4.5 KB, a game's under 2 KB. */
#define SKIFF_SFO_BYTES_MAX ((size_t)64 * 1024)
#define SKIFF_SFO_FILE_NAME "PARAM.SFO"

/* Every save's PARAM.SFO carries these keys, and a game-data install's carries neither. */
#define SKIFF_SFO_KEY_SAVEDATA_PARAMS "SAVEDATA_PARAMS"
#define SKIFF_SFO_KEY_SAVEDATA_FILE_LIST "SAVEDATA_FILE_LIST"

typedef enum skiff_sfo_format {
    /* UTF-8 without a terminator, or binary (SAVEDATA_PARAMS, SAVEDATA_FILE_LIST). */
    SKIFF_SFO_FORMAT_BYTES = 0x0004,
    /* UTF-8 ending in a NUL (TITLE, DISC_ID). */
    SKIFF_SFO_FORMAT_TEXT = 0x0204,
    /* A 32-bit number (PARENTAL_LEVEL, ATTRIBUTE). */
    SKIFF_SFO_FORMAT_NUMBER = 0x0404,
} skiff_sfo_format;

/* A parsed file: it points into the caller's bytes, which must outlive it. */
typedef struct skiff_sfo {
    const unsigned char *data;
    size_t size;
    uint32_t key_table;
    uint32_t data_table;
    uint32_t count;
} skiff_sfo;

typedef struct skiff_sfo_value {
    skiff_sfo_format format;
    /* The value's bytes, inside the parsed file. */
    const unsigned char *bytes;
    /* The bytes it uses; for TEXT, those before its first NUL. */
    size_t length;
    /* The value itself for NUMBER, 0 otherwise. */
    uint32_t number;
} skiff_sfo_value;

/*
 * Checks size bytes of data as a PARAM.SFO and fills *out. SKIFF_ERR_STORAGE_SAVE_DAMAGED unless:
 * the magic and version 1.1 lead; at most SKIFF_SFO_ENTRIES_MAX entries, whose index fits before
 * the key table, which starts before the data table, which starts inside the file; and each entry
 * has a non-empty key ending in a NUL inside the key table, a known format, a used length within
 * its reserved length, a value inside the data table, and 4 bytes for a NUMBER. A file over
 * SKIFF_SFO_BYTES_MAX is damaged too. SKIFF_ERR_INVALID_ARG for a NULL argument. *out is zeroed on
 * any error.
 */
skiff_err skiff_sfo_parse(const void *data, size_t size, skiff_sfo *out);

/* The value of key, matched exactly. SKIFF_ERR_STORAGE_NOT_FOUND when the file has no such key,
 * SKIFF_ERR_INVALID_ARG for a NULL argument. */
skiff_err skiff_sfo_find(const skiff_sfo *sfo, const char *key, skiff_sfo_value *out);

/* A TEXT or BYTES value copied into out, NUL-terminated. skiff_sfo_find()'s errors,
 * SKIFF_ERR_STORAGE_SAVE_DAMAGED for a NUMBER, SKIFF_ERR_BUFFER_TOO_SMALL when it does not fit; out
 * is empty on any error. */
skiff_err skiff_sfo_text(const skiff_sfo *sfo, const char *key, char *out, size_t out_size);

/* A NUMBER value. skiff_sfo_find()'s errors, SKIFF_ERR_STORAGE_SAVE_DAMAGED for another format;
 * *out is 0 on any error. */
skiff_err skiff_sfo_number(const skiff_sfo *sfo, const char *key, uint32_t *out);

/* Whether the file belongs to a save rather than a game-data install: it carries
 * SKIFF_SFO_KEY_SAVEDATA_PARAMS or SKIFF_SFO_KEY_SAVEDATA_FILE_LIST, whatever their values. 0 for
 * NULL. */
int skiff_sfo_is_save(const skiff_sfo *sfo);

#endif
