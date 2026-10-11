#ifndef SKIFF_SAVES_INTERNAL_H
#define SKIFF_SAVES_INTERNAL_H

/*
 * What the saves/ files share: little-endian fields, and the zip format (PKWARE's APPNOTE.TXT) a
 * unit is written and read in. Field offsets are from the start of each record.
 */

#include <cjson/cJSON.h>
#include <stddef.h>
#include <stdint.h>

#include "skiff/saves.h"

#define SAVES_BITS_PER_BYTE 8
#define SAVES_BYTE_MASK 0xFFU

/* Records and their fixed parts (APPNOTE 4.3.7, 4.3.12, 4.3.16, 4.3.9). */
#define ZIP_LOCAL_SIGNATURE 0x04034b50U
#define ZIP_CENTRAL_SIGNATURE 0x02014b50U
#define ZIP_END_SIGNATURE 0x06054b50U
#define ZIP_DESCRIPTOR_SIGNATURE 0x08074b50U
#define ZIP_LOCAL_BYTES 30
#define ZIP_CENTRAL_BYTES 46
#define ZIP_END_BYTES 22
#define ZIP_SIGNATURE_BYTES 4
/* A data descriptor: CRC-32 and sizes, after an optional signature (APPNOTE 4.3.9). */
#define ZIP_DESCRIPTOR_BYTES 12
#define ZIP_DESCRIPTOR_SIGNED_BYTES (ZIP_SIGNATURE_BYTES + ZIP_DESCRIPTOR_BYTES)
#define ZIP_DESCRIPTOR_CRC 0
#define ZIP_DESCRIPTOR_COMPRESSED 4
#define ZIP_DESCRIPTOR_SIZE 8

/* Local file header fields. */
#define ZIP_LOCAL_VERSION 4
#define ZIP_LOCAL_FLAGS 6
#define ZIP_LOCAL_METHOD 8
#define ZIP_LOCAL_TIME 10
#define ZIP_LOCAL_DATE 12
#define ZIP_LOCAL_CRC 14
#define ZIP_LOCAL_COMPRESSED 18
#define ZIP_LOCAL_SIZE 22
#define ZIP_LOCAL_NAME_LENGTH 26
#define ZIP_LOCAL_EXTRA_LENGTH 28

/* Central directory header fields. */
#define ZIP_CENTRAL_MADE_BY 4
#define ZIP_CENTRAL_VERSION 6
#define ZIP_CENTRAL_FLAGS 8
#define ZIP_CENTRAL_METHOD 10
#define ZIP_CENTRAL_TIME 12
#define ZIP_CENTRAL_DATE 14
#define ZIP_CENTRAL_CRC 16
#define ZIP_CENTRAL_COMPRESSED 20
#define ZIP_CENTRAL_SIZE 24
#define ZIP_CENTRAL_NAME_LENGTH 28
#define ZIP_CENTRAL_EXTRA_LENGTH 30
#define ZIP_CENTRAL_COMMENT_LENGTH 32
#define ZIP_CENTRAL_DISK 34
#define ZIP_CENTRAL_INTERNAL 36
#define ZIP_CENTRAL_EXTERNAL 38
#define ZIP_CENTRAL_OFFSET 42

/* End of central directory fields. */
#define ZIP_END_DISK 4
#define ZIP_END_DIRECTORY_DISK 6
#define ZIP_END_DISK_ENTRIES 8
#define ZIP_END_ENTRIES 10
#define ZIP_END_DIRECTORY_SIZE 12
#define ZIP_END_DIRECTORY_OFFSET 16
#define ZIP_END_COMMENT_LENGTH 20

#define ZIP_METHOD_STORED 0
#define ZIP_METHOD_DEFLATE 8
/* "Version 2.0", the lowest that has folders and deflate; made by MS-DOS (high byte 0). */
#define ZIP_VERSION_20 20
/* MS-DOS date 1980-01-01 (day 1, month 1, year 0 from 1980) at time 00:00:00. */
#define ZIP_DOS_DATE_1980_01_01 0x0021U
#define ZIP_DOS_TIME_MIDNIGHT 0
#define ZIP_FLAG_ENCRYPTED 0x0001U
#define ZIP_FLAG_DESCRIPTOR 0x0008U
#define ZIP_FLAG_STRONG_ENCRYPTION 0x0040U
#define ZIP_FLAG_ENCRYPTED_DIRECTORY 0x2000U
/* What a field holds when the real value is in a ZIP64 record (APPNOTE 4.4.8). */
#define ZIP_ZIP64_U16 0xFFFFU
#define ZIP_ZIP64_U32 0xFFFFFFFFU
/* The extra field that carries ZIP64 sizes (APPNOTE 4.5.3), and every extra field's header. */
#define ZIP_EXTRA_ZIP64 0x0001U
#define ZIP_EXTRA_HEADER_BYTES 4
#define ZIP_EXTRA_ID 0
#define ZIP_EXTRA_LENGTH 2

#define SAVES_PATH_SEPARATOR '/'
#define SAVES_LINE_SEPARATOR "\n"
#define SAVES_HASH_SEPARATOR ":"
/* Streams files through a heap buffer this large: the PSP's browsing thread has a 32 KB stack. */
#define SAVES_BUFFER_BYTES ((size_t)16 * 1024)

static inline uint16_t saves_read_le16(const unsigned char *bytes) {
    return (uint16_t)(bytes[0] | bytes[1] << SAVES_BITS_PER_BYTE);
}

static inline uint32_t saves_read_le32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << SAVES_BITS_PER_BYTE |
           (uint32_t)bytes[2] << (2 * SAVES_BITS_PER_BYTE) |
           (uint32_t)bytes[3] << (3 * SAVES_BITS_PER_BYTE);
}

static inline void saves_write_le16(unsigned char *bytes, uint16_t value) {
    bytes[0] = (unsigned char)(value & SAVES_BYTE_MASK);
    bytes[1] = (unsigned char)(value >> SAVES_BITS_PER_BYTE);
}

static inline void saves_write_le32(unsigned char *bytes, uint32_t value) {
    for (size_t i = 0; i < sizeof value; i++) {
        bytes[i] = (unsigned char)((value >> (i * SAVES_BITS_PER_BYTE)) & SAVES_BYTE_MASK);
    }
}

/* ASCII case folding, whatever the C library's locale. */
static inline unsigned char saves_fold(unsigned char c) {
    return c >= 'a' && c <= 'z' ? (unsigned char)(c - 'a' + 'A') : c;
}

/* Whether a and b (a_length and b_length bytes) are the same name to FAT, which ignores case. */
int saves_same_name(const char *a, size_t a_length, const char *b, size_t b_length);

/* Whether name starts with save_target, ignoring case. */
int saves_has_prefix(const char *name, const char *save_target);

/* Whether the first length bytes of name are a valid name, as skiff_saves_name_valid(). */
int saves_name_valid(const char *name, size_t length);

/* "<root>/<a>" or "<root>/<a>/<b>" (b may be NULL) resolved to a device path into out. */
skiff_err saves_resolve(const skiff_storage_roots *roots, const char *root, const char *a,
                        const char *b, char *out, size_t out_size);

/* Fills unit->content_hash, RomM's hash, from each file's md5: MD5 of "<name>:<md5>" lines taken
 * in the order of the names (RomM sorts the names, then joins the lines), joined by "\n", without a
 * final newline. unit->files must be sorted by name already. */
void saves_content_hash(skiff_saves_unit *unit);

/* Sorts unit's folders and files by name, byte by byte (the order of a unit's entries). */
void saves_sort(skiff_saves_unit *unit);

/* Records why a unit or folder was refused, and the name at fault. */
void saves_refuse(skiff_saves_unit *unit, skiff_saves_problem problem, const char *name,
                  size_t length);

/* ---- Backups (backups.c), for the restore ---- */

/* Writes live (collected from "saves:") as the next backup, numbered one above the highest kept,
 * into *number (0 on any error, which leaves no backup file behind). */
skiff_err saves_backup_write(skiff_storage *storage, const skiff_storage_roots *roots,
                             skiff_saves_unit *live, uint32_t *number);

/* Drops what the backup caps no longer allow (skiff_saves_backups_list()), never backup keep, and
 * drafts a cut backup left. Tidying only: a failure leaves more backups, never fewer. */
void saves_backups_prune(skiff_storage *storage, const skiff_storage_roots *roots, uint32_t keep);

/* ---- JSON state files (the restore journal, saves.json) ---- */

/* 2^53: cJSON reads numbers as doubles, exact for every whole number below it. */
#define SAVES_JSON_INTEGER_LIMIT 9007199254740992.0

/* Whether hash is RomM's content_hash form: SKIFF_MD5_HEX_SIZE - 1 lowercase hex digits. */
int saves_hash_valid(const char *hash);

/* The whole number at key in object, from 0 to limit and exact, into *out; 0 when it is missing,
 * not a number, or not such a number. */
int saves_json_number(const cJSON *object, const char *key, double limit, double *out);

/* The string at key, which must fit out_size with its terminator; empty allowed only with
 * allow_empty. 0 otherwise. */
int saves_json_string(const cJSON *object, const char *key, int allow_empty, char *out,
                      size_t out_size);

/*
 * Parses length bytes of text (no NUL in them) as one JSON value with nothing but blanks after it,
 * into *out (free it with cJSON_Delete()). A text with more than tokens_max structural characters
 * (':' ',' '[' '{') outside strings is refused before cJSON builds a tree of it, and the memory
 * cJSON can need for a text that size is checked first, since cJSON reports a failed allocation as
 * a parse error. SKIFF_ERR_INVALID_ARG for a text that is not such JSON, SKIFF_ERR_NO_MEMORY.
 */
skiff_err saves_json_parse(const char *text, size_t length, size_t tokens_max, cJSON **out);

/* Prints root unformatted into out (terminated), its length in *length. SKIFF_ERR_NO_MEMORY,
 * SKIFF_ERR_BUFFER_TOO_SMALL. root is not freed. */
skiff_err saves_json_print(const cJSON *root, char *out, size_t out_size, size_t *length);

#endif
