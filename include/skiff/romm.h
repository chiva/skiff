#ifndef SKIFF_ROMM_H
#define SKIFF_ROMM_H

/*
 * A typed client for the RomM REST API over a skiff_transport: the server's version and whether
 * Skiff supports it, the PSP platform, its ROMs one page at a time, a ROM's files, and the URL a
 * file downloads from (skiff/download.h does the download). Responses are JSON, parsed with cJSON
 * one response at a time from a buffer of at most SKIFF_ROMM_BODY_MAX bytes: a larger response is
 * refused, never cut, so a PSP cannot run out of memory on a large library.
 *
 * Every call returns SKIFF_OK, the transport's error (1xx), the RomM error for an HTTP status
 * (skiff_http_status_error(): 401 → 200, 403 → 201, 404 → 202, 5xx → 203), or
 * SKIFF_ERR_ROMM_BAD_RESPONSE (204) for a body that is not the JSON RomM sends: a proxy's login
 * page, a cut or oversized response, a missing field, a value that does not fit its field.
 * SKIFF_ERR_NO_MEMORY when the response buffer cannot grow. A client is used by one thread at a
 * time.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/config.h"
#include "skiff/error.h"
#include "skiff/transport.h"

/* The oldest RomM Skiff works with: the device-code pairing and the per-file hashes arrived in
 * 5.3. */
#define SKIFF_ROMM_MIN_MAJOR 5
#define SKIFF_ROMM_MIN_MINOR 3
/* The newest RomM release line Skiff was tested against; a newer one gets a one-time notice. */
#define SKIFF_ROMM_TESTED_MAJOR 5
#define SKIFF_ROMM_TESTED_MINOR 3

/* ROMs per page. An unidentified ROM is about 2.6 KB of JSON and one with metadata (summary,
 * alternative names, screenshots) several times that, so 25 keeps a page well under the body cap
 * while a 20-line screen needs one request. */
#define SKIFF_ROMM_PAGE_SIZE 25
/* The largest response read: room for a page of 25 ROMs at 20 KB each. */
#define SKIFF_ROMM_BODY_MAX ((size_t)512 * 1024)
/* The most JSON values a response may hold, checked before cJSON builds its tree (about 44 bytes a
 * value on the PSP). RomM's responses run near one value per 20 bytes, so a full-size real response
 * fits; a body made of tiny values is refused instead of costing megabytes. A request then peaks
 * near 2 MB: the body, the tree and its strings. */
#define SKIFF_ROMM_JSON_NODES_MAX 32768
/* Files of one ROM kept by skiff_romm_get_rom(); a PSP game is one file. */
#define SKIFF_ROMM_FILES_MAX 16

/* Buffer sizes, terminator included. A longer value from RomM is refused (204), never cut. */
#define SKIFF_ROMM_VERSION_MAX 32
#define SKIFF_ROMM_SLUG_MAX 64
#define SKIFF_ROMM_NAME_MAX 256
/* FAT and ext4 both stop at 255 bytes per file name. */
#define SKIFF_ROMM_FILE_NAME_MAX 256
/* A request URL: the server address plus a path and query Skiff builds. */
#define SKIFF_ROMM_URL_MAX (SKIFF_CONFIG_URL_MAX + 256)
/* A download URL: every byte of a file name may become "%XX". */
#define SKIFF_ROMM_CONTENT_URL_MAX (SKIFF_CONFIG_URL_MAX + 64 + 3 * SKIFF_ROMM_FILE_NAME_MAX)
/* "Bearer " and a token from config.ini. */
#define SKIFF_ROMM_AUTHORIZATION_MAX (sizeof "Bearer " + SKIFF_CONFIG_TOKEN_MAX)

typedef struct skiff_romm_client {
    skiff_transport *transport; /* not owned */
    /* Without a trailing '/'. */
    char base_url[SKIFF_CONFIG_URL_MAX];
    /* "Bearer <token>", or empty without a token. Wiped by skiff_romm_client_clear(). */
    char authorization[SKIFF_ROMM_AUTHORIZATION_MAX];
} skiff_romm_client;

typedef struct skiff_romm_server {
    /* As RomM reports it, e.g. "5.3.1". */
    char version[SKIFF_ROMM_VERSION_MAX];
    /* 0 when the version does not start with major.minor (a development build); such a server is
     * allowed, with the newer-than-tested notice. */
    int version_known;
    int major;
    int minor;
    int patch;
    /* Newer release line than SKIFF_ROMM_TESTED_*: show the one-time notice. */
    int newer_than_tested;
} skiff_romm_server;

typedef struct skiff_romm_platform {
    uint64_t id;
    char slug[SKIFF_ROMM_SLUG_MAX];
    char name[SKIFF_ROMM_NAME_MAX];
    uint64_t rom_count;
} skiff_romm_platform;

/* A ROM as a list shows it. size and crc32 are the whole ROM's (one file for a PSP game). */
typedef struct skiff_romm_rom_summary {
    uint64_t id;
    uint64_t platform_id;
    /* The title RomM shows; empty when it has none, then show fs_name. */
    char name[SKIFF_ROMM_NAME_MAX];
    char fs_name[SKIFF_ROMM_FILE_NAME_MAX];
    uint64_t size;
    int has_crc32;
    uint32_t crc32;
    /* A folder of several files rather than one file. */
    int multiple_files;
} skiff_romm_rom_summary;

typedef struct skiff_romm_rom_page {
    /* ROMs on the platform, and where this page starts among them. */
    uint64_t total;
    uint64_t offset;
    size_t count;
    skiff_romm_rom_summary items[SKIFF_ROMM_PAGE_SIZE];
} skiff_romm_rom_page;

typedef struct skiff_romm_file {
    char file_name[SKIFF_ROMM_FILE_NAME_MAX];
    uint64_t size;
    int has_crc32;
    uint32_t crc32;
} skiff_romm_file;

typedef struct skiff_romm_rom {
    skiff_romm_rom_summary summary;
    /* Files RomM lists for the ROM; the first SKIFF_ROMM_FILES_MAX of them are in files. */
    size_t file_count;
    skiff_romm_file files[SKIFF_ROMM_FILES_MAX];
} skiff_romm_rom;

/*
 * Prepares client for the server at base_url (a trailing '/' is dropped), authenticating with token
 * (NULL or empty for none: the heartbeat needs none). Returns SKIFF_ERR_CONFIG_INVALID_VALUE for a
 * base_url without http:// or https:// or too long for SKIFF_CONFIG_URL_MAX, SKIFF_ERR_INVALID_ARG
 * for a NULL client, transport or base_url, or a token too long or with blanks or control
 * characters.
 */
skiff_err skiff_romm_client_init(skiff_romm_client *client, skiff_transport *transport,
                                 const char *base_url, const char *token);

/* Wipes the token from client. Does nothing for NULL. */
void skiff_romm_client_clear(skiff_romm_client *client);

/* Points *out at the client's Authorization header, for skiff_download_spec.headers; returns 1, or
 * 0 (and leaves *out alone) without a token. */
size_t skiff_romm_auth_header(const skiff_romm_client *client, skiff_http_header *out);

/*
 * Applies the version policy to a version string: below SKIFF_ROMM_MIN_* is
 * SKIFF_ERR_ROMM_UNSUPPORTED_VERSION (205); a newer release line than SKIFF_ROMM_TESTED_* sets
 * newer_than_tested; a version that does not start with major.minor is allowed as unknown. out is
 * filled in every case; SKIFF_ERR_ROMM_BAD_RESPONSE for an empty or over-long version.
 */
skiff_err skiff_romm_check_version(const char *version, skiff_romm_server *out);

/* GET /api/heartbeat (no token sent): the server's version, through skiff_romm_check_version(). */
skiff_err skiff_romm_heartbeat(skiff_romm_client *client, skiff_romm_server *out);

/* GET /api/platforms: the platform whose slug is slug (e.g. "psp"). SKIFF_ERR_ROMM_NOT_FOUND (202)
 * when RomM has none, as when its library has no folder for it. */
skiff_err skiff_romm_find_platform(skiff_romm_client *client, const char *slug,
                                   skiff_romm_platform *out);

/*
 * GET /api/roms: up to limit (1 to SKIFF_ROMM_PAGE_SIZE) ROMs of platform_id from offset, ordered
 * by name, without the per-library extras RomM adds by default. A page past the end is empty with
 * the total. A page that is not the one asked for (another offset, more ROMs than limit or than the
 * total leaves, a ROM of another platform) is SKIFF_ERR_ROMM_BAD_RESPONSE. SKIFF_ERR_INVALID_ARG
 * for a limit out of range.
 */
skiff_err skiff_romm_list_roms(skiff_romm_client *client, uint64_t platform_id, uint64_t offset,
                               size_t limit, skiff_romm_rom_page *out);

/* GET /api/roms/{rom_id}: the ROM with its files. A response for another ROM, or listing a file of
 * another ROM, is SKIFF_ERR_ROMM_BAD_RESPONSE. */
skiff_err skiff_romm_get_rom(skiff_romm_client *client, uint64_t rom_id, skiff_romm_rom *out);

/*
 * Writes the URL file_name of rom_id downloads from, "<base>/api/roms/<id>/content/<name>", with
 * every byte of the name other than letters, digits and "-._~" percent-encoded (a '#', '?' or '/'
 * in a file name must not end or split the path). SKIFF_ERR_INVALID_ARG for a NULL or empty
 * argument, SKIFF_ERR_BUFFER_TOO_SMALL when it does not fit (SKIFF_ROMM_CONTENT_URL_MAX always
 * does); out is empty on error.
 */
skiff_err skiff_romm_content_url(const skiff_romm_client *client, uint64_t rom_id,
                                 const char *file_name, char *out, size_t out_size);

/* ---- Parsing, exposed for the tests and the self-test ---- */

/* A /api/roms response body: SKIFF_OK, or SKIFF_ERR_ROMM_BAD_RESPONSE. */
skiff_err skiff_romm_parse_rom_page(const char *json, size_t length, skiff_romm_rom_page *out);

/* A /api/roms/{id} response body: SKIFF_OK, or SKIFF_ERR_ROMM_BAD_RESPONSE. */
skiff_err skiff_romm_parse_rom(const char *json, size_t length, skiff_romm_rom *out);

#endif
