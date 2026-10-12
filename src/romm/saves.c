#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/http.h"
#include "skiff/log.h"
#include "skiff/romm_saves.h"

#include "romm_internal.h"

#define PATH_NEGOTIATE "/api/sync/negotiate"
#define PATH_SAVES "/api/saves"
#define PATH_SESSIONS "/api/sync/sessions"
#define CONTENT_TYPE_ZIP "application/zip"
#define CONTENT_TYPE_MULTIPART "multipart/form-data; boundary="
/* The multipart boundary: this prefix and the file's content hash. A zip's entries cannot hold it
 * (they would have to contain the MD5 of themselves), but its comment could: the file is checked
 * for it as it is sent. */
#define BOUNDARY_PREFIX "skiff-"
#define BOUNDARY_MAX (sizeof BOUNDARY_PREFIX + SKIFF_ROMM_CONTENT_HASH_LENGTH)
/* What opens the delimiter of a part: a file that holds "--" and the boundary would end its own
 * part early. */
#define DELIMITER_DASHES "--"
#define DELIMITER_MAX (sizeof DELIMITER_DASHES + BOUNDARY_MAX)
#define MULTIPART_FIELD "saveFile"
/* The head: the boundary line, the part's headers and a blank line; the tail ends the body. */
#define MULTIPART_HEAD_FORMAT                                                                      \
    "--%s\r\nContent-Disposition: form-data; name=\"" MULTIPART_FIELD "\"; filename=\"%s\"\r\n"    \
    "Content-Type: " CONTENT_TYPE_ZIP "\r\n\r\n"
#define MULTIPART_TAIL_FORMAT "\r\n--%s--\r\n"
#define MULTIPART_HEAD_MAX (sizeof MULTIPART_HEAD_FORMAT + BOUNDARY_MAX + SKIFF_ROMM_FILE_NAME_MAX)
#define MULTIPART_TAIL_MAX (sizeof MULTIPART_TAIL_FORMAT + BOUNDARY_MAX)
#define CONTENT_TYPE_MAX (sizeof CONTENT_TYPE_MULTIPART + BOUNDARY_MAX)
/* What RomM's file and folder names may not hold (its sanitize_filename turns them into '-' or
 * drops them, so the name RomM keeps would differ). */
#define EMULATOR_FORBIDDEN "/\\:|*?\"<>"
#define FILE_NAME_FORBIDDEN "/\\\""
#define HEX_DIGITS_LOWER "0123456789abcdef"
#define ASCII_FIRST_PRINTABLE ' '
#define ASCII_LAST_PRINTABLE '~'
/* "YYYY-MM-DD HH:MM:SS.mmmZ" from skiff_log_format_timestamp(): ISO 8601 has a 'T' where the date
 * ends. */
#define ISO_DATE_LENGTH 10
#define ISO_TIME_SEPARATOR 'T'
/* 2^53: ids and sizes travel as JSON numbers, which RomM and cJSON read as doubles. */
#define JSON_INTEGER_LIMIT 9007199254740992ULL
#define AUTOCLEANUP_LIMIT_PARAMETER "&autocleanup_limit="
/* The decimal digits of a uint32_t. */
#define UINT32_DIGITS_MAX 10
#define FIELD_DEVICE_ID "device_id"
#define FIELD_CONTENT_HASH "content_hash"

enum {
    HTTP_STATUS_OK = 200,
    HTTP_STATUS_CONFLICT = 409,
    HTTP_STATUS_PAYLOAD_TOO_LARGE = 413,
};

/* ---- Values Skiff sends ---- */

static int is_printable_ascii(const char *text) {
    for (const char *c = text; *c != '\0'; c++) {
        const unsigned char byte = (unsigned char)*c;
        if (byte < (unsigned char)ASCII_FIRST_PRINTABLE ||
            byte > (unsigned char)ASCII_LAST_PRINTABLE) {
            return 0;
        }
    }
    return 1;
}

static int is_dot_name(const char *text) {
    return strcmp(text, ".") == 0 || strcmp(text, "..") == 0;
}

/* Non-empty, shorter than max, and nothing RomM would change. */
static int text_fits(const char *text, size_t max) {
    return text != NULL && text[0] != '\0' && strlen(text) < max;
}

int skiff_romm_device_id_valid(const char *device_id) {
    return text_fits(device_id, SKIFF_CONFIG_DEVICE_ID_MAX) && is_printable_ascii(device_id) &&
           strchr(device_id, ' ') == NULL;
}

/* One folder name RomM keeps as it is (its emulator folder). */
static int emulator_valid(const char *emulator) {
    if (!text_fits(emulator, SKIFF_ROMM_EMULATOR_MAX) || !is_printable_ascii(emulator) ||
        strpbrk(emulator, EMULATOR_FORBIDDEN) != NULL || is_dot_name(emulator)) {
        return 0;
    }
    return emulator[0] != ' ' && emulator[strlen(emulator) - 1] != ' ';
}

static int slot_valid(const char *slot) {
    return text_fits(slot, SKIFF_ROMM_SLOT_MAX) && skiff_romm_is_printable_text(slot);
}

static int content_hash_valid(const char *hash) {
    return hash != NULL && strlen(hash) == SKIFF_ROMM_CONTENT_HASH_LENGTH &&
           strspn(hash, HEX_DIGITS_LOWER) == SKIFF_ROMM_CONTENT_HASH_LENGTH;
}

/* A name that goes into the multipart head between quotes and that RomM keeps as it is. */
static int file_name_valid(const char *name) {
    return text_fits(name, SKIFF_ROMM_FILE_NAME_MAX) && is_printable_ascii(name) &&
           strpbrk(name, FILE_NAME_FORBIDDEN) == NULL && !is_dot_name(name);
}

/* An id RomM gives out: from 1, below 2^53. */
static int id_valid(uint64_t id) { return id > 0 && id < JSON_INTEGER_LIMIT; }

static int client_ready(const skiff_romm_client *client) {
    return client != NULL && client->transport != NULL && client->base_url[0] != '\0';
}

/* ---- Building paths ---- */

/* Appends plain text (no encoding) to the path in out; 0 when it does not fit. */
static int append_plain(char *out, size_t out_size, const char *text) {
    const size_t used = strlen(out);
    const size_t length = strlen(text);
    if (length >= out_size - used) {
        return 0;
    }
    memcpy(out + used, text, length + 1);
    return 1;
}

/* Appends "<name><value>" with the value percent-encoded; 0 when it does not fit. */
static int append_parameter(char *out, size_t out_size, const char *name, const char *value) {
    return append_plain(out, out_size, name) &&
           skiff_romm_append_encoded(out, out_size, value) == SKIFF_OK;
}

/* ---- Fields of RomM's answers ---- */

static int read_hash(const cJSON *object, const char *name, char *out) {
    return skiff_romm_read_optional_text(object, name, out, SKIFF_ROMM_CONTENT_HASH_MAX) &&
           (out[0] == '\0' || content_hash_valid(out));
}

/* A slot: null for an archival save, otherwise a string. */
static int read_slot(const cJSON *object, int *has_slot, char *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "slot");
    *has_slot = item != NULL && !cJSON_IsNull(item);
    return skiff_romm_read_optional_text(object, "slot", out, SKIFF_ROMM_SLOT_MAX);
}

/* A save id: null (none) or a count. */
static int read_save_id(const cJSON *object, int *has_save_id, uint64_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "save_id");
    *has_save_id = 0;
    if (item == NULL || cJSON_IsNull(item)) {
        return 1;
    }
    *has_save_id = skiff_romm_read_count(object, "save_id", out);
    return *has_save_id;
}

static const char *const ACTION_NAMES[] = {
    [SKIFF_ROMM_SYNC_UPLOAD] = "upload",     [SKIFF_ROMM_SYNC_DOWNLOAD] = "download",
    [SKIFF_ROMM_SYNC_CONFLICT] = "conflict", [SKIFF_ROMM_SYNC_NO_OP] = "no_op",
    [SKIFF_ROMM_SYNC_DELETE] = "delete",
};

static int read_action(const cJSON *object, skiff_romm_sync_action *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "action");
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return 0;
    }
    for (size_t i = 0; i < sizeof ACTION_NAMES / sizeof ACTION_NAMES[0]; i++) {
        if (strcmp(item->valuestring, ACTION_NAMES[i]) == 0) {
            *out = (skiff_romm_sync_action)i;
            return 1;
        }
    }
    return 0;
}

static int parse_operation(const cJSON *item, skiff_romm_sync_operation *out) {
    memset(out, 0, sizeof *out);
    if (!cJSON_IsObject(item) || !read_action(item, &out->action) ||
        !skiff_romm_read_count(item, "rom_id", &out->rom_id) ||
        !read_save_id(item, &out->has_save_id, &out->save_id) ||
        !skiff_romm_read_text(item, "file_name", out->file_name, sizeof out->file_name) ||
        !read_slot(item, &out->has_slot, out->slot) ||
        !skiff_romm_read_optional_text(item, "emulator", out->emulator, sizeof out->emulator) ||
        !read_hash(item, "server_content_hash", out->server_content_hash) ||
        !skiff_romm_read_optional_text(item, "server_updated_at", out->server_updated_at,
                                       sizeof out->server_updated_at)) {
        return 0;
    }
    /* Both name the server save Skiff would fetch. */
    const int needs_save =
        out->action == SKIFF_ROMM_SYNC_DOWNLOAD || out->action == SKIFF_ROMM_SYNC_CONFLICT;
    return !needs_save || out->has_save_id;
}

static int fill_plan(const cJSON *root, skiff_romm_sync_plan *out) {
    const cJSON *operations = cJSON_GetObjectItemCaseSensitive(root, "operations");
    if (!cJSON_IsObject(root) || !skiff_romm_read_count(root, "session_id", &out->session_id) ||
        !cJSON_IsArray(operations)) {
        return 0;
    }
    const int count = cJSON_GetArraySize(operations);
    if (count < 0 || (size_t)count > SKIFF_ROMM_SYNC_OPERATIONS_MAX) {
        return 0;
    }
    out->count = (size_t)count;
    for (int i = 0; i < count; i++) {
        if (!parse_operation(cJSON_GetArrayItem(operations, i), &out->operations[i])) {
            return 0;
        }
    }
    return 1;
}

skiff_err skiff_romm_parse_sync_plan(const char *json, size_t length, skiff_romm_sync_plan *out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    cJSON *root = skiff_romm_parse_json(json, length);
    const int ok = root != NULL && fill_plan(root, out);
    cJSON_Delete(root);
    if (!ok) {
        memset(out, 0, sizeof *out);
        return SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    return SKIFF_OK;
}

/* The name of the device that uploaded the save, from the devices RomM lists as having synced it;
 * left empty when none matches. */
static int read_origin_name(const cJSON *root, skiff_romm_save *out) {
    const cJSON *syncs = cJSON_GetObjectItemCaseSensitive(root, "device_syncs");
    if (syncs == NULL || cJSON_IsNull(syncs)) {
        return 1;
    }
    if (!cJSON_IsArray(syncs)) {
        return 0;
    }
    const cJSON *sync = NULL;
    cJSON_ArrayForEach(sync, syncs) {
        const cJSON *device = cJSON_GetObjectItemCaseSensitive(sync, FIELD_DEVICE_ID);
        if (!cJSON_IsObject(sync) || !cJSON_IsString(device) || device->valuestring == NULL) {
            return 0;
        }
        if (out->origin_device_id[0] != '\0' &&
            strcmp(device->valuestring, out->origin_device_id) == 0) {
            return skiff_romm_read_display_name(sync, "device_name", out->origin_device_name,
                                                sizeof out->origin_device_name);
        }
    }
    return 1;
}

static int fill_save(const cJSON *root, skiff_romm_save *out) {
    return cJSON_IsObject(root) && skiff_romm_read_count(root, "id", &out->id) &&
           skiff_romm_read_count(root, "rom_id", &out->rom_id) &&
           skiff_romm_read_text(root, "file_name", out->file_name, sizeof out->file_name) &&
           read_slot(root, &out->has_slot, out->slot) &&
           skiff_romm_read_optional_text(root, "emulator", out->emulator, sizeof out->emulator) &&
           read_hash(root, FIELD_CONTENT_HASH, out->content_hash) &&
           skiff_romm_read_text(root, "updated_at", out->updated_at, sizeof out->updated_at) &&
           skiff_romm_read_count(root, "file_size_bytes", &out->file_size_bytes) &&
           skiff_romm_read_optional_text(root, "origin_device_id", out->origin_device_id,
                                         sizeof out->origin_device_id) &&
           read_origin_name(root, out);
}

skiff_err skiff_romm_parse_save(const char *json, size_t length, skiff_romm_save *out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    cJSON *root = skiff_romm_parse_json(json, length);
    const int ok = root != NULL && fill_save(root, out);
    cJSON_Delete(root);
    if (!ok) {
        memset(out, 0, sizeof *out);
        return SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    return SKIFF_OK;
}

/* ---- Requests ---- */

/* POSTs json (freed here with cJSON_free(); NULL means out of memory) to path, and maps the status;
 * a 2xx answer's JSON goes to *out (free it with cJSON_Delete()), and a 2xx that is not JSON (a
 * proxy's page) is SKIFF_ERR_ROMM_BAD_RESPONSE: it confirms nothing. */
static skiff_err post_json(skiff_romm_client *client, const char *path, char *json, cJSON **out) {
    *out = NULL;
    if (json == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    const skiff_romm_body body = {.json = json};
    long status = 0;
    cJSON *root = NULL;
    skiff_err err = skiff_romm_request_json(client, SKIFF_HTTP_POST, path, &body, &status, &root);
    cJSON_free(json);
    if (err == SKIFF_OK) {
        err = skiff_http_status_error(status);
    }
    if (err == SKIFF_OK && root == NULL) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    if (err == SKIFF_OK) {
        *out = root;
        root = NULL;
    }
    cJSON_Delete(root);
    return err;
}

/* ISO 8601 in UTC, "2026-10-11T18:15:01.000Z"; 0 for a time before 1970 or past 9999. */
static int format_time(int64_t unix_ms, char *out, size_t out_size) {
    if (unix_ms < 0 || skiff_log_format_timestamp(unix_ms, out, out_size) != SKIFF_OK) {
        return 0;
    }
    out[ISO_DATE_LENGTH] = ISO_TIME_SEPARATOR;
    return 1;
}

static int state_valid(const skiff_romm_save_state *state) {
    char updated_at[SKIFF_LOG_TIMESTAMP_MAX];
    return id_valid(state->rom_id) && file_name_valid(state->file_name) &&
           slot_valid(state->slot) && emulator_valid(state->emulator) &&
           content_hash_valid(state->content_hash) &&
           format_time(state->updated_at_ms, updated_at, sizeof updated_at) &&
           state->file_size_bytes < JSON_INTEGER_LIMIT;
}

static int query_valid(const skiff_romm_sync_query *query) {
    if (!skiff_romm_device_id_valid(query->device_id) ||
        query->rom_count > SKIFF_ROMM_SYNC_ROMS_MAX ||
        query->save_count > SKIFF_ROMM_SYNC_SAVES_MAX ||
        (query->rom_count > 0 && query->rom_ids == NULL) ||
        (query->save_count > 0 && query->saves == NULL)) {
        return 0;
    }
    for (size_t i = 0; i < query->rom_count; i++) {
        if (!id_valid(query->rom_ids[i])) {
            return 0;
        }
    }
    for (size_t i = 0; i < query->save_count; i++) {
        if (!state_valid(&query->saves[i])) {
            return 0;
        }
    }
    return 1;
}

/* One save as negotiate's ClientSaveState; NULL when out of memory or for a time
 * format_time() refuses. */
static cJSON *state_json(const skiff_romm_save_state *state) {
    char updated_at[SKIFF_LOG_TIMESTAMP_MAX];
    cJSON *item = cJSON_CreateObject();
    if (item == NULL || !format_time(state->updated_at_ms, updated_at, sizeof updated_at) ||
        cJSON_AddNumberToObject(item, "rom_id", (double)state->rom_id) == NULL ||
        cJSON_AddStringToObject(item, "file_name", state->file_name) == NULL ||
        cJSON_AddStringToObject(item, "slot", state->slot) == NULL ||
        cJSON_AddStringToObject(item, "emulator", state->emulator) == NULL ||
        cJSON_AddStringToObject(item, FIELD_CONTENT_HASH, state->content_hash) == NULL ||
        cJSON_AddStringToObject(item, "updated_at", updated_at) == NULL ||
        cJSON_AddNumberToObject(item, "file_size_bytes", (double)state->file_size_bytes) == NULL) {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

/* The negotiate request: device_id, rom_ids (only when the query names ROMs), saves and
 * restore_unlisted when asked, and never "emulators" (see skiff/romm_saves.h). NULL when out of
 * memory; free it with cJSON_free(). */
static char *negotiate_body(const skiff_romm_sync_query *query) {
    cJSON *body = cJSON_CreateObject();
    cJSON *saves = cJSON_CreateArray();
    char *printed = NULL;
    int ok = body != NULL && saves != NULL &&
             cJSON_AddStringToObject(body, FIELD_DEVICE_ID, query->device_id) != NULL &&
             (!query->restore_unlisted || cJSON_AddTrueToObject(body, "restore_unlisted") != NULL);
    if (ok && query->rom_count > 0) {
        cJSON *rom_ids = cJSON_AddArrayToObject(body, "rom_ids");
        ok = rom_ids != NULL;
        for (size_t i = 0; ok && i < query->rom_count; i++) {
            cJSON *id = cJSON_CreateNumber((double)query->rom_ids[i]);
            ok = id != NULL && cJSON_AddItemToArray(rom_ids, id);
        }
    }
    for (size_t i = 0; ok && i < query->save_count; i++) {
        cJSON *state = state_json(&query->saves[i]);
        ok = state != NULL && cJSON_AddItemToArray(saves, state);
    }
    if (ok && cJSON_AddItemToObject(body, "saves", saves)) {
        saves = NULL; /* owned by body now */
        printed = cJSON_PrintUnformatted(body);
    }
    cJSON_Delete(saves);
    cJSON_Delete(body);
    return printed;
}

/* 1 if id is among the query's ROMs or its saves' ROMs; always when the query names no ROM. */
static int rom_asked(const skiff_romm_sync_query *query, uint64_t id) {
    if (query->rom_count == 0) {
        return 1;
    }
    for (size_t i = 0; i < query->rom_count; i++) {
        if (query->rom_ids[i] == id) {
            return 1;
        }
    }
    for (size_t i = 0; i < query->save_count; i++) {
        if (query->saves[i].rom_id == id) {
            return 1;
        }
    }
    return 0;
}

skiff_err skiff_romm_saves_negotiate(skiff_romm_client *client, const skiff_romm_sync_query *query,
                                     skiff_romm_sync_plan *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (!client_ready(client) || query == NULL || out == NULL || !query_valid(query)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    cJSON *root = NULL;
    skiff_err err = post_json(client, PATH_NEGOTIATE, negotiate_body(query), &root);
    if (err == SKIFF_OK && !fill_plan(root, out)) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    /* Operations for ROMs this PSP did not ask about would be applied to the wrong game. */
    for (size_t i = 0; err == SKIFF_OK && i < out->count; i++) {
        if (!rom_asked(query, out->operations[i].rom_id)) {
            err = SKIFF_ERR_ROMM_BAD_RESPONSE;
        }
    }
    cJSON_Delete(root);
    if (err != SKIFF_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}

/* ---- Upload ---- */

/* One upload's state, on the heap: the body is the multipart head, the file, then the tail. */
typedef struct upload_stream {
    char path[SKIFF_ROMM_REQUEST_PATH_MAX];
    char content_type[CONTENT_TYPE_MAX];
    char head[MULTIPART_HEAD_MAX];
    size_t head_size;
    char tail[MULTIPART_TAIL_MAX];
    size_t tail_size;
    skiff_file *file;
    uint64_t file_size;
    /* File bytes read into buffer so far, and whether the file was found to end there. */
    uint64_t file_read;
    int end_confirmed;
    /* Body bytes handed to the transport so far. */
    uint64_t position;
    unsigned char buffer[SKIFF_ROMM_UPLOAD_BUFFER_BYTES];
    size_t buffered;
    size_t buffer_offset;
    /* "--" and the boundary, which the file must not hold, and the file's last bytes before the
     * buffer (one fewer than the delimiter), for a delimiter split between two reads. */
    char delimiter[DELIMITER_MAX];
    size_t delimiter_size;
    unsigned char carry[DELIMITER_MAX];
    size_t carry_size;
} upload_stream;

static int upload_valid(const skiff_romm_upload *upload) {
    return id_valid(upload->rom_id) && skiff_romm_device_id_valid(upload->device_id) &&
           emulator_valid(upload->emulator) && slot_valid(upload->slot) &&
           content_hash_valid(upload->content_hash) && upload->keep_versions >= 1 &&
           upload->keep_versions <= SKIFF_ROMM_SAVE_VERSIONS_MAX && upload->storage != NULL &&
           upload->path != NULL && file_name_valid(upload->file_name);
}

/* The upload's path and query, its content type, and the multipart head and tail; 0 when the
 * path does not fit (it always does for values upload_valid() accepts). */
static int frame_upload(const skiff_romm_upload *upload, upload_stream *stream) {
    char boundary[BOUNDARY_MAX];
    char limit[sizeof AUTOCLEANUP_LIMIT_PARAMETER + UINT32_DIGITS_MAX];
    snprintf(boundary, sizeof boundary, BOUNDARY_PREFIX "%s", upload->content_hash);
    snprintf(stream->content_type, sizeof stream->content_type, CONTENT_TYPE_MULTIPART "%s",
             boundary);
    const int head = snprintf(stream->head, sizeof stream->head, MULTIPART_HEAD_FORMAT, boundary,
                              upload->file_name);
    const int tail = snprintf(stream->tail, sizeof stream->tail, MULTIPART_TAIL_FORMAT, boundary);
    snprintf(stream->delimiter, sizeof stream->delimiter, DELIMITER_DASHES "%s", boundary);
    stream->delimiter_size = strlen(stream->delimiter);
    snprintf(limit, sizeof limit, AUTOCLEANUP_LIMIT_PARAMETER "%u",
             (unsigned)upload->keep_versions);
    snprintf(stream->path, sizeof stream->path, PATH_SAVES "?rom_id=%llu",
             (unsigned long long)upload->rom_id);
    if (head < 0 || (size_t)head >= sizeof stream->head || tail < 0 ||
        (size_t)tail >= sizeof stream->tail) {
        return 0;
    }
    stream->head_size = (size_t)head;
    stream->tail_size = (size_t)tail;
    const size_t size = sizeof stream->path;
    return append_parameter(stream->path, size, "&emulator=", upload->emulator) &&
           append_parameter(stream->path, size, "&slot=", upload->slot) &&
           append_parameter(stream->path, size, "&device_id=", upload->device_id) &&
           append_parameter(stream->path, size, "&content_hash=", upload->content_hash) &&
           append_plain(stream->path, size,
                        upload->overwrite ? "&overwrite=true" : "&overwrite=false") &&
           append_plain(stream->path, size, "&autocleanup=true") &&
           append_plain(stream->path, size, limit);
}

/* 1 if needle (size > 0) occurs in haystack. memchr() finds each place its first byte occurs, so
 * a save's bytes are not compared one call per byte (8 MB of them on every upload). */
static int contains(const unsigned char *haystack, size_t haystack_size, const char *needle,
                    size_t needle_size) {
    if (haystack_size < needle_size) {
        return 0;
    }
    const size_t starts = haystack_size - needle_size + 1;
    const int first = (unsigned char)needle[0];
    for (size_t i = 0; i < starts;) {
        const unsigned char *found = memchr(haystack + i, first, starts - i);
        if (found == NULL) {
            return 0;
        }
        if (memcmp(found, needle, needle_size) == 0) {
            return 1;
        }
        i = (size_t)(found - haystack) + 1;
    }
    return 0;
}

/* Checks the bytes just read, with those before them, for the delimiter, before any of them is
 * sent: a part that ended early would reach RomM as a cut file. */
static int holds_delimiter(upload_stream *stream) {
    const size_t keep = stream->delimiter_size - 1;
    unsigned char seam[2 * DELIMITER_MAX];
    const size_t head = stream->buffered < keep ? stream->buffered : keep;
    memcpy(seam, stream->carry, stream->carry_size);
    memcpy(seam + stream->carry_size, stream->buffer, head);
    if (contains(seam, stream->carry_size + head, stream->delimiter, stream->delimiter_size) ||
        contains(stream->buffer, stream->buffered, stream->delimiter, stream->delimiter_size)) {
        return 1;
    }
    /* The last bytes of what has been read, for the next check. */
    const size_t total = stream->carry_size + stream->buffered;
    const size_t kept = total < keep ? total : keep;
    unsigned char last[DELIMITER_MAX] = {0};
    for (size_t i = 0; i < kept; i++) {
        const size_t at = total - kept + i;
        last[i] =
            at < stream->carry_size ? stream->carry[at] : stream->buffer[at - stream->carry_size];
    }
    memcpy(stream->carry, last, kept);
    stream->carry_size = kept;
    return 0;
}

/* Refills the buffer from the file; a file that ends early, or goes on past the size it had,
 * changed while it was being sent. */
static skiff_err refill(upload_stream *stream) {
    const uint64_t left = stream->file_size - stream->file_read;
    const size_t want = left < sizeof stream->buffer ? (size_t)left : sizeof stream->buffer;
    size_t got = 0;
    skiff_err err = skiff_file_read(stream->file, stream->buffer, want, &got);
    if (err != SKIFF_OK) {
        return err;
    }
    if (got == 0 || got > want) {
        return SKIFF_ERR_STORAGE_IO;
    }
    stream->buffered = got;
    stream->buffer_offset = 0;
    stream->file_read += got;
    return holds_delimiter(stream) ? SKIFF_ERR_INVALID_ARG : SKIFF_OK;
}

static skiff_err confirm_end(upload_stream *stream) {
    unsigned char extra = 0;
    size_t more = 0;
    const skiff_err err = skiff_file_read(stream->file, &extra, sizeof extra, &more);
    if (err != SKIFF_OK) {
        return err;
    }
    stream->end_confirmed = 1;
    return more == 0 ? SKIFF_OK : SKIFF_ERR_STORAGE_IO;
}

/* Copies from text (the head or the tail) at offset; returns the bytes copied. */
static size_t copy_part(const char *text, size_t size, uint64_t offset, unsigned char *out,
                        size_t capacity) {
    const size_t left = size - (size_t)offset;
    const size_t count = left < capacity ? left : capacity;
    memcpy(out, text + (size_t)offset, count);
    return count;
}

/* The transport's reader: the head, the file through the buffer, then the tail. */
static skiff_err read_upload(void *ctx, unsigned char *out, size_t capacity, size_t *out_size) {
    upload_stream *stream = ctx;
    const uint64_t file_end = stream->head_size + stream->file_size;
    const uint64_t body_end = file_end + stream->tail_size;
    size_t used = 0;
    while (used < capacity && stream->position < body_end) {
        size_t count = 0;
        if (stream->position < stream->head_size) {
            count = copy_part(stream->head, stream->head_size, stream->position, out + used,
                              capacity - used);
        } else if (stream->position < file_end) {
            if (stream->buffer_offset == stream->buffered) {
                const skiff_err err = refill(stream);
                if (err != SKIFF_OK) {
                    return err;
                }
            }
            const size_t left = stream->buffered - stream->buffer_offset;
            count = left < capacity - used ? left : capacity - used;
            memcpy(out + used, stream->buffer + stream->buffer_offset, count);
            stream->buffer_offset += count;
        } else {
            if (!stream->end_confirmed) {
                const skiff_err err = confirm_end(stream);
                if (err != SKIFF_OK) {
                    return err;
                }
            }
            count = copy_part(stream->tail, stream->tail_size, stream->position - file_end,
                              out + used, capacity - used);
        }
        used += count;
        stream->position += count;
    }
    *out_size = used;
    return SKIFF_OK;
}

static skiff_err upload_status_error(long status) {
    switch (status) {
    case HTTP_STATUS_CONFLICT:
        return SKIFF_ERR_ROMM_SAVE_CONFLICT;
    case HTTP_STATUS_PAYLOAD_TOO_LARGE:
        return SKIFF_ERR_ROMM_SAVE_TOO_LARGE;
    default:
        return skiff_http_status_error(status);
    }
}

/* Sends the open file and reads RomM's answer into out. */
static skiff_err send_upload(skiff_romm_client *client, const skiff_romm_upload *upload,
                             upload_stream *stream, skiff_romm_save *out) {
    const uint64_t total = stream->head_size + stream->file_size + stream->tail_size;
    /* body_size is a size_t: 32 bits on the PSP. */
    if (stream->file_size > (uint64_t)SIZE_MAX - stream->head_size - stream->tail_size) {
        return SKIFF_ERR_ROMM_SAVE_TOO_LARGE;
    }
    const skiff_romm_body body = {.read = read_upload,
                                  .read_ctx = stream,
                                  .size = (size_t)total,
                                  .content_type = stream->content_type,
                                  .long_transfer = 1};
    long status = 0;
    cJSON *root = NULL;
    skiff_err err =
        skiff_romm_request_json(client, SKIFF_HTTP_POST, stream->path, &body, &status, &root);
    if (err == SKIFF_OK) {
        err = upload_status_error(status);
    }
    /* RomM keeps an identical file once, in the same slot: anything else is not this save. */
    if (err == SKIFF_OK &&
        (root == NULL || !fill_save(root, out) || out->rom_id != upload->rom_id || !out->has_slot ||
         strcmp(out->slot, upload->slot) != 0)) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    cJSON_Delete(root);
    return err;
}

skiff_err skiff_romm_saves_upload(skiff_romm_client *client, const skiff_romm_upload *upload,
                                  skiff_romm_save *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (!client_ready(client) || upload == NULL || out == NULL || !upload_valid(upload)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    upload_stream *stream = calloc(1, sizeof *stream);
    if (stream == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    skiff_err err = frame_upload(upload, stream) ? SKIFF_OK : SKIFF_ERR_INVALID_ARG;
    if (err == SKIFF_OK) {
        err = skiff_storage_size(upload->storage, upload->path, &stream->file_size);
    }
    if (err == SKIFF_OK) {
        err = skiff_storage_open(upload->storage, upload->path, SKIFF_FILE_READ, 0, &stream->file);
    }
    if (err == SKIFF_OK) {
        err = send_upload(client, upload, stream, out);
    }
    /* Only read from: what the close reports changes nothing that was sent. */
    skiff_file_close(stream->file);
    free(stream);
    if (err != SKIFF_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}

/* ---- Download ---- */

/* Writes a 200's body into the .part file, within the cap. */
typedef struct download_sink {
    skiff_http_response response;
    skiff_file *file;
    uint64_t max_bytes;
    uint64_t written;
} download_sink;

static skiff_err write_download(void *ctx, const unsigned char *data, size_t size) {
    download_sink *sink = ctx;
    /* An error page is judged by its status alone and never lands in the file. */
    if (sink->response.status != HTTP_STATUS_OK) {
        return SKIFF_OK;
    }
    if ((sink->response.has_content_length && sink->response.content_length > sink->max_bytes) ||
        size > sink->max_bytes - sink->written) {
        return SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    const skiff_err err = skiff_file_write(sink->file, data, size);
    if (err == SKIFF_OK) {
        sink->written += size;
    }
    return err;
}

/* GETs the save into the open .part file and checks it arrived whole. */
static skiff_err fetch(skiff_romm_client *client, const skiff_romm_download *download,
                       download_sink *sink) {
    char url[SKIFF_ROMM_URL_MAX];
    snprintf(url, sizeof url, "%s" PATH_SAVES "/%llu/content?device_id=", client->base_url,
             (unsigned long long)download->save_id);
    if (skiff_romm_append_encoded(url, sizeof url, download->device_id) != SKIFF_OK ||
        !append_plain(url, sizeof url, "&optimistic=false")) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_http_header authorization;
    skiff_http_request request;
    memset(&request, 0, sizeof request);
    request.method = SKIFF_HTTP_GET;
    request.url = url;
    request.header_count = skiff_romm_auth_header(client, &authorization);
    request.headers = request.header_count > 0 ? &authorization : NULL;
    request.on_body = write_download;
    request.body_ctx = sink;
    request.long_transfer = 1;
    const skiff_http_response *response = &sink->response;
    skiff_err err = skiff_transport_perform(client->transport, &request, &sink->response);
    if (err == SKIFF_OK) {
        err = skiff_http_status_error(response->status);
    }
    if (err == SKIFF_OK &&
        (response->status != HTTP_STATUS_OK ||
         (response->has_content_length && response->content_length != sink->written))) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    if (err == SKIFF_OK) {
        err = skiff_file_sync(sink->file);
    }
    return err;
}

/* Puts the whole .part in place of path, replacing a file there (FAT renames onto nothing). Sets
 * *only_copy when the old file is gone but the rename failed: the .part is then the one copy left
 * of the save. */
static skiff_err place(skiff_storage *storage, const char *part, const char *path, int *only_copy) {
    *only_copy = 0;
    const skiff_err removed = skiff_storage_remove(storage, path);
    uint64_t size = 0;
    /* A device may report an error after the file is gone: then the rename can still go ahead. */
    if (removed != SKIFF_OK && removed != SKIFF_ERR_STORAGE_NOT_FOUND &&
        skiff_storage_size(storage, path, &size) != SKIFF_ERR_STORAGE_NOT_FOUND) {
        return removed;
    }
    const skiff_err err = skiff_storage_rename(storage, part, path);
    *only_copy = err != SKIFF_OK && removed != SKIFF_ERR_STORAGE_NOT_FOUND;
    return err;
}

skiff_err skiff_romm_saves_download(skiff_romm_client *client, const skiff_romm_download *download,
                                    uint64_t *size) {
    if (size != NULL) {
        *size = 0;
    }
    if (!client_ready(client) || download == NULL || size == NULL || !id_valid(download->save_id) ||
        !skiff_romm_device_id_valid(download->device_id) || download->storage == NULL ||
        download->path == NULL || download->path[0] == '\0') {
        return SKIFF_ERR_INVALID_ARG;
    }
    char part[SKIFF_STORAGE_PATH_MAX];
    const int written =
        snprintf(part, sizeof part, "%s" SKIFF_ROMM_DOWNLOAD_PART_SUFFIX, download->path);
    if (written < 0 || (size_t)written >= sizeof part) {
        return SKIFF_ERR_INVALID_ARG;
    }
    download_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.max_bytes = download->max_bytes;
    skiff_err err = skiff_storage_open(download->storage, part, SKIFF_FILE_REPLACE, 0, &sink.file);
    if (err == SKIFF_OK) {
        err = fetch(client, download, &sink);
        const skiff_err closed = skiff_file_close(sink.file);
        err = err != SKIFF_OK ? err : closed;
    }
    int only_copy = 0;
    if (err == SKIFF_OK) {
        err = place(download->storage, part, download->path, &only_copy);
    }
    if (err != SKIFF_OK) {
        /* No part file is left behind, so the next attempt starts over, unless it is all that is
         * left of the save. */
        if (!only_copy) {
            skiff_storage_remove(download->storage, part);
        }
        return err;
    }
    *size = sink.written;
    return SKIFF_OK;
}

/* ---- One save ---- */

skiff_err skiff_romm_saves_get(skiff_romm_client *client, uint64_t save_id, const char *device_id,
                               skiff_romm_save *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (!client_ready(client) || out == NULL || !id_valid(save_id) ||
        !skiff_romm_device_id_valid(device_id)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    char path[SKIFF_ROMM_URL_MAX];
    snprintf(path, sizeof path, PATH_SAVES "/%llu?device_id=", (unsigned long long)save_id);
    if (skiff_romm_append_encoded(path, sizeof path, device_id) != SKIFF_OK) {
        return SKIFF_ERR_INVALID_ARG;
    }
    cJSON *root = NULL;
    int too_large = 0;
    skiff_err err = skiff_romm_get_json(client, path, &root, &too_large);
    if (err == SKIFF_OK && (!fill_save(root, out) || out->id != save_id)) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    cJSON_Delete(root);
    if (err != SKIFF_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}

/* ---- Recording what was synced ---- */

skiff_err skiff_romm_saves_downloaded(skiff_romm_client *client, uint64_t save_id,
                                      const char *device_id, const char *content_hash) {
    if (!client_ready(client) || !id_valid(save_id) || !skiff_romm_device_id_valid(device_id) ||
        !content_hash_valid(content_hash)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    char path[SKIFF_ROMM_URL_MAX];
    snprintf(path, sizeof path, PATH_SAVES "/%llu/downloaded", (unsigned long long)save_id);
    cJSON *body = cJSON_CreateObject();
    char *json = NULL;
    if (body != NULL && cJSON_AddStringToObject(body, FIELD_DEVICE_ID, device_id) != NULL &&
        cJSON_AddStringToObject(body, FIELD_CONTENT_HASH, content_hash) != NULL) {
        json = cJSON_PrintUnformatted(body);
    }
    cJSON_Delete(body);
    /* RomM answers with the save it recorded this PSP for. */
    cJSON *root = NULL;
    skiff_romm_save *save = malloc(sizeof *save);
    skiff_err err = save == NULL ? SKIFF_ERR_NO_MEMORY : post_json(client, path, json, &root);
    if (save == NULL) {
        cJSON_free(json);
    }
    if (err == SKIFF_OK) {
        memset(save, 0, sizeof *save);
        err = fill_save(root, save) && save->id == save_id ? SKIFF_OK : SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    cJSON_Delete(root);
    free(save);
    return err;
}

skiff_err skiff_romm_saves_complete(skiff_romm_client *client, uint64_t session_id,
                                    uint32_t operations_completed, uint32_t operations_failed) {
    if (!client_ready(client) || !id_valid(session_id)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    char path[SKIFF_ROMM_URL_MAX];
    snprintf(path, sizeof path, PATH_SESSIONS "/%llu/complete", (unsigned long long)session_id);
    cJSON *body = cJSON_CreateObject();
    char *json = NULL;
    if (body != NULL &&
        cJSON_AddNumberToObject(body, "operations_completed", (double)operations_completed) !=
            NULL &&
        cJSON_AddNumberToObject(body, "operations_failed", (double)operations_failed) != NULL) {
        json = cJSON_PrintUnformatted(body);
    }
    cJSON_Delete(body);
    /* RomM answers with the session it closed. */
    cJSON *root = NULL;
    skiff_err err = post_json(client, path, json, &root);
    uint64_t closed = 0;
    if (err == SKIFF_OK &&
        (!skiff_romm_read_count(cJSON_GetObjectItemCaseSensitive(root, "session"), "id", &closed) ||
         closed != session_id)) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    cJSON_Delete(root);
    return err;
}
