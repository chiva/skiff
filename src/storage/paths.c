#include <stdio.h>
#include <string.h>

#include "skiff/storage_paths.h"

#define DEVICE_SEPARATOR ':'
#define PATH_SEPARATOR '/'
#define REPLACEMENT '_'
/* Characters FAT does not allow in a name, besides control characters. */
#define FAT_FORBIDDEN "\\/:*?\"<>|"
#define ASCII_DELETE 0x7F
#define ASCII_FIRST_PRINTABLE 0x20
#define EXTENSION_SEPARATOR '.'

/* Lead and continuation bytes of UTF-8 (RFC 3629): no overlong forms, no surrogates, nothing past
 * U+10FFFF. */
#define UTF8_CONTINUATION_MASK 0xC0U
#define UTF8_CONTINUATION 0x80U
#define UTF8_TWO_FIRST 0xC2U
#define UTF8_TWO_LAST 0xDFU
#define UTF8_THREE_FIRST 0xE0U
#define UTF8_THREE_LAST 0xEFU
#define UTF8_FOUR_FIRST 0xF0U
#define UTF8_FOUR_LAST 0xF4U
#define UTF8_E0_SECOND_MIN 0xA0U
#define UTF8_ED_SECOND_MAX 0x9FU
#define UTF8_F0_SECOND_MIN 0x90U
#define UTF8_F4_SECOND_MAX 0x8FU
#define UTF8_SECOND_MIN 0x80U
#define UTF8_SECOND_MAX 0xBFU
#define UTF8_E0 0xE0U
#define UTF8_ED 0xEDU
#define UTF8_F0 0xF0U
#define UTF8_F4 0xF4U

/* Windows and FAT tools treat these names as devices, whatever the extension. */
static const char *const DOS_DEVICES[] = {
    "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
    "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

/* ---- Roots ---- */

/* Copies a and b into out; 0 if they do not fit. */
static int join(const char *a, const char *b, char *out, size_t out_size) {
    const int written = snprintf(out, out_size, "%s%s", a, b);
    return written >= 0 && (size_t)written < out_size;
}

skiff_err skiff_storage_roots_init(const char *device_root, const char *app_dir,
                                   skiff_storage_roots *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (device_root == NULL || app_dir == NULL || out == NULL || device_root[0] == '\0' ||
        app_dir[0] == '\0' || device_root[strlen(device_root) - 1] == PATH_SEPARATOR ||
        app_dir[strlen(app_dir) - 1] == PATH_SEPARATOR) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (!join(app_dir, "", out->app, sizeof out->app) ||
        !join(device_root, SKIFF_STORAGE_GAMES_FOLDER, out->games, sizeof out->games) ||
        !join(device_root, SKIFF_STORAGE_SAVES_FOLDER, out->saves, sizeof out->saves)) {
        memset(out, 0, sizeof *out);
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    return SKIFF_OK;
}

skiff_err skiff_storage_roots_from_program(const char *program_path, skiff_storage_roots *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (program_path == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const char *separator = strchr(program_path, DEVICE_SEPARATOR);
    const char *first_slash = strchr(program_path, PATH_SEPARATOR);
    const char *last_slash = strrchr(program_path, PATH_SEPARATOR);
    if (separator == NULL || separator == program_path || first_slash == NULL ||
        first_slash < separator) {
        return SKIFF_ERR_INVALID_ARG;
    }
    char device[SKIFF_STORAGE_PATH_MAX];
    char app[SKIFF_STORAGE_PATH_MAX];
    const size_t device_length = (size_t)(separator - program_path) + 1;
    const size_t app_length = (size_t)(last_slash - program_path);
    if (device_length >= sizeof device || app_length >= sizeof app) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(device, program_path, device_length);
    device[device_length] = '\0';
    memcpy(app, program_path, app_length);
    app[app_length] = '\0';
    return skiff_storage_roots_init(device, app, out);
}

/* The part of logical after its root, or NULL for an unknown root. */
static const char *after_root(const skiff_storage_roots *roots, const char *logical,
                              const char **root) {
    static const char *const NAMES[] = {SKIFF_STORAGE_ROOT_APP, SKIFF_STORAGE_ROOT_GAMES,
                                        SKIFF_STORAGE_ROOT_SAVES};
    const char *const folders[] = {roots->app, roots->games, roots->saves};
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        const size_t length = strlen(NAMES[i]);
        if (strncmp(logical, NAMES[i], length) == 0) {
            *root = folders[i];
            return logical + length;
        }
    }
    return NULL;
}

static int is_control(char c) {
    const unsigned char byte = (unsigned char)c;
    return byte < ASCII_FIRST_PRINTABLE || byte == ASCII_DELETE;
}

/* "/a/b": parts that are plain names, so the path stays inside its root. */
static int plain_parts(const char *rest) {
    while (*rest != '\0') {
        if (*rest != PATH_SEPARATOR) {
            return 0;
        }
        const char *part = rest + 1;
        const size_t length = strcspn(part, "/");
        /* FAT drops blanks and dots at the end of a name, so ".. ." would become "..": a part may
         * not end in either, which also refuses "." and "..". */
        if (length == 0 || part[length - 1] == '.' || part[length - 1] == ' ') {
            return 0;
        }
        for (size_t i = 0; i < length; i++) {
            if (part[i] == '\\' || part[i] == DEVICE_SEPARATOR || is_control(part[i])) {
                return 0;
            }
        }
        rest = part + length;
    }
    return 1;
}

skiff_err skiff_storage_resolve(const skiff_storage_roots *roots, const char *logical, char *out,
                                size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (roots == NULL || logical == NULL || out == NULL || out_size == 0) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const char *root = NULL;
    const char *rest = after_root(roots, logical, &root);
    if (rest == NULL || root[0] == '\0' || !plain_parts(rest)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (!join(root, rest, out, out_size)) {
        out[0] = '\0';
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    return SKIFF_OK;
}

skiff_err skiff_storage_sibling_path(const char *program_path, const char *file_name, char *out,
                                     size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (program_path == NULL || file_name == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const char *last_separator = strrchr(program_path, PATH_SEPARATOR);
    if (last_separator == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    /* Keeps the trailing '/', so "ms0:/EBOOT.PBP" becomes "ms0:/result.txt". */
    const size_t directory_length = (size_t)(last_separator - program_path) + 1;
    const size_t name_length = strlen(file_name);
    if (directory_length + name_length + 1 > out_size) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(out, program_path, directory_length);
    memcpy(out + directory_length, file_name, name_length + 1);
    return SKIFF_OK;
}

/* ---- Safe names ---- */

static int is_continuation(unsigned char byte) {
    return (byte & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION;
}

/* Length of the valid UTF-8 character at text[0] (at most available bytes), 0 if it is not one. */
static size_t utf8_length(const unsigned char *text, size_t available) {
    const unsigned char lead = text[0];
    size_t length = 0;
    unsigned char second_min = UTF8_SECOND_MIN;
    unsigned char second_max = UTF8_SECOND_MAX;
    if (lead < UTF8_CONTINUATION) {
        return 1;
    }
    if (lead >= UTF8_TWO_FIRST && lead <= UTF8_TWO_LAST) {
        length = 2;
    } else if (lead >= UTF8_THREE_FIRST && lead <= UTF8_THREE_LAST) {
        length = 3;
        second_min = lead == UTF8_E0 ? UTF8_E0_SECOND_MIN : second_min;
        second_max = lead == UTF8_ED ? UTF8_ED_SECOND_MAX : second_max;
    } else if (lead >= UTF8_FOUR_FIRST && lead <= UTF8_FOUR_LAST) {
        length = 4;
        second_min = lead == UTF8_F0 ? UTF8_F0_SECOND_MIN : second_min;
        second_max = lead == UTF8_F4 ? UTF8_F4_SECOND_MAX : second_max;
    } else {
        return 0;
    }
    if (length > available || text[1] < second_min || text[1] > second_max) {
        return 0;
    }
    for (size_t i = 2; i < length; i++) {
        if (!is_continuation(text[i])) {
            return 0;
        }
    }
    return length;
}

static int is_blank(char c) { return c == ' ' || c == '\t'; }

static char ascii_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}

/* The name before its first dot, without trailing blanks, is a DOS device. */
static int is_dos_device(const char *name, size_t length) {
    const char *dot = memchr(name, EXTENSION_SEPARATOR, length);
    size_t base = dot != NULL ? (size_t)(dot - name) : length;
    while (base > 0 && is_blank(name[base - 1])) {
        base--;
    }
    for (size_t i = 0; i < sizeof DOS_DEVICES / sizeof DOS_DEVICES[0]; i++) {
        const char *device = DOS_DEVICES[i];
        size_t matched = 0;
        while (matched < base && device[matched] != '\0' &&
               ascii_upper(name[matched]) == device[matched]) {
            matched++;
        }
        if (matched == base && device[matched] == '\0') {
            return 1;
        }
    }
    return 0;
}

/* Copies name into work with every unsafe byte replaced; returns the length. */
static size_t clean_characters(const char *name, size_t length, char *work) {
    const unsigned char *bytes = (const unsigned char *)name;
    size_t used = 0;
    size_t at = 0;
    while (at < length) {
        const size_t character = utf8_length(bytes + at, length - at);
        if (character == 0 ||
            (character == 1 && (is_control(name[at]) || strchr(FAT_FORBIDDEN, name[at]) != NULL))) {
            work[used++] = REPLACEMENT;
            at++;
            continue;
        }
        memcpy(work + used, name + at, character);
        used += character;
        at += character;
    }
    return used;
}

/* Drops blanks and dots from the end of text[0..*length). */
static void trim_end(const char *text, size_t *length) {
    while (*length > 0 &&
           (is_blank(text[*length - 1]) || text[*length - 1] == EXTENSION_SEPARATOR)) {
        (*length)--;
    }
}

/* Backs length up to the start of a character, so a cut never splits one. */
static size_t character_boundary(const char *text, size_t length) {
    while (length > 0 && is_continuation((unsigned char)text[length])) {
        length--;
    }
    return length;
}

/* Shortens text[0..*length) to at most limit bytes, keeping a short extension. */
static void shorten(char *text, size_t *length, size_t limit) {
    if (*length <= limit) {
        return;
    }
    const char *dot = NULL;
    for (size_t i = *length; i > 1; i--) {
        if (text[i - 1] == EXTENSION_SEPARATOR) {
            dot = text + i - 1;
            break;
        }
    }
    const size_t extension =
        dot != NULL && (size_t)(text + *length - dot) <= SKIFF_STORAGE_EXTENSION_MAX
            ? (size_t)(text + *length - dot)
            : 0;
    size_t stem = character_boundary(text, limit - extension);
    trim_end(text, &stem);
    if (stem == 0) {
        text[0] = REPLACEMENT;
        stem = 1;
    }
    memmove(text + stem, text + *length - extension, extension);
    *length = stem + extension;
}

skiff_err skiff_storage_safe_name(const char *name, char *out, size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (name == NULL || out == NULL || out_size == 0) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const size_t input_length = strlen(name);
    if (input_length > SKIFF_STORAGE_NAME_INPUT_MAX) {
        return SKIFF_ERR_INVALID_ARG;
    }
    /* One more byte than the input, for a DOS device's leading '_'. */
    char work[SKIFF_STORAGE_NAME_INPUT_MAX + 2];
    size_t length = clean_characters(name, input_length, work + 1);
    size_t start = 1;
    while (start < length + 1 && is_blank(work[start])) {
        start++;
    }
    length -= start - 1;
    trim_end(work + start, &length);
    if (length == 0) {
        return SKIFF_ERR_INVALID_ARG;
    }
    shorten(work + start, &length, SKIFF_STORAGE_NAME_MAX - 1);
    /* Checked on the final name: shortening can trim a stem down to "CON". The '_' goes in the byte
     * kept free before the name; a name that no longer fits is shortened again, and now starts with
     * '_', so it cannot be a device name any more. */
    if (is_dos_device(work + start, length)) {
        start--;
        work[start] = REPLACEMENT;
        length++;
        shorten(work + start, &length, SKIFF_STORAGE_NAME_MAX - 1);
    }
    if (length + 1 > out_size) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(out, work + start, length);
    out[length] = '\0';
    return SKIFF_OK;
}
