#ifndef SKIFF_SAVES_SYNC_H
#define SKIFF_SAVES_SYNC_H

/*
 * Skiff's own sync baseline and the rule that decides each game's transfer. RomM keeps a baseline
 * per device too, but only for the save version a device last synced: once another device uploads,
 * RomM falls back to timestamps and may advise an upload over a newer save. So Skiff keeps, per
 * game, what both sides held at its last sync (app:/saves.json), and decides from hashes alone:
 * never from clocks, never from negotiate's advice.
 *
 *   L    the game's local unit hash now (skiff_saves_unit.content_hash of a collect);
 *   S    the server head of (rom_id, "autosave"): its save id and content_hash;
 *   B_l  the local unit hash at the last sync;
 *   B_s  the server save id and content_hash at the last sync.
 *
 * RomM stores save times to the second and may report either of two uploads made in the same
 * second as the head, so nothing here assumes a unique head: the server counts as unchanged when
 * its head holds B_s's content (the same hash), whichever row it is.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"
#include "skiff/saves.h"
#include "skiff/storage.h"
#include "skiff/storage_paths.h"

/* RomM's content_hash of a unit without files: the MD5 of nothing. A server unit with this hash
 * holds no save (it was deleted elsewhere). */
#define SKIFF_SAVES_EMPTY_HASH "d41d8cd98f00b204e9800998ecf8427e"

/* ---- What a game's save folders look like, without reading them ---- */

/* skiff_saves_tree_summary.file_count when the folders hold what no unit may (a folder inside a
 * save folder, a name too long, more than SKIFF_SAVES_FILES_MAX files), or a file the device holds
 * no date for (it could be rewritten at the same size unseen): such a tree always counts as
 * changed, so the caller collects and hashes it (and gets skiff_saves_collect()'s refusal). */
#define SKIFF_SAVES_TREE_UNKNOWN UINT32_MAX

/* The files of every folder in a save root starting with a game's save_target (installs included:
 * telling them apart would mean reading their PARAM.SFO), as a count and the MD5 of their
 * "<FOLDER>/<FILE>:<size>:<mtime>" lines sorted by name. Equal summaries from the same Memory Stick
 * mean no file was added, removed, resized or rewritten, so the unit need not be hashed again.
 * Dates are whole seconds (FAT keeps two), so a file rewritten at the same size within that time,
 * or copied in over USB with its old date, is not seen: a launch check may rely on the summary,
 * but a sync the player asks for should hash anyway. */
typedef struct skiff_saves_tree_summary {
    uint32_t file_count;
    char digest[SKIFF_SAVES_HASH_SIZE];
} skiff_saves_tree_summary;

/*
 * Summarises save_target's folders in the logical folder root ("saves:") from folder listings only
 * (skiff_storage_list()), reading no file. A missing root, or no folder, gives 0 files and the MD5
 * of nothing. SKIFF_ERR_INVALID_ARG for a NULL argument or an invalid save_target,
 * SKIFF_ERR_NO_MEMORY, otherwise the storage's error.
 */
skiff_err skiff_saves_tree_summarize(skiff_storage *storage, const skiff_storage_roots *roots,
                                     const char *root, const char *save_target,
                                     skiff_saves_tree_summary *out);

/* Whether two summaries differ; a summary of SKIFF_SAVES_TREE_UNKNOWN, or NULL, always does. */
int skiff_saves_tree_changed(const skiff_saves_tree_summary *then,
                             const skiff_saves_tree_summary *now);

/* ---- The baseline: app:/saves.json ---- */

#define SKIFF_SAVES_BASELINE_PATH SKIFF_STORAGE_ROOT_APP "/saves.json"
#define SKIFF_SAVES_BASELINE_VERSION 1
/* Games the baseline holds: the save folders of a well-filled Memory Stick, with room to spare. */
#define SKIFF_SAVES_BASELINE_GAMES_MAX 512
/* RomM ROMs whose save ID the baseline remembers (installed.json holds at most as many games). */
#define SKIFF_SAVES_BASELINE_ROMS_MAX 512
/* An entry is about 280 bytes of JSON at most (no name in it needs escaping), so 512 fit in 160 KB,
 * and a remembered ROM about 45, 23 KB more; a larger file is not one Skiff wrote, and is treated
 * as damaged. */
#define SKIFF_SAVES_BASELINE_BYTES_MAX ((size_t)192 * 1024)
/* Ids are JSON numbers, which cJSON prints with 15 significant digits. */
#define SKIFF_SAVES_ID_MAX 999999999999999ULL

typedef struct skiff_saves_baseline_entry {
    char save_target[SKIFF_SAVES_TARGET_SIZE];
    /* The RomM ROM the game's saves sync with. */
    uint64_t rom_id;
    /* B_s: the server save and its content_hash at the last sync. */
    uint64_t server_save_id;
    char server_hash[SKIFF_SAVES_HASH_SIZE];
    /* B_l: the local unit's content_hash at the last sync. */
    char local_hash[SKIFF_SAVES_HASH_SIZE];
    /* When the last sync happened, ms since 1970 (UTC), as the caller read it. */
    int64_t synced_ms;
    /* The local folders right after the last sync. */
    skiff_saves_tree_summary tree;
} skiff_saves_baseline_entry;

/* A ROM's save ID as RomM gave it, kept so a game Skiff installed is matched to its save folders
 * without asking RomM for its details at every launch. */
typedef struct skiff_saves_rom_target {
    uint64_t rom_id;
    /* Empty: RomM had no save ID Skiff syncs for the ROM when it was asked. */
    char save_target[SKIFF_SAVES_TARGET_SIZE];
} skiff_saves_rom_target;

typedef struct skiff_saves_baseline {
    size_t count;
    skiff_saves_baseline_entry entries[SKIFF_SAVES_BASELINE_GAMES_MAX];
    /* The ROMs remembered, oldest first. */
    size_t rom_count;
    skiff_saves_rom_target roms[SKIFF_SAVES_BASELINE_ROMS_MAX];
    /* SKIFF_OK, or the storage error the last load hit: the baselines on the Memory Stick are then
     * unknown, and skiff_saves_baseline_save() refuses with it rather than replace them. */
    skiff_err load_error;
    /* The last load found a file it could not read: the next save first moves it to
     * "<path>" SKIFF_SAVES_BASELINE_DAMAGED_SUFFIX. */
    int set_aside_damaged;
} skiff_saves_baseline;

#define SKIFF_SAVES_BASELINE_DAMAGED_SUFFIX ".damaged"

/* Allocates an empty baseline (about 80 KB). SKIFF_ERR_INVALID_ARG for NULL,
 * SKIFF_ERR_NO_MEMORY. */
skiff_err skiff_saves_baseline_create(skiff_saves_baseline **out);

/* Frees the baseline. Does nothing for NULL. */
void skiff_saves_baseline_destroy(skiff_saves_baseline *baseline);

typedef enum skiff_saves_baseline_load_result {
    SKIFF_SAVES_BASELINE_LOADED,
    /* No file yet: no game synced. */
    SKIFF_SAVES_BASELINE_NO_FILE,
    /* A file Skiff cannot read (cut, edited, too large, from a newer Skiff): the baseline starts
     * empty, so every game that differs asks the player (nothing is overwritten on doubt), the next
     * save sets the file aside, and the caller logs a warning. */
    SKIFF_SAVES_BASELINE_DAMAGED,
} skiff_saves_baseline_load_result;

/*
 * Replaces baseline's entries with the file at path (crash-safe read, skiff_storage_read_whole()).
 * A missing or damaged file gives an empty baseline and SKIFF_OK, *result (may be NULL) saying
 * which. A storage error, or SKIFF_ERR_NO_MEMORY when the memory to parse the file is not there,
 * leaves it empty, returns the error and keeps it in load_error. SKIFF_ERR_INVALID_ARG for a NULL
 * baseline, storage or path.
 */
skiff_err skiff_saves_baseline_load(skiff_saves_baseline *baseline, skiff_storage *storage,
                                    const char *path, skiff_saves_baseline_load_result *result);

/* Writes the baseline to path, replacing the file whole (skiff_storage_replace_whole()), after
 * setting a damaged one aside. The baseline's load_error when the last load failed;
 * SKIFF_ERR_INVALID_ARG for a NULL argument or a path too long; otherwise the storage's error. */
skiff_err skiff_saves_baseline_save(skiff_saves_baseline *baseline, skiff_storage *storage,
                                    const char *path);

/* save_target's entry; NULL when there is none. */
const skiff_saves_baseline_entry *skiff_saves_baseline_find(const skiff_saves_baseline *baseline,
                                                            const char *save_target);

/* Records a sync, replacing the game's entry. SKIFF_ERR_INVALID_ARG for a NULL argument, an invalid
 * save_target, an id of 0 or over SKIFF_SAVES_ID_MAX, a hash that is not 32 lowercase hex digits,
 * or a negative synced_ms; SKIFF_ERR_BUFFER_TOO_SMALL when SKIFF_SAVES_BASELINE_GAMES_MAX games
 * are already held. Save afterwards. */
skiff_err skiff_saves_baseline_record(skiff_saves_baseline *baseline,
                                      const skiff_saves_baseline_entry *entry);

/* Drops save_target's entry; 1 if there was one. Save afterwards. */
int skiff_saves_baseline_forget(skiff_saves_baseline *baseline, const char *save_target);

/* Drops every entry: the server changed, or this PSP paired again (perhaps as another RomM user).
 * Without a baseline any difference asks the player. The remembered ROMs stay (they are the
 * server's, whoever pairs). Save afterwards. Does nothing for NULL. */
void skiff_saves_baseline_forget_all(skiff_saves_baseline *baseline);

/* rom_id's remembered save ID: NULL when the ROM is not remembered, "" when RomM had none. */
const char *skiff_saves_baseline_rom_target(const skiff_saves_baseline *baseline, uint64_t rom_id);

/* Remembers rom_id's save ID (save_target, or "" for none), replacing what was remembered for it;
 * when SKIFF_SAVES_BASELINE_ROMS_MAX ROMs are held, the oldest is forgotten first. *changed (may be
 * NULL) says whether anything changed. SKIFF_ERR_INVALID_ARG for a NULL argument (but changed), an
 * id of 0 or over SKIFF_SAVES_ID_MAX, or a save_target that is neither empty nor valid. Save
 * afterwards. */
skiff_err skiff_saves_baseline_remember_rom(skiff_saves_baseline *baseline, uint64_t rom_id,
                                            const char *save_target, int *changed);

/* Forgets every remembered ROM: the server changed, so its ROM ids mean other games. Save
 * afterwards. Does nothing for NULL. */
void skiff_saves_baseline_forget_roms(skiff_saves_baseline *baseline);

/*
 * saves.json: {"version":1,"games":[{"save_target":"ULUS10064","rom_id":12,"save_id":34,
 * "server_hash":"<32 hex>","local_hash":"<32 hex>","synced_ms":1790812800123,"files":4,
 * "tree":"<32 hex>"}],"roms":[{"rom_id":12,"save_target":"ULUS10064"}]} ("files" is -1 for
 * SKIFF_SAVES_TREE_UNKNOWN, with an empty "tree"; "roms" may be absent, for none). Written
 * into out, terminated, its length in *length. SKIFF_ERR_BUFFER_TOO_SMALL, SKIFF_ERR_NO_MEMORY,
 * SKIFF_ERR_INVALID_ARG for a NULL argument.
 */
skiff_err skiff_saves_baseline_format(const skiff_saves_baseline *baseline, char *out,
                                      size_t out_size, size_t *length);

/* Reads what skiff_saves_baseline_format() writes; unknown fields are ignored. Anything else is
 * refused with SKIFF_ERR_INVALID_ARG (bad JSON, a missing or mistyped field, a value that does not
 * fit, another version, too many games, a game twice), SKIFF_ERR_NO_MEMORY when the memory to parse
 * it is not there; baseline is empty after any error. */
skiff_err skiff_saves_baseline_parse(skiff_saves_baseline *baseline, const char *text,
                                     size_t length);

/* ---- The decision rule ---- */

typedef enum skiff_saves_action {
    /* Nothing to transfer; record the baseline when reason is SKIFF_SAVES_REASON_SAME_CONTENT. */
    SKIFF_SAVES_IN_SYNC,
    SKIFF_SAVES_UPLOAD,
    /* Restore the server's unit (skiff/saves_restore.h), which backs up the local one first. */
    SKIFF_SAVES_DOWNLOAD,
    /* Both sides hold a different save and Skiff cannot prove either is already on the other: the
     * player picks. */
    SKIFF_SAVES_ASK_CONFLICT,
    /* The game's saves were deleted on this PSP since the last sync: download them again, or leave
     * them. Never uploaded as a deletion, and the server's save is never deleted. */
    SKIFF_SAVES_ASK_DELETED_LOCAL,
    /* RomM holds an empty unit (the save was deleted elsewhere) while this PSP holds one: restoring
     * it would delete the local save, so the player is always asked. */
    SKIFF_SAVES_ASK_DELETED_SERVER,
} skiff_saves_action;

/* Why: which row of the rule applied. */
typedef enum skiff_saves_reason {
    /* L == S. */
    SKIFF_SAVES_REASON_SAME_CONTENT,
    /* Neither side holds a save. */
    SKIFF_SAVES_REASON_NO_SAVES,
    /* Only this PSP holds one. */
    SKIFF_SAVES_REASON_ONLY_LOCAL,
    /* Only RomM holds one, and the baseline holds no local save: nothing local to lose. */
    SKIFF_SAVES_REASON_ONLY_SERVER,
    /* No local save while the baseline holds one (B_l is not SKIFF_SAVES_EMPTY_HASH). */
    SKIFF_SAVES_REASON_DELETED_LOCAL,
    /* RomM's head is an empty unit. */
    SKIFF_SAVES_REASON_SERVER_EMPTY,
    /* Both hold a different save and there is no baseline (first sync, Pair again, server change).
     */
    SKIFF_SAVES_REASON_NO_BASELINE,
    /* L == B_l, S != B_s. */
    SKIFF_SAVES_REASON_SERVER_CHANGED,
    /* L != B_l, S == B_s. */
    SKIFF_SAVES_REASON_LOCAL_CHANGED,
    /* L != B_l, S != B_s. */
    SKIFF_SAVES_REASON_BOTH_CHANGED,
    /* L == B_l and S == B_s, though L != S (the last sync left them different, e.g. a unit with
     * folders Skiff leaves out): neither side changed. */
    SKIFF_SAVES_REASON_UNCHANGED,
} skiff_saves_reason;

typedef struct skiff_saves_sync_input {
    /* L, or NULL when the game has no save files on this PSP (a local unit hashing to
     * SKIFF_SAVES_EMPTY_HASH counts as none). */
    const char *local_hash;
    /* S's content_hash, or NULL when RomM holds no save in the slot; and its id. */
    const char *server_hash;
    uint64_t server_save_id;
    /* The game's baseline, or NULL for none. */
    const skiff_saves_baseline_entry *baseline;
} skiff_saves_sync_input;

typedef struct skiff_saves_decision {
    skiff_saves_action action;
    skiff_saves_reason reason;
    /* The server is unchanged by content but its head is another row than B_s's (the same content
     * uploaded again, or a tie): record the new id with the next baseline. */
    int server_id_moved;
} skiff_saves_decision;

/*
 * The rule (design §3), first match:
 *
 *   L == S (both present)                         in sync, SAME_CONTENT
 *   no L, and no S or an empty S                  in sync, NO_SAVES
 *   L present, no S                               upload, ONLY_LOCAL
 *   no L, S present, B_l a save                   ask (deleted local), DELETED_LOCAL
 *   no L, S present, B_l empty, S == B_s          in sync, UNCHANGED (the player left a save
 *                                                 deleted on this PSP; see below)
 *   no L, S present, otherwise                    download, ONLY_SERVER
 *   S empty, L present                            ask (deleted server), SERVER_EMPTY ...
 *                                                 ... unless S == B_s and L != B_l: upload,
 *                                                 LOCAL_CHANGED
 *   both present, no baseline                     ask (conflict), NO_BASELINE
 *   L == B_l, S != B_s                            download, SERVER_CHANGED
 *   L != B_l, S == B_s                            upload, LOCAL_CHANGED
 *   L != B_l, S != B_s                            ask (conflict), BOTH_CHANGED
 *   L == B_l, S == B_s                            in sync, UNCHANGED
 *
 * so it never downloads over a local change, never uploads over a server change, and never
 * restores an empty unit on its own. A save deleted on this PSP that the player chose to leave is
 * recorded as a baseline with B_l the empty hash and B_s the server's: it stays left until the
 * server's save changes, which is then downloaded (nothing local to lose). SKIFF_ERR_INVALID_ARG
 * for a NULL argument or a hash that is not 32 lowercase hex digits; out is zeroed then.
 */
skiff_err skiff_saves_decide(const skiff_saves_sync_input *in, skiff_saves_decision *out);

/* Short stable names for logs ("ask-conflict", "server-changed"); "unknown" outside the enums. */
const char *skiff_saves_action_name(skiff_saves_action action);
const char *skiff_saves_reason_name(skiff_saves_reason reason);

#endif
