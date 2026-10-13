#ifndef SKIFF_ROMM_H
#define SKIFF_ROMM_H

/*
 * A typed client for the RomM REST API over a skiff_transport: the server's version and whether
 * Skiff supports it, the PSP platform, its ROMs (or the player's favourites, or a collection's) one
 * page at a time, the player's collections, a ROM's files and what its details screen shows about
 * the game, and the URL a file downloads from (skiff/download.h does the download). Responses are
 * JSON, parsed with cJSON one response at a time from a buffer of at most SKIFF_ROMM_BODY_MAX
 * bytes: a larger response is refused, never cut, so a PSP cannot run out of memory on a large
 * library.
 *
 * Covers are not API responses: RomM's web server serves them as files under
 * SKIFF_ROMM_COVER_PREFIX, without a token, and a ROM's details name its cover there
 * (skiff_romm_rom.cover_path, skiff_romm_cover_url()).
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
#define SKIFF_ROMM_TESTED_MINOR 4
/* The oldest RomM that syncs saves (skiff/romm_saves.h): RomM's record of what each device synced,
 * the content_hash an upload carries and the save ID its scans read from PSP discs arrived in 5.4.
 * Browsing and downloading keep SKIFF_ROMM_MIN_*. */
#define SKIFF_ROMM_SAVES_MIN_MAJOR 5
#define SKIFF_ROMM_SAVES_MIN_MINOR 4

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
/* Where RomM's web server serves the pictures it keeps (covers, screenshots), without a token. */
#define SKIFF_ROMM_COVER_PREFIX "/assets/romm/resources/"
/* A cover path as RomM gives it, e.g.
 * "/assets/romm/resources/roms/1/2/cover/small.png?ts=2026-10-09 23:02:27"; a longer one is
 * treated as no cover. */
#define SKIFF_ROMM_COVER_PATH_MAX 256
/* A cover URL: the server address and the path, any byte of which may become "%XX". */
#define SKIFF_ROMM_COVER_URL_MAX (SKIFF_CONFIG_URL_MAX + 3 * SKIFF_ROMM_COVER_PATH_MAX)
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
    /* The heartbeat's DEVICE_INSTALL.ENABLED is true: RomM takes games sent to devices
     * (skiff/romm_installs.h). 0 for anything else, a RomM 5.3 without the key included, which
     * never fails the heartbeat. */
    int installs_enabled;
} skiff_romm_server;

typedef struct skiff_romm_platform {
    uint64_t id;
    char slug[SKIFF_ROMM_SLUG_MAX];
    char name[SKIFF_ROMM_NAME_MAX];
    uint64_t rom_count;
} skiff_romm_platform;

/*
 * Whether a name from RomM can be used as it is. A name with a control character, or too long for
 * its field, does not refuse its page: the item is still listed with its name made displayable
 * (each control character, C0, DEL or C1, shown as one SKIFF_ROMM_NAME_REPLACEMENT; a long name cut
 * at a UTF-8 character boundary and ended with SKIFF_ROMM_NAME_CUT_MARKER), and is not
 * downloadable, since the shown name is not the file's real name. A name with both problems reports
 * the control character.
 */
typedef enum skiff_romm_name_status {
    SKIFF_ROMM_NAME_OK,
    SKIFF_ROMM_NAME_CONTROL_CHAR,
    SKIFF_ROMM_NAME_TOO_LONG,
} skiff_romm_name_status;

#define SKIFF_ROMM_NAME_REPLACEMENT '?'
/* ASCII, so the PSP's Latin firmware font draws it (it has no "…"). */
#define SKIFF_ROMM_NAME_CUT_MARKER "~"

typedef struct skiff_romm_file {
    char file_name[SKIFF_ROMM_FILE_NAME_MAX];
    uint64_t size;
    int has_crc32;
    uint32_t crc32;
    /* Anything but SKIFF_ROMM_NAME_OK: file_name is only for display, or the ROM's own names are
     * not usable (the file then carries the ROM's status), and skiff_romm_content_url() refuses the
     * file. */
    skiff_romm_name_status name_status;
} skiff_romm_file;

/* A save_target's buffer, terminator included: RomM's is the 9-character disc id (ULUS10064); a
 * longer one is read as none. */
#define SKIFF_ROMM_SAVE_TARGET_MAX 33

/* Where a game keeps its saves, as RomM's scan read it from the disc (save_target_layout). */
typedef enum skiff_romm_save_layout {
    /* RomM has no save ID for the game: RomM 5.3, a ROM scanned before 5.4, a .pbp, or a disc RomM
     * could not read. */
    SKIFF_ROMM_SAVE_LAYOUT_NONE,
    /* "folder-prefix": every save folder whose name starts with save_target (a PSP game's). */
    SKIFF_ROMM_SAVE_LAYOUT_FOLDER_PREFIX,
    /* Another layout RomM knows (another console's): not a PSP's save folders. */
    SKIFF_ROMM_SAVE_LAYOUT_OTHER,
} skiff_romm_save_layout;

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
    /* For name and fs_name together: anything but SKIFF_ROMM_NAME_OK means the ROM is listed but
     * cannot be downloaded. */
    skiff_romm_name_status name_status;
    /* The ROM's only file, when the answer listed the ROM's files (a ROM's details always do, a
     * list only with_files) and there is exactly one that reads as a file of this ROM: what a
     * download needs, as skiff_romm_get_rom() gives it in files[0]. has_file is 0 otherwise (no
     * files listed, several, or one that cannot be read); the ROM is listed all the same. */
    int has_file;
    skiff_romm_file file;
    /* The game's save ID and its layout (RomM 5.4 scans), e.g. "ULUS10064" and
     * SKIFF_ROMM_SAVE_LAYOUT_FOLDER_PREFIX. Read as RomM sends it, only checked to be printable
     * text that fits: the saves layer validates it (skiff_saves_target_valid()) before it names a
     * folder. Empty with SKIFF_ROMM_SAVE_LAYOUT_NONE when RomM has none, or one Skiff would not
     * keep (too long, a control character, not a string): it never refuses the ROM. */
    char save_target[SKIFF_ROMM_SAVE_TARGET_MAX];
    skiff_romm_save_layout save_layout;
} skiff_romm_rom_summary;

/* Which of a platform's ROMs a list holds. */
typedef enum skiff_romm_list_filter {
    SKIFF_ROMM_LIST_ALL,
    /* The ones in the RomM user's favourites collections (a user may have several: RomM merges
     * them). Needs no scope beyond roms.read; a user without favourites gets an empty list. */
    SKIFF_ROMM_LIST_FAVOURITES,
    /* The ones in a regular or a smart collection (collection_id, as skiff_romm_list_collections()
     * gives it). Paging needs only roms.read; another user's private collection lists nothing
     * (RomM 5.4 checks the owner). */
    SKIFF_ROMM_LIST_COLLECTION,
    SKIFF_ROMM_LIST_SMART_COLLECTION,
    /* The ones holding saves the RomM user can see (RomM 5.4's has_saves: their own, and other
     * users' public ones), whatever slot or emulator: what save sync reads instead of the whole
     * library. Needs only roms.read. */
    SKIFF_ROMM_LIST_WITH_SAVES,
} skiff_romm_list_filter;

typedef struct skiff_romm_list_query {
    uint64_t platform_id;
    skiff_romm_list_filter filter;
    /* The collection of SKIFF_ROMM_LIST_COLLECTION and SKIFF_ROMM_LIST_SMART_COLLECTION (1 or
     * more); unused otherwise. */
    uint64_t collection_id;
    /* Also list each ROM's files (about 0.5 KB more JSON per ROM), so a summary carries its only
     * file (has_file) and a download needs no request for the ROM's details. */
    int with_files;
} skiff_romm_list_query;

typedef struct skiff_romm_rom_page {
    /* ROMs the list holds, and where this page starts among them. */
    uint64_t total;
    uint64_t offset;
    size_t count;
    skiff_romm_rom_summary items[SKIFF_ROMM_PAGE_SIZE];
} skiff_romm_rom_page;

/* The scope listing collections needs (config.ini's [auth] scopes holds it when RomM granted it).
 */
#define SKIFF_ROMM_SCOPE_COLLECTIONS_READ "collections.read"

/* The collections one list keeps: RomM sends them all at once (no paging), and a PSP lists them on
 * one screen. */
#define SKIFF_ROMM_COLLECTIONS_MAX 128

/* A collection as the collections list shows it. */
typedef struct skiff_romm_collection {
    uint64_t id;
    /* A smart collection (RomM fills it from saved filters): its ids are apart from regular ones,
     * and its ROMs list with SKIFF_ROMM_LIST_SMART_COLLECTION. */
    int smart;
    /* The ROMs RomM counts in it, every platform's (what RomM's web UI shows). */
    uint64_t rom_count;
    /* In display form, as a ROM's name in a list (control characters replaced, a long name cut);
     * empty when RomM has none. Only shown, never used in a path. */
    char name[SKIFF_ROMM_NAME_MAX];
} skiff_romm_collection;

typedef struct skiff_romm_collection_list {
    size_t count;
    skiff_romm_collection items[SKIFF_ROMM_COLLECTIONS_MAX];
} skiff_romm_collection_list;

/*
 * A ROM's description and facts, as RomM's details give them (summary, metadatum, regions,
 * languages), for the details screen only: never in a path, a request or the log.
 *
 * All of it is optional, and none of it ever refuses the ROM: a field that is absent, null, of
 * another type, out of range, too long for its buffer or holding a control character is left out
 * (empty, 0), and so is a list entry of that kind, the rest of the list still read. Only the
 * description is kept in part, since it is prose shown as it is: control characters dropped, a
 * long one cut at a UTF-8 character boundary and ended with SKIFF_ROMM_DESCRIPTION_CUT_MARKER.
 *
 * Text is made drawable by the PSP's Latin firmware font: typographic quotes, dashes, the ellipsis
 * and similar punctuation become their ASCII forms, a no-break space a space, a soft hyphen
 * nothing.
 */

/* The description, terminator included: about 45 lines beside the cover, four screens. The
 * structure is held twice (the details shown and the request's result), so the cap stays small on
 * a 32 MB PSP-1000. */
#define SKIFF_ROMM_DESCRIPTION_MAX 2048
#define SKIFF_ROMM_DESCRIPTION_CUT_MARKER "..."
/* A genre, a company, a region or a language, terminator included. */
#define SKIFF_ROMM_FACT_MAX 64
/* RomM's player count ("1-4"), terminator included. */
#define SKIFF_ROMM_PLAYERS_MAX 16
/* The entries kept of each list: what one line beside the cover holds. */
#define SKIFF_ROMM_GENRES_MAX 2
#define SKIFF_ROMM_REGIONS_MAX 4
#define SKIFF_ROMM_LANGUAGES_MAX 6
/* RomM's average_rating is the mean of its providers' scores, each scaled to 0-100
 * (backend/utils/roms_columns.py, 5.4.0). */
#define SKIFF_ROMM_RATING_SCALE 100

typedef struct skiff_romm_rom_metadata {
    /* RomM's summary, with its line breaks kept ('\n', at most one empty line between
     * paragraphs, none around it), tabs and carriage returns as spaces, other control characters
     * dropped; empty without one. */
    char description[SKIFF_ROMM_DESCRIPTION_MAX];
    /* metadatum.first_release_date's year (RomM sends Unix milliseconds), in UTC; 0 when unknown.
     */
    int release_year;
    /* metadatum.average_rating rounded, 1 to SKIFF_ROMM_RATING_SCALE; 0 when unknown. */
    unsigned rating;
    /* metadatum.player_count ("1-2", "4"); empty when unknown and for "1", which RomM also gives
     * every game it knows nothing about. */
    char players[SKIFF_ROMM_PLAYERS_MAX];
    /* The first genres in RomM's order (alphabetical). */
    size_t genre_count;
    char genres[SKIFF_ROMM_GENRES_MAX][SKIFF_ROMM_FACT_MAX];
    /* The first developer and publisher; without either, the first company as the developer. */
    char developer[SKIFF_ROMM_FACT_MAX];
    char publisher[SKIFF_ROMM_FACT_MAX];
    /* RomM's names, from the file name's tags or a provider, e.g. "USA" and "English". */
    size_t region_count;
    char regions[SKIFF_ROMM_REGIONS_MAX][SKIFF_ROMM_FACT_MAX];
    size_t language_count;
    char languages[SKIFF_ROMM_LANGUAGES_MAX][SKIFF_ROMM_FACT_MAX];
} skiff_romm_rom_metadata;

typedef struct skiff_romm_rom {
    skiff_romm_rom_summary summary;
    /* Files RomM lists for the ROM, all of them checked; the first stored_count of them (at most
     * SKIFF_ROMM_FILES_MAX) are in files. */
    size_t file_count;
    size_t stored_count;
    skiff_romm_file files[SKIFF_ROMM_FILES_MAX];
    /* RomM's small cover (path_cover_small), a path on the server under SKIFF_ROMM_COVER_PREFIX
     * with its "?ts=" query; empty when the ROM has none. A cover is decoration, so a path Skiff
     * would not request (another prefix, a "." or ".." segment, a backslash, a control character,
     * too long, not a string) leaves it empty rather than refusing the ROM. */
    char cover_path[SKIFF_ROMM_COVER_PATH_MAX];
    skiff_romm_rom_metadata metadata;
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
 * filled in every case; SKIFF_ERR_ROMM_BAD_RESPONSE for an empty or over-long version, or one
 * with a control character (it is shown and logged).
 */
skiff_err skiff_romm_check_version(const char *version, skiff_romm_server *out);

/* Whether the server syncs saves: a release of SKIFF_ROMM_SAVES_MIN_* or newer, or a version that
 * does not start with major.minor (a development build, allowed like any version Skiff cannot
 * read). 0 for NULL. */
int skiff_romm_server_syncs_saves(const skiff_romm_server *server);

/* GET /api/heartbeat (no token sent): the server's version, through skiff_romm_check_version(),
 * and whether it takes games sent to devices (installs_enabled, read only for a version Skiff
 * supports). */
skiff_err skiff_romm_heartbeat(skiff_romm_client *client, skiff_romm_server *out);

/* GET /api/platforms: the platform whose slug is slug (e.g. "psp"). SKIFF_ERR_ROMM_NOT_FOUND (202)
 * when RomM has none, as when its library has no folder for it. */
skiff_err skiff_romm_find_platform(skiff_romm_client *client, const char *slug,
                                   skiff_romm_platform *out);

/*
 * GET /api/roms: up to limit (1 to SKIFF_ROMM_PAGE_SIZE) ROMs of query's platform and filter from
 * offset, ordered by name, without the per-library extras RomM adds by default (and with each ROM's
 * files if query asks). A page past the end is empty with the total. A page that is not the one
 * asked for (another offset, more ROMs than limit or than the total leaves, a ROM of another
 * platform) is SKIFF_ERR_ROMM_BAD_RESPONSE. SKIFF_ERR_INVALID_ARG for a NULL query, a limit out
 * of range, a filter outside skiff_romm_list_filter or a collection filter without a collection.
 */
skiff_err skiff_romm_list_roms(skiff_romm_client *client, const skiff_romm_list_query *query,
                               uint64_t offset, size_t limit, skiff_romm_rom_page *out);

/*
 * GET /api/collections, then GET /api/collections/smart (both need collections.read): the
 * collections the RomM user owns and every other user's public ones, the regular ones first, then
 * the smart ones, each in RomM's order (by name). Favourites collections are left out (the
 * favourites list holds their games), and virtual ones (generated per genre, company and so on)
 * are not asked for.
 *
 * RomM pages neither list and lists every ROM id of every collection, with two cover paths per ROM
 * of a regular one (about 150 bytes a ROM). An answer over SKIFF_ROMM_BODY_MAX or
 * SKIFF_ROMM_JSON_NODES_MAX, or more than SKIFF_ROMM_COLLECTIONS_MAX collections in all, is
 * SKIFF_ERR_ROMM_COLLECTIONS_TOO_LARGE (214): the list is refused whole, never shown in part. A
 * token without collections.read is SKIFF_ERR_ROMM_FORBIDDEN (201). out is empty on error.
 */
skiff_err skiff_romm_list_collections(skiff_romm_client *client, skiff_romm_collection_list *out);

/* GET /api/roms/{rom_id}: the ROM with its files, cover and metadata. A response for another ROM,
 * or listing a file of another ROM, is SKIFF_ERR_ROMM_BAD_RESPONSE. */
skiff_err skiff_romm_get_rom(skiff_romm_client *client, uint64_t rom_id, skiff_romm_rom *out);

/*
 * Writes the URL file (of the ROM rom_id) downloads from, "<base>/api/roms/<id>/content/<name>",
 * with every byte of the name other than letters, digits and "-._~" percent-encoded (a '#', '?' or
 * '/' in a file name must not end or split the path). SKIFF_ERR_INVALID_ARG for a NULL argument, an
 * empty name, or a file whose name_status is not SKIFF_ROMM_NAME_OK (its file_name is a display
 * form, not the name RomM serves it under); SKIFF_ERR_BUFFER_TOO_SMALL when it does not fit
 * (SKIFF_ROMM_CONTENT_URL_MAX always does). out is empty on error.
 */
skiff_err skiff_romm_content_url(const skiff_romm_client *client, uint64_t rom_id,
                                 const skiff_romm_file *file, char *out, size_t out_size);

/*
 * Writes the URL of a ROM's cover, the server address followed by cover_path (as
 * skiff_romm_rom.cover_path holds it) with every byte other than letters, digits and "-._~/?=&:"
 * percent-encoded: the "?ts=" RomM appends holds a raw space, which no request line may carry. The
 * request needs no token. SKIFF_ERR_INVALID_ARG for a NULL argument or a path
 * skiff_romm_rom.cover_path would not hold (an empty one included); SKIFF_ERR_BUFFER_TOO_SMALL when
 * it does not fit (SKIFF_ROMM_COVER_URL_MAX always does). out is empty on error.
 */
skiff_err skiff_romm_cover_url(const skiff_romm_client *client, const char *cover_path, char *out,
                               size_t out_size);

/*
 * GET the cover at cover_path (skiff_romm_rom.cover_path) without the token: its bytes in a new
 * buffer *out of *size bytes (at most SKIFF_ROMM_BODY_MAX), which the caller frees. Returns the
 * transport's error, the RomM error for an HTTP status (a cover RomM no longer has is
 * SKIFF_ERR_ROMM_NOT_FOUND), SKIFF_ERR_ROMM_BAD_RESPONSE for a body over the cap,
 * SKIFF_ERR_ROMM_COVER_DAMAGED for an empty one, skiff_romm_cover_url()'s refusals, and
 * SKIFF_ERR_NO_MEMORY. *out is NULL and *size 0 on error.
 */
skiff_err skiff_romm_get_cover(skiff_romm_client *client, const char *cover_path,
                               unsigned char **out, size_t *size);

/* ---- Parsing, exposed for the tests and the self-test ---- */

/* A /api/roms response body: SKIFF_OK, or SKIFF_ERR_ROMM_BAD_RESPONSE. */
skiff_err skiff_romm_parse_rom_page(const char *json, size_t length, skiff_romm_rom_page *out);

/* A /api/roms/{id} response body: SKIFF_OK, or SKIFF_ERR_ROMM_BAD_RESPONSE. */
skiff_err skiff_romm_parse_rom(const char *json, size_t length, skiff_romm_rom *out);

/*
 * A /api/collections body (smart 0) or a /api/collections/smart one (smart 1), appended to the
 * collections already in out: SKIFF_OK; SKIFF_ERR_ROMM_COLLECTIONS_TOO_LARGE for a body over
 * SKIFF_ROMM_BODY_MAX or SKIFF_ROMM_JSON_NODES_MAX, or more collections than out has room for; or
 * SKIFF_ERR_ROMM_BAD_RESPONSE; SKIFF_ERR_INVALID_ARG for a NULL out or one fuller than its room.
 * out->count is left as it was on error.
 */
skiff_err skiff_romm_parse_collections(const char *json, size_t length, int smart,
                                       skiff_romm_collection_list *out);

#endif
