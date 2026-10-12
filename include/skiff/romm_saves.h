#ifndef SKIFF_ROMM_SAVES_H
#define SKIFF_ROMM_SAVES_H

/*
 * RomM's save sync calls (RomM 5.4 or newer), over the client from skiff/romm.h, for the saves
 * layer:
 *
 *   1. skiff_romm_saves_negotiate() (POST /api/sync/negotiate): this PSP's device id, the ROMs it
 *      knows and the saves it holds; RomM opens a sync session and answers with an operation for
 *      each (rom_id, slot) it pairs. Skiff decides from its own baseline and the hashes: the action
 *      is RomM's advice. Skiff never sends negotiate's "emulators" filter, so a slot's saves meet
 *      whichever emulator wrote them (a PPSSPP save in "autosave" pairs with this PSP's), and the
 *      operations cover every slot of the ROMs listed: the caller keeps those of its own slot.
 *   2. skiff_romm_saves_upload() (POST /api/saves): a save file streamed from the storage seam as
 *      multipart/form-data with its exact length, under RomM's guard: without overwrite, RomM
 *      refuses it (SKIFF_ERR_ROMM_SAVE_CONFLICT) when the slot holds a version this PSP has not
 *      synced.
 *   3. skiff_romm_saves_download() (GET /api/saves/{id}/content): a save into a file, whole or not
 *      at all.
 *   4. skiff_romm_saves_downloaded() (POST /api/saves/{id}/downloaded): tells RomM this PSP holds
 *      a version, so its record of what this PSP synced matches.
 *   5. skiff_romm_saves_complete() (POST /api/sync/sessions/{id}/complete): closes the session
 *      with the counts.
 *
 * skiff_romm_saves_get() (GET /api/saves/{id}) reads one save, for the device that uploaded it.
 * An upload and a download are long transfers (skiff_http_request.long_transfer): a save can be a
 * few MB, which a PSP's Wi-Fi may take longer to move than the limit on the screen's requests.
 *
 * The token needs assets.read, assets.write, devices.read and devices.write (RomM's default user
 * group grants them; pairing asks for them as optional scopes): RomM refuses a token without them
 * with 403, SKIFF_ERR_ROMM_FORBIDDEN. Every call returns what skiff/romm.h's calls return (the
 * transport's error, the RomM error for an HTTP status, SKIFF_ERR_ROMM_BAD_RESPONSE for a body that
 * is not RomM's JSON or does not fit its fields, SKIFF_ERR_NO_MEMORY), SKIFF_ERR_INVALID_ARG for a
 * NULL argument or a value RomM would refuse, plus the errors each lists. Nothing is logged: the
 * token travels only in the Authorization header.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/config.h"
#include "skiff/error.h"
#include "skiff/romm.h"
#include "skiff/storage.h"

/* The scopes save sync needs (config.ini's [auth] scopes holds them when RomM granted them): the
 * player's own saves, and RomM's record of what this PSP synced. */
#define SKIFF_ROMM_SCOPE_ASSETS_READ "assets.read"
#define SKIFF_ROMM_SCOPE_ASSETS_WRITE "assets.write"
#define SKIFF_ROMM_SCOPE_DEVICES_READ "devices.read"
#define SKIFF_ROMM_SCOPE_DEVICES_WRITE "devices.write"

/* RomM's limits: a slot of up to 255 bytes, an emulator name of up to 50, 500 ROMs a negotiate.
 * Buffer sizes include the terminator. */
#define SKIFF_ROMM_SLOT_MAX 256
#define SKIFF_ROMM_EMULATOR_MAX 51
#define SKIFF_ROMM_SYNC_ROMS_MAX 500
/* Saves one negotiate sends, and operations it reads (every slot of every ROM listed): a caller
 * with more negotiates its ROMs in turns. A plan of SKIFF_ROMM_SYNC_OPERATIONS_MAX operations is
 * about 170 KB, so it belongs on the heap. */
#define SKIFF_ROMM_SYNC_SAVES_MAX 256
#define SKIFF_ROMM_SYNC_OPERATIONS_MAX 256
/* RomM's content_hash: an MD5 in lowercase hexadecimal. */
#define SKIFF_ROMM_CONTENT_HASH_LENGTH 32
#define SKIFF_ROMM_CONTENT_HASH_MAX (SKIFF_ROMM_CONTENT_HASH_LENGTH + 1)
/* A timestamp as RomM writes it, e.g. "2026-10-11T18:15:01.794675+00:00". */
#define SKIFF_ROMM_TIMESTAMP_MAX 40
/* The versions of an upload's lineage (its emulator and file extension) RomM keeps in the slot,
 * older ones pruned: RomM clamps autocleanup_limit to 1 and its MAX_AUTOCLEANUP_LIMIT (100 by
 * default). */
#define SKIFF_ROMM_SAVE_VERSIONS_MAX 100

/* What RomM advises for one (rom_id, slot). */
typedef enum skiff_romm_sync_action {
    /* This PSP has a save RomM lacks, or a newer one. */
    SKIFF_ROMM_SYNC_UPLOAD,
    /* RomM has a save this PSP lacks, or a newer one. */
    SKIFF_ROMM_SYNC_DOWNLOAD,
    /* Both changed since this PSP last synced. */
    SKIFF_ROMM_SYNC_CONFLICT,
    SKIFF_ROMM_SYNC_NO_OP,
    /* The slot this PSP still holds was emptied in RomM. */
    SKIFF_ROMM_SYNC_DELETE,
} skiff_romm_sync_action;

/* One save this PSP holds, as negotiate reports it. */
typedef struct skiff_romm_save_state {
    uint64_t rom_id;
    /* The name it would upload under, e.g. "SKIF00001.zip". */
    const char *file_name;
    const char *slot;
    const char *emulator;
    /* RomM's content_hash of the file: SKIFF_ROMM_CONTENT_HASH_LENGTH lowercase hex digits. */
    const char *content_hash;
    /* When it last changed, in milliseconds since 1970 (UTC); sent as ISO 8601 UTC. */
    int64_t updated_at_ms;
    uint64_t file_size_bytes;
} skiff_romm_save_state;

typedef struct skiff_romm_sync_query {
    /* RomM's id for this PSP (config.ini's [auth] device_id). */
    const char *device_id;
    /* The ROMs this PSP knows: RomM offers downloads only for these and the ROMs of saves. None
     * (rom_count 0) offers the player's whole save library. */
    const uint64_t *rom_ids;
    size_t rom_count;
    const skiff_romm_save_state *saves;
    size_t save_count;
    /* RomM's restore_unlisted: also report the slots this PSP synced before and no longer lists
     * (its save was deleted here), which RomM otherwise leaves out as deliberately deleted. Skiff
     * decides those itself (it asks), so it sets this. */
    int restore_unlisted;
} skiff_romm_sync_query;

typedef struct skiff_romm_sync_operation {
    skiff_romm_sync_action action;
    uint64_t rom_id;
    /* The server save it concerns; always set for a download or a conflict. */
    int has_save_id;
    uint64_t save_id;
    /* The client's name for an upload or a delete, the server's (with RomM's date tag) for the
     * rest. */
    char file_name[SKIFF_ROMM_FILE_NAME_MAX];
    /* has_slot 0: an archival save, without a slot. */
    int has_slot;
    char slot[SKIFF_ROMM_SLOT_MAX];
    /* Empty when RomM has none. */
    char emulator[SKIFF_ROMM_EMULATOR_MAX];
    /* The server save's hash and when it last changed, as RomM wrote it; empty when there is none.
     */
    char server_content_hash[SKIFF_ROMM_CONTENT_HASH_MAX];
    char server_updated_at[SKIFF_ROMM_TIMESTAMP_MAX];
} skiff_romm_sync_operation;

typedef struct skiff_romm_sync_plan {
    /* The session to close with skiff_romm_saves_complete(). */
    uint64_t session_id;
    size_t count;
    skiff_romm_sync_operation operations[SKIFF_ROMM_SYNC_OPERATIONS_MAX];
} skiff_romm_sync_plan;

/* A save as RomM keeps it (SaveSchema), as far as sync needs it. */
typedef struct skiff_romm_save {
    uint64_t id;
    uint64_t rom_id;
    /* RomM's name for it: a slotted upload gets a date tag, "SKIF00001 [2026-10-11_18-15-01].zip".
     */
    char file_name[SKIFF_ROMM_FILE_NAME_MAX];
    int has_slot;
    char slot[SKIFF_ROMM_SLOT_MAX];
    /* Empty when RomM has none. */
    char emulator[SKIFF_ROMM_EMULATOR_MAX];
    /* Empty when RomM could not hash the file. */
    char content_hash[SKIFF_ROMM_CONTENT_HASH_MAX];
    char updated_at[SKIFF_ROMM_TIMESTAMP_MAX];
    uint64_t file_size_bytes;
    /* The device that uploaded it, and that device's name where RomM lists it among the devices
     * that synced this save, for "from <device>" (shown, never trusted: control characters become
     * SKIFF_ROMM_NAME_REPLACEMENT and a long name is cut, as for ROM names). Empty when RomM does
     * not say. */
    char origin_device_id[SKIFF_CONFIG_DEVICE_ID_MAX];
    char origin_device_name[SKIFF_ROMM_NAME_MAX];
} skiff_romm_save;

typedef struct skiff_romm_upload {
    uint64_t rom_id;
    const char *device_id;
    /* At most SKIFF_ROMM_EMULATOR_MAX - 1 bytes of printable ASCII that RomM keeps as one folder
     * name: no / \ : | * ? " < >, no blank at either end, not "." or "..". */
    const char *emulator;
    /* At most SKIFF_ROMM_SLOT_MAX - 1 bytes, without control characters. */
    const char *slot;
    /* The file's content_hash, recorded by RomM as what this PSP holds. */
    const char *content_hash;
    /* 0 for RomM's guard (SKIFF_ERR_ROMM_SAVE_CONFLICT when the slot holds a version this PSP has
     * not synced), 1 to replace it anyway: the older version stays in RomM's history. */
    int overwrite;
    /* Versions of this lineage RomM keeps in the slot, 1 to SKIFF_ROMM_SAVE_VERSIONS_MAX. */
    uint32_t keep_versions;
    /* The file to send, opened for this call only, and the name RomM is told: printable ASCII,
     * at most SKIFF_ROMM_FILE_NAME_MAX - 1 bytes, no / \ or ", not "." or "..". */
    skiff_storage *storage;
    const char *path;
    const char *file_name;
} skiff_romm_upload;

typedef struct skiff_romm_download {
    uint64_t save_id;
    const char *device_id;
    /* Written as "<path>.part", renamed to path once whole (a file already at path is replaced);
     * the .part is removed on any failure, except when the old file was already removed and the
     * rename failed: the complete .part is then the one copy left, and stays for the caller. */
    skiff_storage *storage;
    const char *path;
    /* The most bytes accepted: a larger save is refused before anything is written when RomM
     * announces its size, otherwise as soon as it passes the cap. */
    uint64_t max_bytes;
} skiff_romm_download;

/*
 * POST /api/sync/negotiate: the operations RomM plans for query, of every slot. Refused as
 * SKIFF_ERR_ROMM_BAD_RESPONSE besides what skiff_romm_parse_sync_plan() refuses: an operation for
 * a ROM the query did not name (when it names ROMs). SKIFF_ERR_INVALID_ARG for more than
 * SKIFF_ROMM_SYNC_ROMS_MAX ROMs or SKIFF_ROMM_SYNC_SAVES_MAX saves, a ROM id of 0, or a save whose
 * fields skiff_romm_upload would refuse (the same rules), with a negative time or a size past
 * 2^53. out is empty on error.
 */
skiff_err skiff_romm_saves_negotiate(skiff_romm_client *client, const skiff_romm_sync_query *query,
                                     skiff_romm_sync_plan *out);

/*
 * POST /api/saves?rom_id=&emulator=&slot=&device_id=&content_hash=&overwrite=&autocleanup=true
 * &autocleanup_limit=: the file at upload->path, as the "saveFile" part of a multipart body
 * (Content-Type application/zip) streamed from the storage seam in SKIFF_ROMM_UPLOAD_BUFFER_BYTES
 * reads, and RomM's record of the save it now holds (RomM keeps an identical file once, so that
 * may be an older save of the slot). Returns SKIFF_ERR_ROMM_SAVE_CONFLICT for RomM's 409,
 * SKIFF_ERR_ROMM_SAVE_TOO_LARGE for its 413 (RomM's MAX_ASSET_UPLOAD_SIZE_BYTES, or a proxy's
 * limit), or for a file too large to send at all, the storage's error (unchanged, also mid-upload:
 * the transfer stops and RomM keeps nothing), SKIFF_ERR_STORAGE_IO for a file whose size changed
 * while it was sent, SKIFF_ERR_INVALID_ARG (also mid-upload) for a file that holds the multipart
 * delimiter, "--skiff-<content_hash>" (only a zip comment could; Skiff's units have none), and
 * SKIFF_ERR_ROMM_BAD_RESPONSE for a save of another ROM or slot, or what skiff_romm_parse_save()
 * refuses. The file is closed on every path; out is empty on error.
 */
skiff_err skiff_romm_saves_upload(skiff_romm_client *client, const skiff_romm_upload *upload,
                                  skiff_romm_save *out);

/* What an upload reads from the Memory Stick at a time, on the heap. While the Memory Stick works
 * the Wi-Fi transfer stops and takes about 0.1 s to pick up again (hardware-findings.md, Download
 * speed), so reads are few and large, as a download's writes are
 * (SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES): 16 KB reads, 512 pauses per 8 MB, held S1.6's upload to 123
 * KB/s. */
#define SKIFF_ROMM_UPLOAD_BUFFER_BYTES ((size_t)1024 * 1024)

/*
 * GET /api/saves/{save_id}/content?device_id=&optimistic=false into download->path, and its size
 * in *size. optimistic=false: RomM records what this PSP holds only through
 * skiff_romm_saves_downloaded(), once the save is really here. Returns SKIFF_ERR_ROMM_BAD_RESPONSE
 * for a save over max_bytes, a status other than 200 that is not an error, or a body shorter than
 * announced, the storage's error, and SKIFF_ERR_INVALID_ARG for a save id of 0 or a path too long
 * for its ".part". *size is 0 on error. No hash is checked here.
 */
skiff_err skiff_romm_saves_download(skiff_romm_client *client, const skiff_romm_download *download,
                                    uint64_t *size);

/* GET /api/saves/{save_id}?device_id=: the save as RomM keeps it, with the name of the device that
 * uploaded it (for "from <device>"). An answer that is not that save (skiff_romm_parse_save()'s
 * rules, and its id) is SKIFF_ERR_ROMM_BAD_RESPONSE; SKIFF_ERR_INVALID_ARG for a save id of 0 or a
 * device id RomM would refuse. out is empty on error. */
skiff_err skiff_romm_saves_get(skiff_romm_client *client, uint64_t save_id, const char *device_id,
                               skiff_romm_save *out);

/* The suffix of a download that is not whole yet. */
#define SKIFF_ROMM_DOWNLOAD_PART_SUFFIX ".part"

/* POST /api/saves/{save_id}/downloaded with {device_id, content_hash}: this PSP now holds the
 * version whose content_hash is given. An answer that is not that save (skiff_romm_parse_save()'s
 * rules, and its id) is SKIFF_ERR_ROMM_BAD_RESPONSE: it confirms nothing. */
skiff_err skiff_romm_saves_downloaded(skiff_romm_client *client, uint64_t save_id,
                                      const char *device_id, const char *content_hash);

/* POST /api/sync/sessions/{session_id}/complete with the operations done and failed. A session
 * already closed is RomM's 400, and an answer that does not name the session closed, both
 * SKIFF_ERR_ROMM_BAD_RESPONSE. */
skiff_err skiff_romm_saves_complete(skiff_romm_client *client, uint64_t session_id,
                                    uint32_t operations_completed, uint32_t operations_failed);

/* ---- Parsing, exposed for the tests and the self-test ---- */

/*
 * A negotiate response body: SKIFF_OK, or SKIFF_ERR_ROMM_BAD_RESPONSE for anything RomM would not
 * send: no session id, more than SKIFF_ROMM_SYNC_OPERATIONS_MAX operations, an unknown action, a
 * download or conflict without a save id, a hash that is not SKIFF_ROMM_CONTENT_HASH_LENGTH
 * lowercase hex digits, a string too long for its field or with a control character.
 */
skiff_err skiff_romm_parse_sync_plan(const char *json, size_t length, skiff_romm_sync_plan *out);

/* A save (SaveSchema) body: SKIFF_OK, or SKIFF_ERR_ROMM_BAD_RESPONSE under the same rules. */
skiff_err skiff_romm_parse_save(const char *json, size_t length, skiff_romm_save *out);

#endif
