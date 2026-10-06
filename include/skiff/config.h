#ifndef SKIFF_CONFIG_H
#define SKIFF_CONFIG_H

/*
 * PSP/GAME/Skiff/config.ini: the server, its certificates, the token and custom headers. The player
 * may edit it on a computer (docs/guide/04-connect-to-romm.md, 05-secure-connections.md) and Skiff
 * edits it too (pairing saves the token, Settings the address), so Skiff changes only the line of
 * the key it sets: comments, unknown keys and the player's order survive.
 *
 *   [skiff]    version     schema version; absent means 1
 *   [server]   url         http:// or https:// address of RomM
 *              ca_file     file name in the Skiff folder; empty for the bundled public CAs
 *   [auth]     token       RomM client API token
 *   [mtls]     cert_file   client certificate and key, file names in the Skiff folder
 *              key_file
 *   [headers]  <name>      sent on every request (e.g. CF-Access-Client-Id)
 *
 * Lines are "key = value"; '#' or ';' at the start of a line begins a comment. There are no inline
 * comments, because a token or header value may contain '#'. Section and key names ignore case.
 * An empty value means the setting is not set.
 */

#include <stddef.h>

#include "skiff/error.h"
#include "skiff/storage.h"
#include "skiff/transport.h"

#define SKIFF_CONFIG_FILE_NAME "config.ini"
/* Saving writes "<path>.tmp", renames it "<path>.new" once synced, then replaces config.ini; see
 * skiff_config_save(). */
#define SKIFF_CONFIG_DRAFT_SUFFIX ".tmp"
#define SKIFF_CONFIG_NEW_SUFFIX ".new"
#define SKIFF_CONFIG_VERSION 1
/* A config.ini larger than this is not one Skiff or a player wrote: SKIFF_ERR_CONFIG_PARSE. */
#define SKIFF_CONFIG_TEXT_MAX 8192
/* Room for the path of config.ini or its .new file, with the terminator. */
#define SKIFF_CONFIG_PATH_MAX 256

/* Buffer sizes, terminator included. A longer value is refused, never cut. */
#define SKIFF_CONFIG_URL_MAX 256
#define SKIFF_CONFIG_TOKEN_MAX 128
#define SKIFF_CONFIG_FILE_NAME_MAX 64
#define SKIFF_CONFIG_NAME_MAX 64
#define SKIFF_CONFIG_HEADER_VALUE_MAX 256
#define SKIFF_CONFIG_HEADERS_MAX 8

typedef struct skiff_config_header {
    char name[SKIFF_CONFIG_NAME_MAX];
    char value[SKIFF_CONFIG_HEADER_VALUE_MAX];
} skiff_config_header;

/* Where a setting is, so the player can be told which one to fix. line is 1-based, 0 when the
 * problem is a setting that is missing. */
typedef struct skiff_config_issue {
    int line;
    char section[SKIFF_CONFIG_NAME_MAX];
    char key[SKIFF_CONFIG_NAME_MAX];
} skiff_config_issue;

typedef struct skiff_config {
    char server_url[SKIFF_CONFIG_URL_MAX];
    char ca_file[SKIFF_CONFIG_FILE_NAME_MAX];
    char token[SKIFF_CONFIG_TOKEN_MAX];
    char cert_file[SKIFF_CONFIG_FILE_NAME_MAX];
    char key_file[SKIFF_CONFIG_FILE_NAME_MAX];
    skiff_config_header headers[SKIFF_CONFIG_HEADERS_MAX];
    size_t header_count;
    /* Keys Skiff does not know are ignored (a newer Skiff may have written them), but a typo such
     * as "ca-file" would silently do nothing, so the first is kept for a log warning. */
    int unknown_count;
    skiff_config_issue first_unknown;
} skiff_config;

/*
 * Parses config.ini's text into out (an empty text gives an empty config). On error out is empty
 * and issue, when not NULL, names the line and setting:
 *   - SKIFF_ERR_CONFIG_PARSE: text over SKIFF_CONFIG_TEXT_MAX or with a NUL byte, a line without
 *     '=', a section header without its ']' or name;
 *   - SKIFF_ERR_CONFIG_MISSING_KEY: cert_file without key_file or the reverse (the issue names the
 *     missing one);
 *   - SKIFF_ERR_CONFIG_INVALID_VALUE: a setting given twice, a value too long, a version other than
 *     SKIFF_CONFIG_VERSION, a URL without http:// or https:// or with blanks, a token with blanks
 * or control characters, a file name with a path in it, a header Skiff sets itself (Authorization,
 *     Host, Range, If-Range), a header name that is not an HTTP token, an empty header value, or
 *     more than SKIFF_CONFIG_HEADERS_MAX headers;
 *   - SKIFF_ERR_INVALID_ARG: NULL text with a non-zero length, or a NULL out.
 */
skiff_err skiff_config_parse(const char *text, size_t length, skiff_config *out,
                             skiff_config_issue *issue);

/* Points out[i] at config's headers, for skiff_curl_config.default_headers; returns how many, at
 * most capacity. The views live as long as config. */
size_t skiff_config_headers(const skiff_config *config, skiff_http_header *out, size_t capacity);

/*
 * Writes text with key in section set to value into out (with its length in *out_length): the
 * key's line is rewritten in place (its first occurrence, keeping the key as written); a missing
 * key is added after the last setting of its section, a missing section at the end. Line endings
 * follow the text's first line (CRLF for a file saved on Windows). Returns SKIFF_ERR_INVALID_ARG
 * for a NULL argument, text and out overlapping (out must be another buffer), a section or key
 * that would not parse back (empty, blanks at either end, '=', '[', ']', a line break, or a leading
 * '#' or ';'), a value with a line break or blanks at either end; SKIFF_ERR_BUFFER_TOO_SMALL if the
 * result does not fit. The result is parsed before it is returned, so it always loads back: when
 * skiff_config_parse() refuses it (a bad value for this key, or a damaged line elsewhere in text),
 * that error is returned with its issue and out is left empty. A client certificate and its key
 * cannot be set one at a time; players set them by hand.
 */
skiff_err skiff_config_set(const char *text, size_t length, const char *section, const char *key,
                           const char *value, char *out, size_t out_size, size_t *out_length,
                           skiff_config_issue *issue);

/*
 * Reads config.ini into text (text_size should be SKIFF_CONFIG_TEXT_MAX + 1) and terminates it.
 * First finishes or undoes a save cut short by a power loss (see skiff_config_save()): a .tmp file
 * is deleted; a .new file without config.ini becomes config.ini; a .new file beside config.ini is
 * deleted when it can be, and config.ini is read either way. No file at all is an empty config
 * (length 0). SKIFF_ERR_CONFIG_PARSE when the file does not fit, SKIFF_ERR_INVALID_ARG for a NULL
 * argument or a path too long for SKIFF_CONFIG_PATH_MAX, otherwise the storage's error.
 */
skiff_err skiff_config_load(skiff_storage *storage, const char *path, char *text, size_t text_size,
                            size_t *length);

/*
 * Replaces config.ini with text. FAT cannot replace a file in one step, so: write and sync
 * "<path>.tmp", rename it "<path>.new" (so a .new file is always complete) and sync the device,
 * remove config.ini, rename .new to config.ini and sync again. A power cut leaves the old file, or
 * a complete .new file the next load puts in place; a cut .tmp file is never used. A save first
 * finishes an earlier cut save the same way. Returns SKIFF_ERR_INVALID_ARG for a NULL argument, a
 * path too long or a text over SKIFF_CONFIG_TEXT_MAX (it could not be loaded back), otherwise the
 * storage's error; after a failure the next load still finds either the old settings or the new
 * ones, whole.
 */
skiff_err skiff_config_save(skiff_storage *storage, const char *path, const char *text,
                            size_t length);

#endif
