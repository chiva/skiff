#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/install.h"

#define KEY_VERSION "version"
#define KEY_INSTALLED "installed"
#define KEY_ROM_ID "rom_id"
#define KEY_FILE_NAME "file_name"
#define KEY_PATH "path"
#define KEY_SIZE "size"
#define KEY_CRC32 "crc32"
#define KEY_INSTALLED_MS "installed_ms"
#define CRC32_FORMAT "%08lx"
#define CRC32_HEX_DIGITS 8
#define HEX_BASE 16
/* 2^53: cJSON reads numbers as doubles, exact for every whole number below it. */
#define JSON_INTEGER_LIMIT 9007199254740992.0
/* "\u0000" would cut a name short where C strings end; Skiff never writes it. */
#define JSON_ESCAPED_NUL "\\u0000"
/* Structural characters (':' ',' '[' '{') a manifest of SKIFF_INSTALL_RECORDS_MAX records holds
 * at most, with room to spare: about 16 per record. A text with more is not one Skiff wrote, and
 * is refused before cJSON builds a tree of it. */
#define JSON_TOKENS_MAX ((size_t)SKIFF_INSTALL_RECORDS_MAX * 24 + 64)

static const char *const LOGICAL_ROOTS[] = {SKIFF_STORAGE_ROOT_APP, SKIFF_STORAGE_ROOT_GAMES,
                                            SKIFF_STORAGE_ROOT_SAVES};

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

/* Paths compare as FAT names do: ASCII letters ignore case. */
static int same_path(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (ascii_lower(*a) != ascii_lower(*b)) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

/* "games:/Game.iso", never a device path: the manifest follows the device Skiff runs from. */
static int is_logical_path(const char *path) {
    for (size_t i = 0; i < sizeof LOGICAL_ROOTS / sizeof LOGICAL_ROOTS[0]; i++) {
        const size_t length = strlen(LOGICAL_ROOTS[i]);
        if (strncmp(path, LOGICAL_ROOTS[i], length) == 0 && path[length] == '/' &&
            path[length + 1] != '\0') {
            return 1;
        }
    }
    return 0;
}

static int fits(const char *text, size_t size) { return text[0] != '\0' && strlen(text) < size; }

static int record_valid(const skiff_install_record *record) {
    return record->rom_id <= SKIFF_INSTALL_ROM_ID_MAX &&
           record->size <= SKIFF_STORAGE_MAX_FILE_BYTES &&
           memchr(record->file_name, '\0', sizeof record->file_name) != NULL &&
           memchr(record->path, '\0', sizeof record->path) != NULL &&
           fits(record->file_name, sizeof record->file_name) &&
           fits(record->path, sizeof record->path) && is_logical_path(record->path) &&
           record->installed_ms >= 0 && (double)record->installed_ms < JSON_INTEGER_LIMIT;
}

/* ---- Finding records ---- */

static long index_of(const skiff_install_manifest *manifest, uint64_t rom_id,
                     const char *file_name) {
    for (size_t i = 0; i < manifest->count; i++) {
        const skiff_install_record *record = &manifest->records[i];
        if (record->rom_id == rom_id && strcmp(record->file_name, file_name) == 0) {
            return (long)i;
        }
    }
    return -1;
}

static long index_of_path(const skiff_install_manifest *manifest, const char *path) {
    for (size_t i = 0; i < manifest->count; i++) {
        if (same_path(manifest->records[i].path, path)) {
            return (long)i;
        }
    }
    return -1;
}

const skiff_install_record *skiff_install_manifest_find(const skiff_install_manifest *manifest,
                                                        uint64_t rom_id, const char *file_name) {
    if (manifest == NULL || file_name == NULL) {
        return NULL;
    }
    const long i = index_of(manifest, rom_id, file_name);
    return i < 0 ? NULL : &manifest->records[i];
}

const skiff_install_record *skiff_install_manifest_find_rom(const skiff_install_manifest *manifest,
                                                            uint64_t rom_id) {
    if (manifest == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < manifest->count; i++) {
        if (manifest->records[i].rom_id == rom_id) {
            return &manifest->records[i];
        }
    }
    return NULL;
}

const skiff_install_record *skiff_install_manifest_find_path(const skiff_install_manifest *manifest,
                                                             const char *path) {
    if (manifest == NULL || path == NULL) {
        return NULL;
    }
    const long i = index_of_path(manifest, path);
    return i < 0 ? NULL : &manifest->records[i];
}

/* ---- Changing records ---- */

static void remove_at(skiff_install_manifest *manifest, size_t index) {
    memmove(&manifest->records[index], &manifest->records[index + 1],
            (manifest->count - index - 1) * sizeof manifest->records[0]);
    manifest->count--;
    memset(&manifest->records[manifest->count], 0, sizeof manifest->records[0]);
}

skiff_err skiff_install_manifest_record(skiff_install_manifest *manifest,
                                        const skiff_install_record *record) {
    if (manifest == NULL || record == NULL || !record_valid(record)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const long own = index_of(manifest, record->rom_id, record->file_name);
    const long at_path = index_of_path(manifest, record->path);
    /* The slot this record takes: its own, or the one of the file it replaced at its path. */
    long slot = own >= 0 ? own : at_path;
    if (slot < 0) {
        if (manifest->count == SKIFF_INSTALL_RECORDS_MAX) {
            return SKIFF_ERR_BUFFER_TOO_SMALL;
        }
        slot = (long)manifest->count++;
    }
    manifest->records[slot] = *record;
    if (own >= 0 && at_path >= 0 && at_path != own) {
        remove_at(manifest, (size_t)at_path);
    }
    return SKIFF_OK;
}

int skiff_install_manifest_forget(skiff_install_manifest *manifest, uint64_t rom_id,
                                  const char *file_name) {
    if (manifest == NULL || file_name == NULL) {
        return 0;
    }
    const long i = index_of(manifest, rom_id, file_name);
    if (i < 0) {
        return 0;
    }
    remove_at(manifest, (size_t)i);
    return 1;
}

skiff_err skiff_install_manifest_reconcile(skiff_install_manifest *manifest, skiff_storage *storage,
                                           const skiff_storage_roots *roots, size_t *forgotten) {
    if (forgotten != NULL) {
        *forgotten = 0;
    }
    if (manifest == NULL || storage == NULL || roots == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    size_t i = 0;
    while (i < manifest->count) {
        char path[SKIFF_STORAGE_PATH_MAX];
        uint64_t size = 0;
        skiff_err err = skiff_storage_resolve(roots, manifest->records[i].path, path, sizeof path);
        if (err == SKIFF_OK) {
            err = skiff_storage_size(storage, path, &size);
        }
        if (err == SKIFF_OK && size == manifest->records[i].size) {
            i++;
            continue;
        }
        if (err != SKIFF_OK && err != SKIFF_ERR_STORAGE_NOT_FOUND && err != SKIFF_ERR_INVALID_ARG &&
            err != SKIFF_ERR_BUFFER_TOO_SMALL) {
            return err;
        }
        remove_at(manifest, i);
        if (forgotten != NULL) {
            (*forgotten)++;
        }
    }
    return SKIFF_OK;
}

/* ---- The file format ---- */

static cJSON *record_to_json(const skiff_install_record *record) {
    cJSON *item = cJSON_CreateObject();
    if (item == NULL || cJSON_AddNumberToObject(item, KEY_ROM_ID, (double)record->rom_id) == NULL ||
        cJSON_AddStringToObject(item, KEY_FILE_NAME, record->file_name) == NULL ||
        cJSON_AddStringToObject(item, KEY_PATH, record->path) == NULL ||
        cJSON_AddNumberToObject(item, KEY_SIZE, (double)record->size) == NULL ||
        cJSON_AddNumberToObject(item, KEY_INSTALLED_MS, (double)record->installed_ms) == NULL) {
        cJSON_Delete(item);
        return NULL;
    }
    if (record->has_crc32) {
        char crc[CRC32_HEX_DIGITS + 1];
        snprintf(crc, sizeof crc, CRC32_FORMAT, (unsigned long)record->crc32);
        if (cJSON_AddStringToObject(item, KEY_CRC32, crc) == NULL) {
            cJSON_Delete(item);
            return NULL;
        }
    }
    return item;
}

skiff_err skiff_install_manifest_format(const skiff_install_manifest *manifest, char *out,
                                        size_t out_size, size_t *length) {
    if (manifest == NULL || out == NULL || length == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *length = 0;
    cJSON *root = cJSON_CreateObject();
    cJSON *installed = root != NULL ? cJSON_AddArrayToObject(root, KEY_INSTALLED) : NULL;
    int built = installed != NULL &&
                cJSON_AddNumberToObject(root, KEY_VERSION, SKIFF_INSTALL_MANIFEST_VERSION) != NULL;
    for (size_t i = 0; built && i < manifest->count; i++) {
        cJSON *item = record_to_json(&manifest->records[i]);
        built = item != NULL && cJSON_AddItemToArray(installed, item);
        if (item != NULL && !built) {
            cJSON_Delete(item);
        }
    }
    char *text = built ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (text == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    const size_t text_length = strlen(text);
    skiff_err err = SKIFF_ERR_BUFFER_TOO_SMALL;
    if (text_length < out_size) {
        snprintf(out, out_size, "%s", text);
        *length = text_length;
        err = SKIFF_OK;
    }
    free(text);
    return err;
}

/* At most JSON_TOKENS_MAX structural characters outside strings, so cJSON's tree stays small. */
static int token_count_ok(const char *text, size_t length) {
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
    return tokens <= JSON_TOKENS_MAX;
}

static int contains(const char *text, size_t length, const char *needle) {
    const size_t needle_length = strlen(needle);
    for (size_t i = 0; i + needle_length <= length; i++) {
        if (memcmp(text + i, needle, needle_length) == 0) {
            return 1;
        }
    }
    return 0;
}

/* A whole number of a double: in range and exact. */
static int read_whole_number(const cJSON *object, const char *key, double limit, double *out) {
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

static int read_string(const cJSON *object, const char *key, char *out, size_t out_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL || !fits(item->valuestring, out_size)) {
        return 0;
    }
    snprintf(out, out_size, "%s", item->valuestring);
    return 1;
}

static int read_crc32(const cJSON *object, skiff_install_record *record) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, KEY_CRC32);
    if (item == NULL) {
        return 1;
    }
    const char *text = cJSON_IsString(item) ? item->valuestring : NULL;
    if (text == NULL || strlen(text) != CRC32_HEX_DIGITS ||
        strspn(text, "0123456789abcdefABCDEF") != CRC32_HEX_DIGITS) {
        return 0;
    }
    record->has_crc32 = 1;
    record->crc32 = (uint32_t)strtoul(text, NULL, HEX_BASE);
    return 1;
}

static int read_record(const cJSON *item, skiff_install_record *record) {
    double rom_id = 0.0;
    double size = 0.0;
    double installed_ms = 0.0;
    memset(record, 0, sizeof *record);
    if (!cJSON_IsObject(item) ||
        !read_whole_number(item, KEY_ROM_ID, (double)SKIFF_INSTALL_ROM_ID_MAX, &rom_id) ||
        !read_whole_number(item, KEY_SIZE, (double)SKIFF_STORAGE_MAX_FILE_BYTES, &size) ||
        !read_string(item, KEY_FILE_NAME, record->file_name, sizeof record->file_name) ||
        !read_string(item, KEY_PATH, record->path, sizeof record->path) ||
        !read_crc32(item, record)) {
        return 0;
    }
    if (cJSON_GetObjectItemCaseSensitive(item, KEY_INSTALLED_MS) != NULL &&
        !read_whole_number(item, KEY_INSTALLED_MS, JSON_INTEGER_LIMIT - 1.0, &installed_ms)) {
        return 0;
    }
    record->rom_id = (uint64_t)rom_id;
    record->size = (uint64_t)size;
    record->installed_ms = (int64_t)installed_ms;
    return record_valid(record);
}

static int fill(skiff_install_manifest *manifest, const cJSON *root) {
    double version = 0.0;
    const cJSON *installed = cJSON_GetObjectItemCaseSensitive(root, KEY_INSTALLED);
    if (!cJSON_IsObject(root) ||
        !read_whole_number(root, KEY_VERSION, JSON_INTEGER_LIMIT, &version) ||
        version != SKIFF_INSTALL_MANIFEST_VERSION || !cJSON_IsArray(installed) ||
        cJSON_GetArraySize(installed) > SKIFF_INSTALL_RECORDS_MAX) {
        return 0;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, installed) {
        skiff_install_record record;
        if (!read_record(item, &record) ||
            index_of(manifest, record.rom_id, record.file_name) >= 0 ||
            index_of_path(manifest, record.path) >= 0) {
            return 0;
        }
        manifest->records[manifest->count++] = record;
    }
    return 1;
}

static int only_blanks(const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n') {
            return 0;
        }
    }
    return 1;
}

skiff_err skiff_install_manifest_parse(skiff_install_manifest *manifest, const char *text,
                                       size_t length) {
    if (manifest == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    manifest->count = 0;
    memset(manifest->records, 0, sizeof manifest->records);
    if (text == NULL || length == 0 || length > SKIFF_INSTALL_MANIFEST_BYTES_MAX ||
        memchr(text, '\0', length) != NULL || contains(text, length, JSON_ESCAPED_NUL) ||
        !token_count_ok(text, length)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, length, &end, 0);
    const int ok = root != NULL && end != NULL && end >= text && end <= text + length &&
                   only_blanks(end, (size_t)(text + length - end)) && fill(manifest, root);
    cJSON_Delete(root);
    if (!ok) {
        manifest->count = 0;
        memset(manifest->records, 0, sizeof manifest->records);
        return SKIFF_ERR_INVALID_ARG;
    }
    return SKIFF_OK;
}

/* ---- Loading and saving ---- */

skiff_err skiff_install_manifest_create(skiff_install_manifest **out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = calloc(1, sizeof **out);
    return *out != NULL ? SKIFF_OK : SKIFF_ERR_NO_MEMORY;
}

void skiff_install_manifest_destroy(skiff_install_manifest *manifest) { free(manifest); }

skiff_err skiff_install_manifest_load(skiff_install_manifest *manifest, skiff_storage *storage,
                                      const char *path, skiff_install_load_result *result) {
    if (result != NULL) {
        *result = SKIFF_INSTALL_NO_MANIFEST;
    }
    if (manifest == NULL || storage == NULL || path == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    manifest->count = 0;
    memset(manifest->records, 0, sizeof manifest->records);
    char *text = malloc(SKIFF_INSTALL_MANIFEST_BYTES_MAX);
    if (text == NULL) {
        manifest->load_error = SKIFF_ERR_NO_MEMORY;
        return SKIFF_ERR_NO_MEMORY;
    }
    size_t length = 0;
    const skiff_err err =
        skiff_storage_read_whole(storage, path, text, SKIFF_INSTALL_MANIFEST_BYTES_MAX, &length);
    skiff_install_load_result found = SKIFF_INSTALL_NO_MANIFEST;
    if (err == SKIFF_ERR_BUFFER_TOO_SMALL) {
        found = SKIFF_INSTALL_DAMAGED;
    } else if (err == SKIFF_OK && length > 0) {
        found = skiff_install_manifest_parse(manifest, text, length) == SKIFF_OK
                    ? SKIFF_INSTALL_LOADED
                    : SKIFF_INSTALL_DAMAGED;
    }
    free(text);
    if (err != SKIFF_OK && err != SKIFF_ERR_BUFFER_TOO_SMALL) {
        manifest->load_error = err;
        return err;
    }
    manifest->load_error = SKIFF_OK;
    if (result != NULL) {
        *result = found;
    }
    return SKIFF_OK;
}

skiff_err skiff_install_manifest_save(const skiff_install_manifest *manifest,
                                      skiff_storage *storage, const char *path) {
    if (manifest == NULL || storage == NULL || path == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (manifest->load_error != SKIFF_OK) {
        return manifest->load_error;
    }
    char *text = malloc(SKIFF_INSTALL_MANIFEST_BYTES_MAX);
    if (text == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    size_t length = 0;
    skiff_err err =
        skiff_install_manifest_format(manifest, text, SKIFF_INSTALL_MANIFEST_BYTES_MAX, &length);
    if (err == SKIFF_OK) {
        err = skiff_storage_replace_whole(storage, path, text, length);
    }
    free(text);
    return err;
}
