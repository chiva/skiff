#include "skiff/config.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTION_SKIFF "skiff"
#define SECTION_SERVER "server"
#define SECTION_AUTH "auth"
#define SECTION_MTLS "mtls"
#define SECTION_HEADERS "headers"
#define KEY_VERSION "version"
#define KEY_URL "url"
#define KEY_CA_FILE "ca_file"
#define KEY_TOKEN "token"
#define KEY_CERT_FILE "cert_file"
#define KEY_KEY_FILE "key_file"
/* Windows Notepad starts a UTF-8 file with it. */
#define UTF8_BOM "\xEF\xBB\xBF"
#define UTF8_BOM_LENGTH 3
#define DECIMAL_BASE 10
#define ASCII_DELETE 0x7F
#define EOL_LF "\n"
#define EOL_CRLF "\r\n"
/* Characters RFC 9110 allows in a header name besides letters and digits. */
#define HEADER_NAME_SYMBOLS "!#$%&'*+-.^_`|~"

/* Headers Skiff sets itself: the token goes in Authorization, and Host follows the server address.
 * Range and If-Range are refused by skiff_http_headers_valid(). */
static const char *const RESERVED_HEADERS[] = {"authorization", "host"};

typedef enum value_kind { VALUE_VERSION, VALUE_URL, VALUE_FILE_NAME, VALUE_TOKEN } value_kind;

#define FIELD(name) offsetof(skiff_config, name), sizeof(((skiff_config *)NULL)->name)

typedef struct known_key {
    const char *section;
    const char *key;
    value_kind kind;
    /* Where the value goes in skiff_config; unused for the version. */
    size_t offset;
    size_t size;
} known_key;

// clang-format off
static const known_key KNOWN_KEYS[] = {
    {SECTION_SKIFF, KEY_VERSION, VALUE_VERSION, 0, 0},
    {SECTION_SERVER, KEY_URL, VALUE_URL, FIELD(server_url)},
    {SECTION_SERVER, KEY_CA_FILE, VALUE_FILE_NAME, FIELD(ca_file)},
    {SECTION_AUTH, KEY_TOKEN, VALUE_TOKEN, FIELD(token)},
    {SECTION_MTLS, KEY_CERT_FILE, VALUE_FILE_NAME, FIELD(cert_file)},
    {SECTION_MTLS, KEY_KEY_FILE, VALUE_FILE_NAME, FIELD(key_file)},
};
// clang-format on

enum { KNOWN_KEY_COUNT = sizeof KNOWN_KEYS / sizeof KNOWN_KEYS[0] };

/* A piece of the text, not terminated. */
typedef struct span {
    const char *data;
    size_t length;
} span;

static int is_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static span trim(span s) {
    while (s.length > 0 && is_blank(s.data[0])) {
        s.data++;
        s.length--;
    }
    while (s.length > 0 && is_blank(s.data[s.length - 1])) {
        s.length--;
    }
    return s;
}

static int equals_ignoring_case(span s, const char *word) {
    if (strlen(word) != s.length) {
        return 0;
    }
    for (size_t i = 0; i < s.length; i++) {
        if (ascii_lower(s.data[i]) != ascii_lower(word[i])) {
            return 0;
        }
    }
    return 1;
}

static int spans_equal_ignoring_case(span a, span b) {
    if (a.length != b.length) {
        return 0;
    }
    for (size_t i = 0; i < a.length; i++) {
        if (ascii_lower(a.data[i]) != ascii_lower(b.data[i])) {
            return 0;
        }
    }
    return 1;
}

static int has_bom(span text) {
    return text.length >= UTF8_BOM_LENGTH && memcmp(text.data, UTF8_BOM, UTF8_BOM_LENGTH) == 0;
}

static span span_of(const char *text) {
    span s = {text, strlen(text)};
    return s;
}

/* Copies s into out, cut to fit: only for naming a setting in an issue. */
static void copy_cut(span s, char *out, size_t out_size) {
    const size_t length = s.length < out_size - 1 ? s.length : out_size - 1;
    memcpy(out, s.data, length);
    out[length] = '\0';
}

/* Copies s into out; 0 if it does not fit. */
static int copy_whole(span s, char *out, size_t out_size) {
    if (s.length >= out_size) {
        return 0;
    }
    memcpy(out, s.data, s.length);
    out[s.length] = '\0';
    return 1;
}

static int is_control(char c) {
    const unsigned char byte = (unsigned char)c;
    return byte < (unsigned char)' ' || byte == ASCII_DELETE;
}

/* Printable ASCII without blanks: what a URL or a token is made of. */
static int is_visible_ascii(span s) {
    for (size_t i = 0; i < s.length; i++) {
        const unsigned char byte = (unsigned char)s.data[i];
        if (byte <= (unsigned char)' ' || byte >= ASCII_DELETE) {
            return 0;
        }
    }
    return 1;
}

static int has_control(span s) {
    for (size_t i = 0; i < s.length; i++) {
        if (is_control(s.data[i])) {
            return 1;
        }
    }
    return 0;
}

/* A file in the Skiff folder: no folder of its own, no device ("ms0:"), not "." or "..". */
static int is_plain_file_name(span s) {
    return memchr(s.data, '/', s.length) == NULL && memchr(s.data, '\\', s.length) == NULL &&
           memchr(s.data, ':', s.length) == NULL && !has_control(s) &&
           !equals_ignoring_case(s, ".") && !equals_ignoring_case(s, "..");
}

static int is_header_name(span s) {
    for (size_t i = 0; i < s.length; i++) {
        const char c = s.data[i];
        const int alphanumeric =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alphanumeric && strchr(HEADER_NAME_SYMBOLS, c) == NULL) {
            return 0;
        }
    }
    return 1;
}

/* Tabs and printable characters, UTF-8 included: what a header value may hold. */
static int is_header_value(span s) {
    for (size_t i = 0; i < s.length; i++) {
        if (is_control(s.data[i]) && s.data[i] != '\t') {
            return 0;
        }
    }
    return 1;
}

typedef struct parser {
    skiff_config *config;
    skiff_config_issue *issue;
    span section;
    int line;
    /* One bit per KNOWN_KEYS entry already set. */
    unsigned seen;
} parser;

static skiff_err fail(parser *p, skiff_err err, span key) {
    if (p->issue != NULL) {
        p->issue->line = p->line;
        copy_cut(p->section, p->issue->section, sizeof p->issue->section);
        copy_cut(key, p->issue->key, sizeof p->issue->key);
    }
    return err;
}

static int parse_version(span value) {
    char digits[SKIFF_CONFIG_NAME_MAX];
    if (value.length == 0 || !copy_whole(value, digits, sizeof digits) ||
        strspn(digits, "0123456789") != value.length) {
        return 0;
    }
    errno = 0;
    const unsigned long version = strtoul(digits, NULL, DECIMAL_BASE);
    return errno == 0 && version == SKIFF_CONFIG_VERSION;
}

static int store_value(const known_key *known, span value, skiff_config *config) {
    if (known->kind == VALUE_VERSION) {
        return parse_version(value);
    }
    char *field = (char *)config + known->offset;
    if (!copy_whole(value, field, known->size)) {
        return 0;
    }
    if (value.length == 0) {
        return 1;
    }
    switch (known->kind) {
    case VALUE_URL:
        return is_visible_ascii(value) && skiff_http_url_scheme_valid(field);
    case VALUE_TOKEN:
        return is_visible_ascii(value);
    case VALUE_FILE_NAME:
        return is_plain_file_name(value);
    case VALUE_VERSION:
        break;
    }
    return 0;
}

static int is_reserved_header(span name) {
    for (size_t i = 0; i < sizeof RESERVED_HEADERS / sizeof RESERVED_HEADERS[0]; i++) {
        if (equals_ignoring_case(name, RESERVED_HEADERS[i])) {
            return 1;
        }
    }
    return 0;
}

static skiff_err add_header(parser *p, span name, span value) {
    skiff_config *config = p->config;
    for (size_t i = 0; i < config->header_count; i++) {
        if (spans_equal_ignoring_case(span_of(config->headers[i].name), name)) {
            return fail(p, SKIFF_ERR_CONFIG_INVALID_VALUE, name);
        }
    }
    if (config->header_count == SKIFF_CONFIG_HEADERS_MAX || !is_header_name(name) ||
        is_reserved_header(name) || value.length == 0 || !is_header_value(value)) {
        return fail(p, SKIFF_ERR_CONFIG_INVALID_VALUE, name);
    }
    skiff_config_header *header = &config->headers[config->header_count];
    if (!copy_whole(name, header->name, sizeof header->name) ||
        !copy_whole(value, header->value, sizeof header->value)) {
        return fail(p, SKIFF_ERR_CONFIG_INVALID_VALUE, name);
    }
    const skiff_http_header view = {header->name, header->value};
    if (!skiff_http_headers_valid(&view, 1)) {
        return fail(p, SKIFF_ERR_CONFIG_INVALID_VALUE, name);
    }
    config->header_count++;
    return SKIFF_OK;
}

static void note_unknown(parser *p, span key) {
    skiff_config *config = p->config;
    if (config->unknown_count == 0) {
        config->first_unknown.line = p->line;
        copy_cut(p->section, config->first_unknown.section, sizeof config->first_unknown.section);
        copy_cut(key, config->first_unknown.key, sizeof config->first_unknown.key);
    }
    config->unknown_count++;
}

static skiff_err parse_setting(parser *p, span key, span value) {
    if (equals_ignoring_case(p->section, SECTION_HEADERS)) {
        return add_header(p, key, value);
    }
    for (int i = 0; i < KNOWN_KEY_COUNT; i++) {
        const known_key *known = &KNOWN_KEYS[i];
        if (!equals_ignoring_case(p->section, known->section) ||
            !equals_ignoring_case(key, known->key)) {
            continue;
        }
        const unsigned bit = 1U << (unsigned)i;
        if ((p->seen & bit) != 0 || !store_value(known, value, p->config)) {
            return fail(p, SKIFF_ERR_CONFIG_INVALID_VALUE, key);
        }
        p->seen |= bit;
        return SKIFF_OK;
    }
    note_unknown(p, key);
    return SKIFF_OK;
}

static skiff_err parse_line(parser *p, span line) {
    const span none = {"", 0};
    if (line.length == 0 || line.data[0] == '#' || line.data[0] == ';') {
        return SKIFF_OK;
    }
    if (line.data[0] == '[') {
        span name = {line.data + 1, line.length - 1};
        if (line.length < 2 || line.data[line.length - 1] != ']') {
            return fail(p, SKIFF_ERR_CONFIG_PARSE, none);
        }
        name.length--;
        name = trim(name);
        if (name.length == 0 || name.length >= SKIFF_CONFIG_NAME_MAX) {
            return fail(p, SKIFF_ERR_CONFIG_PARSE, none);
        }
        p->section = name;
        return SKIFF_OK;
    }
    const char *equals = memchr(line.data, '=', line.length);
    if (equals == NULL) {
        return fail(p, SKIFF_ERR_CONFIG_PARSE, none);
    }
    const span key = trim((span){line.data, (size_t)(equals - line.data)});
    const span value = trim((span){equals + 1, line.length - (size_t)(equals - line.data) - 1});
    if (key.length == 0) {
        return fail(p, SKIFF_ERR_CONFIG_PARSE, none);
    }
    return parse_setting(p, key, value);
}

/* The client certificate is useless without its key and the other way round. */
static skiff_err check_mtls_pair(parser *p) {
    const skiff_config *config = p->config;
    const int has_cert = config->cert_file[0] != '\0';
    const int has_key = config->key_file[0] != '\0';
    if (has_cert == has_key) {
        return SKIFF_OK;
    }
    p->line = 0;
    p->section = span_of(SECTION_MTLS);
    return fail(p, SKIFF_ERR_CONFIG_MISSING_KEY, span_of(has_cert ? KEY_KEY_FILE : KEY_CERT_FILE));
}

skiff_err skiff_config_parse(const char *text, size_t length, skiff_config *out,
                             skiff_config_issue *issue) {
    if (issue != NULL) {
        memset(issue, 0, sizeof *issue);
    }
    if (out == NULL || (text == NULL && length > 0)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    parser p = {out, issue, {"", 0}, 0, 0};
    if (length > SKIFF_CONFIG_TEXT_MAX || (length > 0 && memchr(text, '\0', length) != NULL)) {
        return fail(&p, SKIFF_ERR_CONFIG_PARSE, (span){"", 0});
    }
    size_t position = has_bom((span){text, length}) ? UTF8_BOM_LENGTH : 0;
    skiff_err err = SKIFF_OK;
    while (err == SKIFF_OK && position < length) {
        const char *start = text + position;
        const char *newline = memchr(start, '\n', length - position);
        const size_t line_length = newline != NULL ? (size_t)(newline - start) : length - position;
        position += line_length + (newline != NULL ? 1 : 0);
        p.line++;
        err = parse_line(&p, trim((span){start, line_length}));
    }
    if (err == SKIFF_OK) {
        err = check_mtls_pair(&p);
    }
    if (err != SKIFF_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}

size_t skiff_config_headers(const skiff_config *config, skiff_http_header *out, size_t capacity) {
    if (config == NULL || out == NULL) {
        return 0;
    }
    size_t count = 0;
    for (; count < config->header_count && count < capacity; count++) {
        out[count].name = config->headers[count].name;
        out[count].value = config->headers[count].value;
    }
    return count;
}

/* ---- Editing ---- */

/* A section or key that parses back as itself. */
static int is_settable_name(const char *name) {
    const span s = span_of(name);
    return s.length > 0 && trim(s).length == s.length && strpbrk(name, "=[]\r\n") == NULL &&
           name[0] != '#' && name[0] != ';';
}

static int is_settable_value(const char *value) {
    const span s = span_of(value);
    return strpbrk(value, "\r\n") == NULL && trim(s).length == s.length;
}

/* Where the edit goes, found in one pass over the lines. */
typedef struct edit_plan {
    const char *eol;
    int section_found;
    /* The key's line: from its start to the end of its value (before any CR). */
    int key_found;
    size_t key_line_start;
    size_t key_value_end;
    /* Where a new key goes: just after the section's last setting (or its header). */
    size_t insert_at;
    int insert_needs_eol;
} edit_plan;

static void plan_edit(span text, const char *section, const char *key, edit_plan *plan) {
    memset(plan, 0, sizeof *plan);
    const char *first_newline = memchr(text.data, '\n', text.length);
    plan->eol = first_newline != NULL && first_newline > text.data && first_newline[-1] == '\r'
                    ? EOL_CRLF
                    : EOL_LF;
    int in_section = 0;
    int left_first_instance = 0;
    size_t position = has_bom(text) ? UTF8_BOM_LENGTH : 0;
    while (position < text.length) {
        const char *start = text.data + position;
        const char *newline = memchr(start, '\n', text.length - position);
        const size_t line_length =
            newline != NULL ? (size_t)(newline - start) : text.length - position;
        const size_t next = position + line_length + (newline != NULL ? 1 : 0);
        const span line = trim((span){start, line_length});
        if (line.length >= 2 && line.data[0] == '[' && line.data[line.length - 1] == ']') {
            const int matches =
                equals_ignoring_case(trim((span){line.data + 1, line.length - 2}), section);
            left_first_instance = left_first_instance || (in_section && !matches);
            in_section = matches;
            if (matches && !plan->section_found) {
                plan->section_found = 1;
                plan->insert_at = next;
                plan->insert_needs_eol = newline == NULL;
            }
        } else if (in_section && line.length > 0 && line.data[0] != '#' && line.data[0] != ';') {
            const char *equals = memchr(line.data, '=', line.length);
            if (equals != NULL && !plan->key_found &&
                equals_ignoring_case(trim((span){line.data, (size_t)(equals - line.data)}), key)) {
                plan->key_found = 1;
                plan->key_line_start = position;
                plan->key_value_end = (size_t)(line.data - text.data) + line.length;
            }
            if (equals != NULL && !left_first_instance) {
                plan->insert_at = next;
                plan->insert_needs_eol = newline == NULL;
            }
        }
        position = next;
    }
}

typedef struct builder {
    char *out;
    size_t size;
    size_t used;
    int fits;
} builder;

static void put(builder *b, const char *data, size_t length) {
    if (!b->fits || length >= b->size - b->used) {
        b->fits = 0;
        return;
    }
    memcpy(b->out + b->used, data, length);
    b->used += length;
    b->out[b->used] = '\0';
}

static void put_text(builder *b, const char *text) { put(b, text, strlen(text)); }

static void put_setting(builder *b, const char *key, const char *value, const char *eol) {
    put_text(b, key);
    put_text(b, value[0] != '\0' ? " = " : " =");
    put_text(b, value);
    put_text(b, eol);
}

skiff_err skiff_config_set(const char *text, size_t length, const char *section, const char *key,
                           const char *value, char *out, size_t out_size, size_t *out_length,
                           skiff_config_issue *issue) {
    if (issue != NULL) {
        memset(issue, 0, sizeof *issue);
    }
    if ((text == NULL && length > 0) || section == NULL || key == NULL || value == NULL ||
        out == NULL || out_size == 0 || out_length == NULL || !is_settable_name(section) ||
        !is_settable_name(key) || !is_settable_value(value)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    /* out is written while text is still being read. */
    const uintptr_t text_start = (uintptr_t)text;
    const uintptr_t out_start = (uintptr_t)out;
    if (text != NULL && length > 0 && text_start < out_start + out_size &&
        out_start < text_start + length) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const span source = {text != NULL ? text : "", length};
    edit_plan plan;
    plan_edit(source, section, key, &plan);
    builder b = {out, out_size, 0, 1};
    out[0] = '\0';
    if (plan.key_found) {
        const char *line = source.data + plan.key_line_start;
        const char *equals = memchr(line, '=', plan.key_value_end - plan.key_line_start);
        const size_t through_equals = (size_t)(equals - source.data) + 1;
        put(&b, source.data, through_equals);
        if (value[0] != '\0') {
            put_text(&b, through_equals < plan.key_value_end && source.data[through_equals] != ' '
                             ? ""
                             : " ");
        }
        put_text(&b, value);
        put(&b, source.data + plan.key_value_end, source.length - plan.key_value_end);
    } else if (plan.section_found) {
        put(&b, source.data, plan.insert_at);
        if (plan.insert_needs_eol) {
            put_text(&b, plan.eol);
        }
        put_setting(&b, key, value, plan.eol);
        put(&b, source.data + plan.insert_at, source.length - plan.insert_at);
    } else {
        put(&b, source.data, source.length);
        if (source.length > 0 && source.data[source.length - 1] != '\n') {
            put_text(&b, plan.eol);
        }
        if (source.length > 0) {
            put_text(&b, plan.eol);
        }
        put_text(&b, "[");
        put_text(&b, section);
        put_text(&b, "]");
        put_text(&b, plan.eol);
        put_setting(&b, key, value, plan.eol);
    }
    if (!b.fits) {
        out[0] = '\0';
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    /* What is saved must load: an address without a scheme, a token with a blank or a header Skiff
     * sets itself is refused here, before it reaches the Memory Stick. */
    skiff_config parsed;
    const skiff_err err = skiff_config_parse(out, b.used, &parsed, issue);
    if (err != SKIFF_OK) {
        out[0] = '\0';
        return err;
    }
    *out_length = b.used;
    return SKIFF_OK;
}

/* ---- Loading and saving ---- */

/* The two files a save goes through, next to config.ini. */
typedef struct save_paths {
    /* Written and synced first; never trusted, since a power cut can leave it cut short. */
    char draft[SKIFF_CONFIG_PATH_MAX];
    /* The draft renamed once it is complete: if it exists, it is a whole config.ini. */
    char pending[SKIFF_CONFIG_PATH_MAX];
} save_paths;

static int with_suffix(const char *path, const char *suffix, char *out, size_t out_size) {
    const int written = snprintf(out, out_size, "%s%s", path, suffix);
    return written > 0 && (size_t)written < out_size;
}

static int make_save_paths(const char *path, save_paths *paths) {
    return with_suffix(path, SKIFF_CONFIG_DRAFT_SUFFIX, paths->draft, sizeof paths->draft) &&
           with_suffix(path, SKIFF_CONFIG_NEW_SUFFIX, paths->pending, sizeof paths->pending);
}

static skiff_err read_all(skiff_storage *storage, const char *path, char *text, size_t text_size,
                          size_t *length) {
    skiff_file *file = NULL;
    skiff_err err = skiff_storage_open(storage, path, SKIFF_FILE_READ, 0, &file);
    if (err != SKIFF_OK) {
        return err;
    }
    size_t used = 0;
    size_t got = 1;
    while (err == SKIFF_OK && got > 0 && used < text_size - 1) {
        err = skiff_file_read(file, text + used, text_size - 1 - used, &got);
        used += err == SKIFF_OK ? got : 0;
    }
    if (err == SKIFF_OK && used == text_size - 1) {
        char extra = 0;
        err = skiff_file_read(file, &extra, sizeof extra, &got);
        if (err == SKIFF_OK && got > 0) {
            err = SKIFF_ERR_CONFIG_PARSE;
        }
    }
    const skiff_err close_err = skiff_file_close(file);
    if (err == SKIFF_OK) {
        err = close_err;
    }
    if (err != SKIFF_OK) {
        text[0] = '\0';
        return err;
    }
    text[used] = '\0';
    *length = used;
    return SKIFF_OK;
}

/* Makes a rename or remove durable before the next step relies on it. The PSP flushes a whole
 * device on any file's sync (sceIoSync), directory entries included, so syncing the file a rename
 * produced commits the rename. */
static skiff_err flush_device(skiff_storage *storage, const char *path) {
    skiff_file *file = NULL;
    skiff_err err = skiff_storage_open(storage, path, SKIFF_FILE_READ, 0, &file);
    if (err != SKIFF_OK) {
        return err;
    }
    err = skiff_file_sync(file);
    const skiff_err close_err = skiff_file_close(file);
    return err != SKIFF_OK ? err : close_err;
}

/*
 * Finishes or undoes a save cut short, so config.ini is the newest complete settings. A draft is
 * dropped (it may be cut short). A pending file is complete: without config.ini the save was cut
 * between the remove and the rename, so the rename is finished; beside config.ini it was cut
 * before the remove, so the old file stands. Removing a file nobody trusts is only tidying, so a
 * failure there never stops config.ini from loading.
 */
static skiff_err recover(skiff_storage *storage, const char *path, const save_paths *paths) {
    (void)skiff_storage_remove(storage, paths->draft);
    uint64_t size = 0;
    const skiff_err err = skiff_storage_size(storage, path, &size);
    const skiff_err pending_err = skiff_storage_size(storage, paths->pending, &size);
    if (err == SKIFF_ERR_STORAGE_NOT_FOUND) {
        if (pending_err == SKIFF_ERR_STORAGE_NOT_FOUND) {
            return SKIFF_OK;
        }
        if (pending_err != SKIFF_OK) {
            return pending_err;
        }
        const skiff_err rename_err = skiff_storage_rename(storage, paths->pending, path);
        return rename_err == SKIFF_OK ? flush_device(storage, path) : rename_err;
    }
    if (err == SKIFF_OK && pending_err != SKIFF_ERR_STORAGE_NOT_FOUND) {
        (void)skiff_storage_remove(storage, paths->pending);
    }
    return err;
}

skiff_err skiff_config_load(skiff_storage *storage, const char *path, char *text, size_t text_size,
                            size_t *length) {
    if (text != NULL && text_size > 0) {
        text[0] = '\0';
    }
    if (length != NULL) {
        *length = 0;
    }
    save_paths paths;
    if (storage == NULL || path == NULL || text == NULL || text_size == 0 || length == NULL ||
        !make_save_paths(path, &paths)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_err err = recover(storage, path, &paths);
    if (err == SKIFF_OK) {
        err = read_all(storage, path, text, text_size, length);
    }
    return err == SKIFF_ERR_STORAGE_NOT_FOUND ? SKIFF_OK : err;
}

static skiff_err write_synced(skiff_storage *storage, const char *path, const char *text,
                              size_t length) {
    skiff_file *file = NULL;
    skiff_err err = skiff_storage_open(storage, path, SKIFF_FILE_REPLACE, 0, &file);
    if (err != SKIFF_OK) {
        return err;
    }
    if (length > 0) {
        err = skiff_file_write(file, text, length);
    }
    if (err == SKIFF_OK) {
        err = skiff_file_sync(file);
    }
    const skiff_err close_err = skiff_file_close(file);
    return err != SKIFF_OK ? err : close_err;
}

skiff_err skiff_config_save(skiff_storage *storage, const char *path, const char *text,
                            size_t length) {
    save_paths paths;
    if (storage == NULL || path == NULL || (text == NULL && length > 0) ||
        length > SKIFF_CONFIG_TEXT_MAX || !make_save_paths(path, &paths)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    /* A pending file from an earlier cut save may be the only copy of the settings. */
    skiff_err err = recover(storage, path, &paths);
    if (err != SKIFF_OK && err != SKIFF_ERR_STORAGE_NOT_FOUND) {
        return err;
    }
    err = write_synced(storage, paths.draft, text, length);
    if (err == SKIFF_OK) {
        err = skiff_storage_rename(storage, paths.draft, paths.pending);
    }
    if (err != SKIFF_OK) {
        (void)skiff_storage_remove(storage, paths.draft);
        return err;
    }
    /* From here the pending file is complete. Whatever fails, it stays for the next load to
     * judge: a failed remove may still have taken config.ini with it. config.ini is only removed
     * once the pending file's name is on the device. */
    err = flush_device(storage, paths.pending);
    if (err == SKIFF_OK) {
        err = skiff_storage_remove(storage, path);
        err = err == SKIFF_ERR_STORAGE_NOT_FOUND ? SKIFF_OK : err;
    }
    if (err == SKIFF_OK) {
        err = skiff_storage_rename(storage, paths.pending, path);
    }
    return err == SKIFF_OK ? flush_device(storage, path) : err;
}
