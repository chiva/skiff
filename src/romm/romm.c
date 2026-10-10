#include "skiff/romm.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/http.h"

#include "romm_internal.h"

#define PATH_HEARTBEAT "/api/heartbeat"
#define PATH_PLATFORMS "/api/platforms"
#define PATH_ROMS "/api/roms"
#define BEARER_PREFIX "Bearer "
#define HEADER_AUTHORIZATION "Authorization"
#define CONTENT_TYPE_JSON "application/json"
/* What a ROM list asks for besides the page: a stable order, and none of the per-library extras
 * RomM adds by default (a character index, filter values, the id of every ROM), which grow with the
 * whole library rather than the page. */
#define ROM_LIST_QUERY                                                                             \
    "&order_by=name&order_dir=asc&with_char_index=false&with_filter_values=false"                  \
    "&with_rom_id_index=false"
/* What a ROM list adds for SKIFF_ROMM_LIST_FAVOURITES and for with_files. */
#define ROM_LIST_FAVOURITES "&favorite=true"
#define ROM_LIST_WITH_FILES "&with_files=true"
#define UNRESERVED_SYMBOLS "-._~"
/* Kept as they are in a cover URL besides the unreserved ones: they separate the path's segments
 * and its query. */
#define COVER_URL_SYMBOLS "/?=&:"
#define HEX_DIGITS "0123456789ABCDEF"
#define CRC32_HEX_DIGITS_MAX 8
#define HEX_BASE 16
#define DECIMAL_BASE 10
#define ASCII_DELETE 0x7F
/* First size of the response buffer; it doubles up to SKIFF_ROMM_BODY_MAX. */
#define BODY_INITIAL_BYTES ((size_t)16 * 1024)
/* 2^53: ids and sizes arrive as JSON numbers, which cJSON reads as doubles. Every whole number
 * below it is exact; at it and above, a neighbour would round to the same value (2^53 + 1 reads as
 * 2^53). */
#define JSON_INTEGER_LIMIT 9007199254740992.0
/* "\u0000": an escaped NUL would cut a name short where C strings end. */
#define JSON_ESCAPED_NUL "u0000"
#define JSON_ESCAPED_NUL_LENGTH 5
/* UTF-8 continuation bytes are 10xxxxxx. */
#define UTF8_CONTINUATION_MASK 0xC0U
#define UTF8_CONTINUATION 0x80U
/* C1 control characters, U+0080 to U+009F, in UTF-8. */
#define UTF8_C1_LEAD 0xC2U
#define UTF8_C1_FIRST 0x80U
#define UTF8_C1_LAST 0x9FU

/* ---- Fields ---- */

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* A JSON number that is a whole, non-negative value a double holds exactly. */
static int read_count(const cJSON *object, const char *name, uint64_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) {
        return 0;
    }
    const double value = item->valuedouble;
    if (!(value >= 0.0 && value < JSON_INTEGER_LIMIT) || value != (double)(uint64_t)value) {
        return 0;
    }
    *out = (uint64_t)value;
    return 1;
}

/*
 * The bytes of the control character text starts with: 1 for C0 (below ' ') and DEL, 2 for a C1
 * control (U+0080 to U+009F, which UTF-8 writes as C2 80 to C2 9F), 0 for anything else. text is
 * NUL-terminated, so reading the byte after a C2 stays inside it.
 */
static size_t control_length(const char *text) {
    const unsigned char byte = (unsigned char)text[0];
    if (byte < (unsigned char)' ' || byte == ASCII_DELETE) {
        return 1;
    }
    const unsigned char next = (unsigned char)text[1];
    return byte == UTF8_C1_LEAD && next >= UTF8_C1_FIRST && next <= UTF8_C1_LAST ? 2 : 0;
}

/* No control character, decoded from an escape such as "\u001b": a name is shown and logged. */
static int is_printable_text(const char *text) {
    for (const char *c = text; *c != '\0'; c++) {
        if (control_length(c) > 0) {
            return 0;
        }
    }
    return 1;
}

/* A JSON string copied whole into out; 0 when absent, not a string, too long, or holding a control
 * character. */
static int read_text(const cJSON *object, const char *name, char *out, size_t out_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return 0;
    }
    const size_t length = strlen(item->valuestring);
    if (length >= out_size || !is_printable_text(item->valuestring)) {
        return 0;
    }
    memcpy(out, item->valuestring, length + 1);
    return 1;
}

/* Like read_text(), but null or absent leaves out empty. */
static int read_optional_text(const cJSON *object, const char *name, char *out, size_t out_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    out[0] = '\0';
    return item == NULL || cJSON_IsNull(item) || read_text(object, name, out, out_size);
}

static int is_control_byte(char c) {
    const unsigned char byte = (unsigned char)c;
    return byte < (unsigned char)' ' || byte == ASCII_DELETE;
}

/* The first byte of a UTF-8 character, or ASCII: a cut before it splits no character. */
static int starts_character(char c) {
    return ((unsigned char)c & UTF8_CONTINUATION_MASK) != UTF8_CONTINUATION;
}

/*
 * Copies text into out (out_size > sizeof SKIFF_ROMM_NAME_CUT_MARKER) in the form a list can show:
 * each control character replaced, and a text too long cut at a character boundary with the
 * marker after it. Returns what was wrong with it.
 */
static skiff_romm_name_status copy_display_name(const char *text, char *out, size_t out_size) {
    const size_t length = strlen(text);
    size_t kept = length;
    if (length >= out_size) {
        kept = out_size - sizeof SKIFF_ROMM_NAME_CUT_MARKER;
        while (kept > 0 && !starts_character(text[kept])) {
            kept--;
        }
    }
    int control = 0;
    for (size_t i = 0; i < length; i++) {
        control = control || control_length(text + i) > 0;
    }
    /* kept ends on a character boundary, so no control character straddles it; one '?' per control
     * character makes the copy no longer than kept. */
    size_t used = 0;
    for (size_t i = 0; i < kept;) {
        const size_t skip = control_length(text + i);
        if (skip > 0) {
            out[used++] = SKIFF_ROMM_NAME_REPLACEMENT;
            i += skip;
        } else {
            out[used++] = text[i++];
        }
    }
    out[used] = '\0';
    if (kept < length) {
        memcpy(out + used, SKIFF_ROMM_NAME_CUT_MARKER, sizeof SKIFF_ROMM_NAME_CUT_MARKER);
    }
    return control         ? SKIFF_ROMM_NAME_CONTROL_CHAR
           : kept < length ? SKIFF_ROMM_NAME_TOO_LONG
                           : SKIFF_ROMM_NAME_OK;
}

/*
 * A name RomM gives (a ROM's title or file name) into out in display form, with what was wrong
 * with it in *status (left alone when nothing was, so one status covers several names). 0 only for
 * a structural problem: absent or empty when required, or not a string (null counts as absent).
 */
static int read_name(const cJSON *object, const char *name, int required, char *out,
                     size_t out_size, skiff_romm_name_status *status) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    out[0] = '\0';
    if (item == NULL || cJSON_IsNull(item)) {
        return !required;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        (required && item->valuestring[0] == '\0')) {
        return 0;
    }
    const skiff_romm_name_status found = copy_display_name(item->valuestring, out, out_size);
    if (*status == SKIFF_ROMM_NAME_OK) {
        *status = found;
    }
    return 1;
}

/* RomM's crc_hash: hexadecimal, at most 8 digits; empty or null when RomM has none. */
static int read_crc32(const cJSON *object, const char *name, int *has, uint32_t *out) {
    char text[CRC32_HEX_DIGITS_MAX + 1];
    *has = 0;
    *out = 0;
    if (!read_optional_text(object, name, text, sizeof text)) {
        return 0;
    }
    const size_t length = strlen(text);
    if (length == 0) {
        return 1;
    }
    if (strspn(text, "0123456789abcdefABCDEF") != length) {
        return 0;
    }
    *out = (uint32_t)strtoul(text, NULL, HEX_BASE);
    *has = 1;
    return 1;
}

static int read_flag(const cJSON *object, const char *name) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static int parse_file(const cJSON *item, uint64_t rom_id, skiff_romm_file *out) {
    memset(out, 0, sizeof *out);
    uint64_t owner = 0;
    /* A file listed under another ROM would download the wrong bytes under this one's name. */
    return cJSON_IsObject(item) && read_count(item, "rom_id", &owner) && owner == rom_id &&
           read_name(item, "file_name", 1, out->file_name, sizeof out->file_name,
                     &out->name_status) &&
           read_count(item, "file_size_bytes", &out->size) &&
           read_crc32(item, "crc_hash", &out->has_crc32, &out->crc32);
}

/* A file of a ROM whose own names are not usable is not downloadable either. */
static void take_rom_name_status(const skiff_romm_rom_summary *summary, skiff_romm_file *file) {
    if (file->name_status == SKIFF_ROMM_NAME_OK) {
        file->name_status = summary->name_status;
    }
}

/* The summary's only file, when the answer lists exactly one that reads as this ROM's. Anything
 * else leaves has_file 0 and keeps the ROM listed: one odd file must not refuse a page. */
static void read_only_file(const cJSON *item, skiff_romm_rom_summary *out) {
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(item, "files");
    if (!cJSON_IsArray(files) || cJSON_GetArraySize(files) != 1) {
        return;
    }
    if (!parse_file(cJSON_GetArrayItem(files, 0), out->id, &out->file)) {
        memset(&out->file, 0, sizeof out->file);
        return;
    }
    take_rom_name_status(out, &out->file);
    out->has_file = 1;
}

static int parse_summary(const cJSON *item, skiff_romm_rom_summary *out) {
    memset(out, 0, sizeof *out);
    if (!cJSON_IsObject(item) || !read_count(item, "id", &out->id) ||
        !read_count(item, "platform_id", &out->platform_id) ||
        !read_name(item, "name", 0, out->name, sizeof out->name, &out->name_status) ||
        !read_name(item, "fs_name", 1, out->fs_name, sizeof out->fs_name, &out->name_status) ||
        !read_count(item, "fs_size_bytes", &out->size) ||
        !read_crc32(item, "crc_hash", &out->has_crc32, &out->crc32)) {
        return 0;
    }
    out->multiple_files = read_flag(item, "has_multiple_files");
    read_only_file(item, out);
    return 1;
}

/* A segment of a path that a server resolves to the folder itself or the one above it. */
static int is_dot_segment(const char *segment, size_t length) {
    return (length == 1 && segment[0] == '.') ||
           (length == 2 && segment[0] == '.' && segment[1] == '.');
}

/*
 * A cover path Skiff will request: under SKIFF_ROMM_COVER_PREFIX, so a response cannot send it
 * elsewhere on the server or to another host; no "." or ".." segment before the query, which would
 * climb out of the prefix; no backslash, which some servers read as '/'; no control character; and
 * short enough for skiff_romm_rom.cover_path.
 */
static int is_cover_path(const char *path) {
    const size_t length = strlen(path);
    const size_t prefix_length = sizeof SKIFF_ROMM_COVER_PREFIX - 1;
    if (length <= prefix_length || length >= SKIFF_ROMM_COVER_PATH_MAX ||
        strncmp(path, SKIFF_ROMM_COVER_PREFIX, prefix_length) != 0 || strchr(path, '\\') != NULL ||
        !is_printable_text(path)) {
        return 0;
    }
    /* Only '?' ends the path: skiff_romm_cover_url() encodes a '#' as "%23", so what follows one is
     * still path and a request would climb with its ".." segments. */
    const size_t path_end = strcspn(path, "?");
    for (size_t start = 0; start < path_end;) {
        const size_t segment = strcspn(path + start, "/?");
        if (is_dot_segment(path + start, segment)) {
            return 0;
        }
        start += segment + 1;
    }
    return 1;
}

/* path_cover_small into out when it is a path is_cover_path() accepts, otherwise empty: a cover
 * never refuses its ROM. */
static void read_cover_path(const cJSON *object, char *out, size_t out_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "path_cover_small");
    out[0] = '\0';
    if (cJSON_IsString(item) && item->valuestring != NULL && is_cover_path(item->valuestring) &&
        strlen(item->valuestring) < out_size) {
        memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    }
}

/* ---- Parsing whole responses ---- */

static int is_json_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/*
 * Checks a body before cJSON builds a tree from it: at most SKIFF_ROMM_JSON_NODES_MAX values (each
 * value but the first follows a ',' or opens its '[' or '{', so counting those outside strings
 * bounds them without parsing), no NUL anywhere, raw or escaped (it would cut a name short at the
 * C string's end), and no raw control character in a string.
 */
static int json_shape_ok(const char *json, size_t length) {
    size_t openings = 0;
    int in_string = 0;
    for (size_t i = 0; i < length; i++) {
        const char c = json[i];
        /* JSON has no raw C0 control characters inside a string and no NUL anywhere; a raw DEL is
         * refused with them, so only an escaped control character reaches a name. */
        if (c == '\0' || (in_string && is_control_byte(c))) {
            return 0;
        }
        if (in_string) {
            if (c == '\\') {
                if (length - i > JSON_ESCAPED_NUL_LENGTH &&
                    memcmp(json + i + 1, JSON_ESCAPED_NUL, JSON_ESCAPED_NUL_LENGTH) == 0) {
                    return 0;
                }
                i++;
            } else if (c == '"') {
                in_string = 0;
            }
        } else if (c == '"') {
            in_string = 1;
        } else if (c == ',' || c == '[' || c == '{') {
            if (++openings >= SKIFF_ROMM_JSON_NODES_MAX) {
                return 0;
            }
        }
    }
    return 1;
}

/* The whole body as one JSON value, followed by nothing but blanks. */
/* Wipes size bytes so the compiler cannot drop the stores. Does nothing for NULL. */
static void wipe(char *bytes, size_t size) {
    if (bytes == NULL) {
        return;
    }
    volatile char *target = bytes;
    for (size_t i = 0; i < size; i++) {
        target[i] = '\0';
    }
}

void skiff_romm_json_wipe_strings(cJSON *item) {
    /* Depth first without recursion: where to carry on at each level above the current one.
     * cJSON refuses trees nested deeper than CJSON_NESTING_LIMIT, so this always has room. */
    cJSON *resume[CJSON_NESTING_LIMIT + 1];
    size_t depth = 0;
    cJSON *current = item;
    while (current != NULL || depth > 0) {
        if (current == NULL) {
            current = resume[--depth];
            continue;
        }
        if (cJSON_IsString(current) && current->valuestring != NULL) {
            wipe(current->valuestring, strlen(current->valuestring));
        }
        if (current->child != NULL && depth < sizeof resume / sizeof resume[0]) {
            resume[depth++] = current->next;
            current = current->child;
        } else {
            current = current->next;
        }
    }
}

/* Frees a tree, first wiping its strings when it may hold a credential. */
static void delete_tree(cJSON *root, int secret) {
    if (secret) {
        skiff_romm_json_wipe_strings(root);
    }
    cJSON_Delete(root);
}

/* The whole body as one JSON value, followed by nothing but blanks; a refused tree holding a
 * credential is wiped before it is freed. */
static cJSON *parse_json_tree(const char *json, size_t length, int secret) {
    if (json == NULL || length == 0 || !json_shape_ok(json, length)) {
        return NULL;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, 0);
    if (root == NULL || end == NULL || end < json || (size_t)(end - json) > length) {
        delete_tree(root, secret);
        return NULL;
    }
    for (const char *rest = end; rest < json + length; rest++) {
        if (!is_json_blank(*rest)) {
            delete_tree(root, secret);
            return NULL;
        }
    }
    return root;
}

static cJSON *parse_json(const char *json, size_t length) {
    return parse_json_tree(json, length, 0);
}

static int fill_rom_page(const cJSON *root, skiff_romm_rom_page *out) {
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsObject(root) || !cJSON_IsArray(items) || !read_count(root, "total", &out->total) ||
        !read_count(root, "offset", &out->offset)) {
        return 0;
    }
    const int count = cJSON_GetArraySize(items);
    if (count < 0 || (size_t)count > SKIFF_ROMM_PAGE_SIZE) {
        return 0;
    }
    out->count = (size_t)count;
    /* Past the total there is nothing to list. */
    if (out->offset > out->total || out->count > out->total - out->offset) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (!parse_summary(cJSON_GetArrayItem(items, i), &out->items[i])) {
            return 0;
        }
    }
    return 1;
}

/* Every ROM of a page belongs to platform_id. */
static int on_platform(const skiff_romm_rom_page *page, uint64_t platform_id) {
    for (size_t i = 0; i < page->count; i++) {
        if (page->items[i].platform_id != platform_id) {
            return 0;
        }
    }
    return 1;
}

skiff_err skiff_romm_parse_rom_page(const char *json, size_t length, skiff_romm_rom_page *out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    cJSON *root = parse_json(json, length);
    const int ok = root != NULL && fill_rom_page(root, out);
    cJSON_Delete(root);
    if (!ok) {
        memset(out, 0, sizeof *out);
        return SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    return SKIFF_OK;
}

static int fill_rom(const cJSON *root, skiff_romm_rom *out) {
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files");
    if (!parse_summary(root, &out->summary) || !cJSON_IsArray(files)) {
        return 0;
    }
    const int count = cJSON_GetArraySize(files);
    if (count < 0) {
        return 0;
    }
    out->file_count = (size_t)count;
    /* Every file is checked; the first SKIFF_ROMM_FILES_MAX are kept. */
    for (int i = 0; i < count; i++) {
        skiff_romm_file file;
        if (!parse_file(cJSON_GetArrayItem(files, i), out->summary.id, &file)) {
            return 0;
        }
        if (i < SKIFF_ROMM_FILES_MAX) {
            take_rom_name_status(&out->summary, &file);
            out->files[i] = file;
            out->stored_count++;
        }
    }
    read_cover_path(root, out->cover_path, sizeof out->cover_path);
    return 1;
}

skiff_err skiff_romm_parse_rom(const char *json, size_t length, skiff_romm_rom *out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    cJSON *root = parse_json(json, length);
    const int ok = root != NULL && fill_rom(root, out);
    cJSON_Delete(root);
    if (!ok) {
        memset(out, 0, sizeof *out);
        return SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    return SKIFF_OK;
}

/* ---- The version policy ---- */

/* Reads a decimal number at *text, moving past it; 0 when there is none or it is too large. */
static int take_number(const char **text, int *out) {
    if (!is_digit(**text)) {
        return 0;
    }
    int64_t value = 0;
    while (is_digit(**text)) {
        value = value * DECIMAL_BASE + (**text - '0');
        if (value > INT32_MAX) {
            return 0;
        }
        (*text)++;
    }
    *out = (int)value;
    return 1;
}

/* "major.minor[.patch][anything]", e.g. "5.3.1" or "5.4.0-beta.1". */
static int parse_version(const char *text, skiff_romm_server *out) {
    const char *cursor = text;
    if (!take_number(&cursor, &out->major) || *cursor != '.') {
        return 0;
    }
    cursor++;
    if (!take_number(&cursor, &out->minor)) {
        return 0;
    }
    if (*cursor == '.') {
        cursor++;
        if (!take_number(&cursor, &out->patch)) {
            out->patch = 0;
        }
    }
    return 1;
}

skiff_err skiff_romm_check_version(const char *version, skiff_romm_server *out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (version == NULL || version[0] == '\0' || strlen(version) >= sizeof out->version ||
        !is_printable_text(version)) {
        return SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    memcpy(out->version, version, strlen(version) + 1);
    out->version_known = parse_version(version, out);
    if (!out->version_known) {
        out->major = 0;
        out->minor = 0;
        out->patch = 0;
        out->newer_than_tested = 1;
        return SKIFF_OK;
    }
    if (out->major < SKIFF_ROMM_MIN_MAJOR ||
        (out->major == SKIFF_ROMM_MIN_MAJOR && out->minor < SKIFF_ROMM_MIN_MINOR)) {
        return SKIFF_ERR_ROMM_UNSUPPORTED_VERSION;
    }
    out->newer_than_tested =
        out->major > SKIFF_ROMM_TESTED_MAJOR ||
        (out->major == SKIFF_ROMM_TESTED_MAJOR && out->minor > SKIFF_ROMM_TESTED_MINOR);
    return SKIFF_OK;
}

/* ---- Requests ---- */

/* Collects a JSON response in a buffer that grows up to SKIFF_ROMM_BODY_MAX. */
typedef struct body_sink {
    const skiff_http_response *response;
    /* Keep the body of an error status too: pairing reads its state from a 400's JSON. */
    int keep_errors;
    /* The body may hold a credential (pairing's device code or token): every copy of it is wiped
     * before its memory is given back. */
    int secret;
    char *data;
    size_t used;
    size_t capacity;
} body_sink;

static int is_success(long status) {
    enum { FIRST_SUCCESS = 200, FIRST_REDIRECT = 300 };
    return status >= FIRST_SUCCESS && status < FIRST_REDIRECT;
}

/* The sink's buffer at capacity bytes; realloc() would leave a secret behind in the old block. */
static char *grow(const body_sink *sink, size_t capacity) {
    if (!sink->secret) {
        return realloc(sink->data, capacity);
    }
    char *grown = malloc(capacity);
    if (grown != NULL && sink->data != NULL) {
        memcpy(grown, sink->data, sink->used);
        wipe(sink->data, sink->capacity);
        free(sink->data);
    }
    return grown;
}

static skiff_err collect_body(void *ctx, const unsigned char *data, size_t size) {
    body_sink *sink = ctx;
    /* An error page is judged by its status alone, unless the caller reads it. */
    if (!sink->keep_errors && !is_success(sink->response->status)) {
        return SKIFF_OK;
    }
    if ((sink->response->has_content_length &&
         sink->response->content_length > SKIFF_ROMM_BODY_MAX) ||
        size > SKIFF_ROMM_BODY_MAX - sink->used) {
        return SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    if (sink->used + size > sink->capacity) {
        size_t capacity = sink->capacity == 0 ? BODY_INITIAL_BYTES : sink->capacity;
        while (capacity < sink->used + size) {
            capacity = capacity > SKIFF_ROMM_BODY_MAX / 2 ? SKIFF_ROMM_BODY_MAX : capacity * 2;
        }
        char *grown = grow(sink, capacity);
        if (grown == NULL) {
            return SKIFF_ERR_NO_MEMORY;
        }
        sink->data = grown;
        sink->capacity = capacity;
    }
    memcpy(sink->data + sink->used, data, size);
    sink->used += size;
    return SKIFF_OK;
}

/* Sends one request to base + path, with json_body (NULL for none) as a POST's body, and parses the
 * response body as JSON into *out (NULL when it is not JSON; free it with cJSON_Delete()). Bodies
 * of error statuses are read only with keep_errors. Returns the transport's error; the HTTP status
 * is left to the caller in *status. */
static skiff_err send_json(skiff_romm_client *client, skiff_http_method method, const char *path,
                           const char *json_body, int with_token, int keep_errors, long *status,
                           cJSON **out) {
    /* Only pairing reads error bodies, and only pairing's bodies hold credentials. */
    const int secret = keep_errors;
    *out = NULL;
    *status = 0;
    char url[SKIFF_ROMM_URL_MAX];
    const int written = snprintf(url, sizeof url, "%s%s", client->base_url, path);
    if (written < 0 || (size_t)written >= sizeof url) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_http_header authorization = {HEADER_AUTHORIZATION, client->authorization};
    const int send_token = with_token && client->authorization[0] != '\0';
    skiff_http_response response;
    body_sink sink = {&response, keep_errors, secret, NULL, 0, 0};
    skiff_http_request request;
    memset(&request, 0, sizeof request);
    request.method = method;
    request.url = url;
    request.headers = send_token ? &authorization : NULL;
    request.header_count = send_token ? 1 : 0;
    if (json_body != NULL) {
        request.body = json_body;
        request.body_size = strlen(json_body);
        request.content_type = CONTENT_TYPE_JSON;
    }
    request.on_body = collect_body;
    request.body_ctx = &sink;
    const skiff_err err = skiff_transport_perform(client->transport, &request, &response);
    *status = response.status;
    if (err == SKIFF_OK && (keep_errors || is_success(response.status))) {
        *out = parse_json_tree(sink.data, sink.used, secret);
    }
    if (secret) {
        wipe(sink.data, sink.capacity);
    }
    free(sink.data);
    return err;
}

/* GETs base + path and parses the body as JSON into *out (free it with cJSON_Delete()). */
static skiff_err get_json(skiff_romm_client *client, const char *path, int with_token,
                          cJSON **out) {
    long status = 0;
    skiff_err err = send_json(client, SKIFF_HTTP_GET, path, NULL, with_token, 0, &status, out);
    if (err == SKIFF_OK) {
        err = skiff_http_status_error(status);
    }
    if (err == SKIFF_OK && *out == NULL) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    if (err != SKIFF_OK) {
        cJSON_Delete(*out);
        *out = NULL;
    }
    return err;
}

skiff_err skiff_romm_post_json(skiff_romm_client *client, const char *path, const char *json_body,
                               long *status, cJSON **out) {
    if (out != NULL) {
        *out = NULL;
    }
    if (status != NULL) {
        *status = 0;
    }
    if (client == NULL || client->transport == NULL || client->base_url[0] == '\0' ||
        path == NULL || json_body == NULL || status == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return send_json(client, SKIFF_HTTP_POST, path, json_body, 0, 1, status, out);
}

/* ---- The client ---- */

static int is_token_text(const char *token) {
    for (const char *c = token; *c != '\0'; c++) {
        const unsigned char byte = (unsigned char)*c;
        if (byte <= (unsigned char)' ' || byte >= ASCII_DELETE) {
            return 0;
        }
    }
    return 1;
}

skiff_err skiff_romm_client_init(skiff_romm_client *client, skiff_transport *transport,
                                 const char *base_url, const char *token) {
    if (client == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(client, 0, sizeof *client);
    if (transport == NULL || base_url == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    size_t length = strlen(base_url);
    while (length > 0 && base_url[length - 1] == '/') {
        length--;
    }
    if (!skiff_http_url_scheme_valid(base_url) || length >= sizeof client->base_url) {
        return SKIFF_ERR_CONFIG_INVALID_VALUE;
    }
    if (token != NULL && token[0] != '\0') {
        const int written = snprintf(client->authorization, sizeof client->authorization,
                                     BEARER_PREFIX "%s", token);
        /* The same limit config.ini puts on a token. */
        if (strlen(token) >= SKIFF_CONFIG_TOKEN_MAX || !is_token_text(token) || written < 0 ||
            (size_t)written >= sizeof client->authorization) {
            skiff_romm_client_clear(client);
            return SKIFF_ERR_INVALID_ARG;
        }
    }
    memcpy(client->base_url, base_url, length);
    client->base_url[length] = '\0';
    client->transport = transport;
    return SKIFF_OK;
}

void skiff_romm_client_clear(skiff_romm_client *client) {
    if (client != NULL) {
        /* volatile, so the compiler cannot drop a wipe of memory that is not read again. */
        volatile char *secret = client->authorization;
        for (size_t i = 0; i < sizeof client->authorization; i++) {
            secret[i] = '\0';
        }
    }
}

size_t skiff_romm_auth_header(const skiff_romm_client *client, skiff_http_header *out) {
    if (client == NULL || out == NULL || client->authorization[0] == '\0') {
        return 0;
    }
    out->name = HEADER_AUTHORIZATION;
    out->value = client->authorization;
    return 1;
}

static int client_ready(const skiff_romm_client *client) {
    return client != NULL && client->transport != NULL && client->base_url[0] != '\0';
}

skiff_err skiff_romm_heartbeat(skiff_romm_client *client, skiff_romm_server *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (!client_ready(client) || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    cJSON *root = NULL;
    skiff_err err = get_json(client, PATH_HEARTBEAT, 0, &root);
    if (err == SKIFF_OK) {
        const cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "SYSTEM");
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(system, "VERSION");
        err = cJSON_IsString(version) ? skiff_romm_check_version(version->valuestring, out)
                                      : SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    cJSON_Delete(root);
    return err;
}

static int parse_platform(const cJSON *item, skiff_romm_platform *out) {
    memset(out, 0, sizeof *out);
    return cJSON_IsObject(item) && read_count(item, "id", &out->id) &&
           read_text(item, "slug", out->slug, sizeof out->slug) &&
           read_optional_text(item, "display_name", out->name, sizeof out->name) &&
           read_count(item, "rom_count", &out->rom_count);
}

/* The element of a platform array whose slug is slug, or NULL. */
static const cJSON *find_by_slug(const cJSON *platforms, const char *slug) {
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, platforms) {
        const cJSON *item_slug = cJSON_GetObjectItemCaseSensitive(item, "slug");
        if (cJSON_IsString(item_slug) && strcmp(item_slug->valuestring, slug) == 0) {
            return item;
        }
    }
    return NULL;
}

skiff_err skiff_romm_find_platform(skiff_romm_client *client, const char *slug,
                                   skiff_romm_platform *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (!client_ready(client) || slug == NULL || slug[0] == '\0' || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    cJSON *root = NULL;
    skiff_err err = get_json(client, PATH_PLATFORMS, 1, &root);
    if (err == SKIFF_OK) {
        const cJSON *match = find_by_slug(root, slug);
        err = !cJSON_IsArray(root)         ? SKIFF_ERR_ROMM_BAD_RESPONSE
              : match == NULL              ? SKIFF_ERR_ROMM_NOT_FOUND
              : parse_platform(match, out) ? SKIFF_OK
                                           : SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    cJSON_Delete(root);
    if (err != SKIFF_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}

skiff_err skiff_romm_list_roms(skiff_romm_client *client, const skiff_romm_list_query *query,
                               uint64_t offset, size_t limit, skiff_romm_rom_page *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (!client_ready(client) || query == NULL || out == NULL || limit == 0 ||
        limit > SKIFF_ROMM_PAGE_SIZE) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const uint64_t platform_id = query->platform_id;
    char path[SKIFF_ROMM_URL_MAX];
    snprintf(path, sizeof path,
             PATH_ROMS "?platform_ids=%llu&limit=%zu&offset=%llu" ROM_LIST_QUERY "%s%s",
             (unsigned long long)platform_id, limit, (unsigned long long)offset,
             query->filter == SKIFF_ROMM_LIST_FAVOURITES ? ROM_LIST_FAVOURITES : "",
             query->with_files ? ROM_LIST_WITH_FILES : "");
    cJSON *root = NULL;
    skiff_err err = get_json(client, path, 1, &root);
    /* The page must be the one asked for: never show another page's ROMs under this offset. */
    if (err == SKIFF_OK && (!fill_rom_page(root, out) || out->offset != offset ||
                            out->count > limit || !on_platform(out, platform_id))) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    cJSON_Delete(root);
    if (err != SKIFF_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}

skiff_err skiff_romm_get_rom(skiff_romm_client *client, uint64_t rom_id, skiff_romm_rom *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (!client_ready(client) || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    char path[SKIFF_ROMM_URL_MAX];
    snprintf(path, sizeof path, PATH_ROMS "/%llu", (unsigned long long)rom_id);
    cJSON *root = NULL;
    skiff_err err = get_json(client, path, 1, &root);
    /* A different ROM under this id would download the wrong file. */
    if (err == SKIFF_OK && (!fill_rom(root, out) || out->summary.id != rom_id)) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    cJSON_Delete(root);
    if (err != SKIFF_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}

/* ---- URLs ---- */

static int is_unreserved(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_digit(c) ||
           (c != '\0' && strchr(UNRESERVED_SYMBOLS, c) != NULL);
}

/*
 * Writes prefix into out, then text with every byte that is neither unreserved nor in kept
 * percent-encoded. SKIFF_ERR_BUFFER_TOO_SMALL (out emptied) when the result does not fit.
 */
static skiff_err write_encoded(char *out, size_t out_size, const char *prefix, const char *text,
                               const char *kept) {
    size_t used = strlen(prefix);
    if (used >= out_size) {
        out[0] = '\0';
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(out, prefix, used);
    for (const char *c = text; *c != '\0'; c++) {
        const int plain = is_unreserved(*c) || strchr(kept, *c) != NULL;
        const size_t needed = plain ? 1 : 3;
        if (needed >= out_size - used) {
            out[0] = '\0';
            return SKIFF_ERR_BUFFER_TOO_SMALL;
        }
        if (plain) {
            out[used++] = *c;
        } else {
            const unsigned char byte = (unsigned char)*c;
            out[used++] = '%';
            out[used++] = HEX_DIGITS[byte >> 4];
            out[used++] = HEX_DIGITS[byte & 0x0F];
        }
    }
    out[used] = '\0';
    return SKIFF_OK;
}

skiff_err skiff_romm_content_url(const skiff_romm_client *client, uint64_t rom_id,
                                 const skiff_romm_file *file, char *out, size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    /* A name that is not OK is a display form: requesting it would ask RomM for another file. */
    if (!client_ready(client) || file == NULL || file->name_status != SKIFF_ROMM_NAME_OK ||
        file->file_name[0] == '\0' || out == NULL || out_size == 0) {
        return SKIFF_ERR_INVALID_ARG;
    }
    char prefix[SKIFF_ROMM_URL_MAX];
    snprintf(prefix, sizeof prefix, "%s" PATH_ROMS "/%llu/content/", client->base_url,
             (unsigned long long)rom_id);
    /* A '#', '?' or '/' in a file name must not end or split the path. */
    return write_encoded(out, out_size, prefix, file->file_name, "");
}

skiff_err skiff_romm_cover_url(const skiff_romm_client *client, const char *cover_path, char *out,
                               size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (!client_ready(client) || cover_path == NULL || !is_cover_path(cover_path) || out == NULL ||
        out_size == 0) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return write_encoded(out, out_size, client->base_url, cover_path, COVER_URL_SYMBOLS);
}

skiff_err skiff_romm_get_cover(skiff_romm_client *client, const char *cover_path,
                               unsigned char **out, size_t *size) {
    if (out != NULL) {
        *out = NULL;
    }
    if (size != NULL) {
        *size = 0;
    }
    if (!client_ready(client) || cover_path == NULL || out == NULL || size == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    char url[SKIFF_ROMM_COVER_URL_MAX];
    skiff_err err = skiff_romm_cover_url(client, cover_path, url, sizeof url);
    if (err != SKIFF_OK) {
        return err;
    }
    skiff_http_response response;
    body_sink sink = {&response, 0, 0, NULL, 0, 0};
    skiff_http_request request;
    memset(&request, 0, sizeof request);
    request.method = SKIFF_HTTP_GET;
    request.url = url;
    request.on_body = collect_body;
    request.body_ctx = &sink;
    /* RomM's web server serves covers to anyone: the token stays off this request. */
    err = skiff_transport_perform(client->transport, &request, &response);
    if (err == SKIFF_OK) {
        err = skiff_http_status_error(response.status);
    }
    if (err == SKIFF_OK && sink.used == 0) {
        err = SKIFF_ERR_ROMM_COVER_DAMAGED;
    }
    if (err != SKIFF_OK) {
        free(sink.data);
        return err;
    }
    *out = (unsigned char *)sink.data;
    *size = sink.used;
    return SKIFF_OK;
}
