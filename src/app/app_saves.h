#ifndef SKIFF_APP_SAVES_H
#define SKIFF_APP_SAVES_H

/*
 * Save sync in the app (design: RomM 5.4 or newer, a token with the save scopes). The work runs on
 * the browsing thread as calls, one at a time like every request (app_saves_run.c):
 *
 *   - CALL_SAVES_CHECK: the games to sync and what each needs. It lists the PSP ROMs RomM holds
 *     saves for (one paged list, not the library), learns the save IDs of games Skiff installed
 *     (remembered in saves.json, so asked once), lists PSP/SAVEDATA, hashes each game's saves (or
 *     trusts the folder summary at launch), makes one negotiate and decides each game with
 *     skiff_saves_decide(), never with RomM's advice. A check of one game always hashes.
 *   - CALL_SAVES_APPLY: one game's upload (overwrite only when the player kept this PSP's), its
 *     download and restore (a backup first, the hash checked before anything is restored), leaving
 *     a deleted save deleted, or restoring a backup.
 *   - CALL_SAVES_COMPLETE: closes the check's sync session with the counts.
 *   - CALL_SAVES_BACKUPS: a game's backups.
 *
 * The screen's thread starts them when nothing else waits (a library page goes first) and applies
 * each result (app_saves.c); the call touches only app->call, the baseline (which nothing else
 * touches while it runs) and the list it fills, which the screen does not show until it is done.
 */

#include "skiff/romm_saves.h"
#include "skiff/saves.h"
#include "skiff/saves_restore.h"
#include "skiff/saves_sync.h"

/* What Skiff's uploads carry: the slot every PSP client uses, a label of its own in RomM's
 * history, and the versions RomM keeps of it. */
#define APP_SAVES_SLOT "autosave"
#define APP_SAVES_EMULATOR "psp_console"
#define APP_SAVES_KEEP_VERSIONS 10U
/* Games the Saves screen lists, and PSP ROMs with saves one check reads from RomM (pages of
 * SKIFF_ROMM_PAGE_SIZE). */
#define APP_SAVES_GAMES_MAX 128
#define APP_SAVES_SERVER_ROMS_MAX 128
/* Installed games whose save ID one check asks RomM for (a ROM's details, about 0.3 s each on a
 * PSP-1000): the rest wait for the next check, since the answers are remembered. */
#define APP_SAVES_LOOKUPS_MAX 16
/* Conflicts whose RomM side one check asks the device of. */
#define APP_SAVES_DEVICE_LOOKUPS_MAX 8
/* Folders of PSP/SAVEDATA one check reads; a Memory Stick holds far fewer. */
#define APP_SAVES_FOLDERS_MAX 512
/* A PSP game ID as save folders start with it, "ULUS10064": letters, then digits. Folders no game
 * in RomM claims are grouped by it. */
#define APP_SAVES_GAME_ID_LETTERS 4
#define APP_SAVES_GAME_ID_LENGTH 9
/* Lines under the Saves screen's list: the selected game's state, why, and when it last synced. */
#define APP_SAVES_DETAIL_LINES 3
#define APP_SAVES_ROWS SKIFF_APP_ROWS_FIT(APP_SAVES_DETAIL_LINES, 0)
#define APP_SAVES_BACKUP_ROWS SKIFF_APP_ROWS_FIT(1, 0)

/* Why a game cannot be synced. */
typedef enum app_save_problem {
    SAVE_PROBLEM_NONE,
    /* RomM holds saves for the ROM but no save ID Skiff syncs (a scan before RomM 5.4, a .pbp,
     * another layout). */
    SAVE_PROBLEM_NO_ID,
    /* Save folders on this PSP that no game Skiff knows in RomM claims. */
    SAVE_PROBLEM_UNKNOWN_GAME,
    /* Several PSP ROMs in RomM have this save ID (an ISO and a CSO of one game). */
    SAVE_PROBLEM_SEVERAL_GAMES,
    /* A step failed: the game's error says which. */
    SAVE_PROBLEM_ERROR,
} app_save_problem;

/* One game as the Saves screen shows it, and what a call needs to sync it. */
typedef struct app_save_game {
    char save_target[SKIFF_SAVES_TARGET_SIZE];
    /* RomM's title; empty without one (the save_target is shown). */
    char title[SKIFF_ROMM_NAME_MAX];
    uint64_t rom_id;
    app_save_problem problem;
    skiff_err error;
    /* L: this PSP's saves, their newest file's date (SKIFF_STORAGE_MTIME_UNKNOWN when not read)
     * and the folders' summary. */
    int has_local;
    char local_hash[SKIFF_SAVES_HASH_SIZE];
    int64_t local_mtime;
    uint64_t local_bytes;
    skiff_saves_tree_summary tree;
    /* S: RomM's head of the slot, when it holds one, and the device that uploaded it. */
    int has_server;
    uint64_t server_save_id;
    char server_hash[SKIFF_SAVES_HASH_SIZE];
    char server_updated_at[SKIFF_ROMM_TIMESTAMP_MAX];
    char server_device[SKIFF_ROMM_NAME_MAX];
    int decided;
    skiff_saves_decision decision;
    /* When the baseline last recorded a sync; 0 for never. */
    int64_t synced_ms;
    /* Check it again (alone, hashing) before anything else: a transfer found it changed. */
    int recheck;
} app_save_game;

typedef struct app_save_list {
    size_t count;
    app_save_game games[APP_SAVES_GAMES_MAX];
    /* The check's sync session, closed after the transfers it led to; 0 for none. */
    uint64_t session_id;
} app_save_list;

/* What a CALL_SAVES_CHECK or CALL_SAVES_APPLY does. */
typedef enum app_saves_op {
    SAVES_OP_CHECK,
    SAVES_OP_CHECK_ONE,
    SAVES_OP_UPLOAD,
    /* "Keep this PSP's": the only upload that replaces RomM's head. */
    SAVES_OP_UPLOAD_OVERWRITE,
    SAVES_OP_DOWNLOAD,
    SAVES_OP_LEAVE_DELETED,
    SAVES_OP_RESTORE_BACKUP,
} app_saves_op;

/* A save call's inputs and results (inside app_call). */
typedef struct app_saves_call {
    app_saves_op op;
    /* The app's baseline, which only this call touches while it runs, and a clock for its
     * records. */
    skiff_saves_baseline *baseline;
    int64_t now_utc_ms;
    char device_id[SKIFF_CONFIG_DEVICE_ID_MAX];
    skiff_log *log;
    /* SAVES_OP_CHECK: the list it fills, the PSP platform, the installed ROMs (copied under the
     * manifest lock), and whether to hash every game whatever its folders' summary says. */
    app_save_list *list;
    int has_platform;
    uint64_t platform_id;
    size_t installed_count;
    uint64_t installed[SKIFF_INSTALL_RECORDS_MAX];
    int force_hash;
    /* Every other op: the game, as it then stands. */
    app_save_game game;
    /* SAVES_OP_RESTORE_BACKUP: which; CALL_SAVES_BACKUPS: the list. */
    uint32_t backup;
    skiff_saves_backup_list backups;
    /* CALL_SAVES_COMPLETE. */
    uint64_t session_id;
    uint32_t completed;
    uint32_t failed;
    /* A transfer found the game's saves changed since its check (nothing was sent or restored):
     * check it again. */
    int stale;
    /* In: the baseline holds changes not saved yet (a ROM's save ID learnt from its details), so
     * the call saves it. Out: it was saved; else why not (the transfer itself happened). */
    int baseline_pending;
    int baseline_saved;
    skiff_err baseline_error;
    /* A restore that failed was settled at once (skiff_saves_recover()): SKIFF_OK, 308, 309, or
     * the error that left its journal for the next launch. */
    skiff_err recovery;
} app_saves_call;

/* Where a player's sync of one game came from, and returns to. */
typedef enum app_saves_origin {
    SAVES_FROM_NONE,
    SAVES_FROM_LIST,
    SAVES_FROM_DETAILS,
} app_saves_origin;

/* Why save sync cannot run, in the order the Saves screen says it. */
typedef enum app_saves_block {
    SAVES_OK,
    /* The restore journal left at launch could not be replayed (the storage error in
     * app_saves.recovered): the journal stays for the next launch. */
    SAVES_UNRECOVERED,
    /* Not connected to RomM yet (nothing is known about the server). */
    SAVES_NOT_CONNECTED,
    /* RomM before 5.4 (SKIFF_ERR_ROMM_SAVES_UNSUPPORTED). */
    SAVES_NEEDS_UPDATE,
    /* The token lacks a save scope, has no device id, or RomM refused a save call (403). */
    SAVES_NEEDS_PAIRING,
} app_saves_block;

/* The app's save sync state (app->saves, on the heap). */
typedef struct app_saves {
    skiff_saves_baseline *baseline;
    /* What the screen shows, and what a check fills (swapped when it is done). */
    app_save_list *shown;
    app_save_list *work;
    char labels[APP_SAVES_GAMES_MAX][SKIFF_APP_LABEL_MAX];
    skiff_ui_list list;
    /* The baseline in memory holds what saves.json does not yet (written by the next save call). */
    int baseline_dirty;
    /* The backup the player picked to restore. */
    uint32_t backups_pick;
    /* A check ran (the list is RomM's answer), or failed with check_error; RomM refused a save
     * call with 403. */
    int checked;
    skiff_err check_error;
    int forbidden;
    /* The launch check: not run yet this connection. */
    int launch_done;
    /* Waiting to run: a check of every game (forcing hashes: Sync all), and the batch after it
     * (every transfer that needs no choice, then the session closed). */
    int want_check;
    int want_force;
    int batch;
    size_t batch_next;
    /* The session the last check opened, closed once its transfers are done (with their counts)
     * and no choice about it is on screen. */
    uint32_t batch_completed;
    uint32_t batch_failed;
    uint64_t batch_session;
    /* A player's sync of one game: check it, then transfer it or ask. */
    int want_one;
    int one_retried;
    app_saves_origin one_origin;
    app_save_game one;
    /* Whom the check of one game or the transfer running now is for: the player (and where they
     * started it), or nobody (the batch, a game checked again). */
    app_saves_origin running_origin;
    /* A transfer the player chose (conflict screen, backups), waiting for the browsing thread. */
    int want_apply;
    app_saves_op apply_op;
    app_saves_origin apply_origin;
    app_save_game apply_game;
    /* The conflict or deletion choice on screen, and where Later returns. */
    app_save_game choice;
    app_saves_origin choice_origin;
    /* The backups of backups_game, once listed. */
    app_save_game backups_game;
    skiff_ui_list backups_list;
    skiff_saves_backup_list backups;
    int backups_known;
    skiff_err backups_error;
    /* The header's note while a check or transfer runs; empty otherwise. */
    char activity[SKIFF_TEXT_MAX];
    /* What the recovery at startup found (SKIFF_OK, 308 or 309), and where its notice goes on. */
    skiff_err recovered;
} app_saves;

#endif
