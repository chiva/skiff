#ifndef SKIFF_SAVES_RESTORE_H
#define SKIFF_SAVES_RESTORE_H

/*
 * Putting a unit's saves on the Memory Stick without ever losing one. FAT cannot replace a folder
 * in one step, so a restore is staged and journalled:
 *
 *   1. the unit (downloaded by the caller, or a backup) is checked whole (skiff_saves_read_unit()),
 *      the game's live folders are collected, and the Memory Stick must hold the stage, the new
 *      folders, the backup and the journal, counted in whole 32 KiB clusters, with
 *      SKIFF_STORAGE_FREE_MARGIN_BYTES to spare: nothing is written before these pass;
 *   2. the unit is extracted into SKIFF_SAVES_STAGE_FOLDER, each file synced;
 *   3. the live folders are backed up as a unit (SKIFF_SAVES_BACKUP_FOLDER), synced;
 *   4. SKIFF_SAVES_JOURNAL_PATH is written crash-safely (skiff_storage_replace_whole()), naming the
 *      game, the folders to replace (the unit's), the folders to remove (live ones the unit lacks)
 *      and every staged file with its size;
 *   5. per folder: the live files are removed and the staged ones copied in, each synced; a folder
 *      the unit lacks is removed whole, its files first and then the emptied folder, so a folder
 *      with files never reaches the device's rmdir (PPSSPP's removes one with everything in it);
 *   6. the journal is marked done (crash-safely, so the mark is on the device before anything
 *      else), the stage is removed, then the journal.
 *
 * The live saves are untouched until step 5. A cut before the journal exists (power, suspend, a
 * full stick) leaves them as they were and a stage the next restore or recovery removes; a cut
 * during step 5 leaves a journal, and skiff_saves_recover() redoes step 5 from the stage, which is
 * idempotent: the end state is the old save or the new one, never a mix of both, never neither.
 * The redo first checks that every staged file is there with its journalled size; a stage that is
 * not (deleted over USB, a damaged journal) is never applied, and the backup made in step 3 holds
 * the save as it was.
 *
 * Folders holding game data a game installed (a PARAM.SFO without SAVEDATA_PARAMS or
 * SAVEDATA_FILE_LIST, see skiff_saves_collect()) are never touched: not backed up, replaced nor
 * removed, even when a unit (one grout wrote) carries a folder of that name. Live folders are
 * matched to the unit's ignoring case, since a listing may give 8.3 names in capitals.
 *
 * Restoring an empty unit (no files, 22 bytes) removes the game's save folders after backing them
 * up: RomM holds an empty unit when the save was deleted elsewhere. The decision rule
 * (skiff/saves_sync.h) never applies one on its own; the player is always asked.
 *
 * Every file is opened per step and closed on every path, since a suspend invalidates open files
 * on the PSP; a copy that fails with SKIFF_ERR_STORAGE_IO (a handle lost to a suspend) is reopened
 * and redone once. Units and the live save root are addressed through roots: "saves:" is the
 * firmware's PSP/SAVEDATA, and tests and the self-test point it at a scratch folder.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"
#include "skiff/saves.h"
#include "skiff/storage.h"
#include "skiff/storage_paths.h"

/* Skiff's working folder for restores: the unit being restored ("<save_target>.zip", see
 * skiff_saves_work_unit_path()), the stage and the journal. */
#define SKIFF_SAVES_WORK_FOLDER SKIFF_STORAGE_ROOT_APP "/saves-work"
#define SKIFF_SAVES_STAGE_FOLDER SKIFF_SAVES_WORK_FOLDER "/stage"
#define SKIFF_SAVES_JOURNAL_PATH SKIFF_SAVES_WORK_FOLDER "/journal"
#define SKIFF_SAVES_JOURNAL_VERSION 1
/* The largest journal: SKIFF_SAVES_FILES_MAX files of the longest names, and the folders. */
#define SKIFF_SAVES_JOURNAL_BYTES_MAX ((size_t)64 * 1024)

/* Backups: "<folder>/<save_target>/<number>.zip", each a unit. Numbers count up across every game
 * (the next is one more than the highest kept), so the lowest is the oldest whatever the clock
 * says. */
#define SKIFF_SAVES_BACKUP_FOLDER SKIFF_STORAGE_ROOT_APP "/saves-backup"
#define SKIFF_SAVES_BACKUPS_PER_GAME 3
#define SKIFF_SAVES_BACKUPS_BYTES_MAX ((uint64_t)32 * 1024 * 1024)
#define SKIFF_SAVES_BACKUP_NUMBER_MAX 999999999U

/* What a restore did. */
typedef struct skiff_saves_restore_report {
    /* RomM's content_hash of the unit restored. */
    char content_hash[SKIFF_SAVES_HASH_SIZE];
    /* The backup made of the live saves first; 0 when the game had no save files. */
    uint32_t backup;
    size_t folders_replaced;
    size_t folders_removed;
    /* The unit's folders left alone because the Memory Stick holds a game's installed data under
     * that name. */
    size_t installs_kept;
    /* A restore cut short earlier, finished (or given up) before this one started:
     * skiff_saves_recover()'s result, SKIFF_OK when none was pending. */
    skiff_err recovered;
} skiff_saves_restore_report;

/* "app:/saves-work/<save_target>.zip", where the caller puts a unit downloaded for a restore.
 * SKIFF_ERR_INVALID_ARG for a NULL argument or an invalid save_target, SKIFF_ERR_BUFFER_TOO_SMALL.
 */
skiff_err skiff_saves_work_unit_path(const char *save_target, char *out, size_t out_size);

/*
 * Restores the unit at the logical path unit_path as save_target's saves in "saves:", in the steps
 * above, then prunes the backups (see skiff_saves_backups_list()). A restore cut short earlier is
 * finished first (skiff_saves_recover(), its result in report->recovered); when that fails, so does
 * this restore, with its error, before anything else. The unit file itself is left in place.
 *
 * Refused before anything is written: SKIFF_ERR_STORAGE_SAVE_DAMAGED for a unit that is not a safe
 * unit of save_target (skiff_saves_read_unit()), SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE when the live
 * folders cannot be backed up as a unit (skiff_saves_collect()), skiff_storage_check_room()'s
 * SKIFF_ERR_STORAGE_NO_SPACE or SKIFF_ERR_STORAGE_FILE_TOO_LARGE (a device that cannot tell its
 * free space is not refused). SKIFF_ERR_BUFFER_TOO_SMALL, after the stage was removed again, when
 * there are too many backups to number a new one safely (over 2048, or 1024 game folders). A
 * storage error before step 5 leaves the live saves as they were and
 * no stage or journal; one during step 5 leaves the journal, so skiff_saves_recover() (now, or at
 * the next launch) finishes the restore. report (may be NULL) is filled on SKIFF_OK.
 * SKIFF_ERR_INVALID_ARG for a NULL argument (but report) or an invalid save_target,
 * SKIFF_ERR_NO_MEMORY.
 */
skiff_err skiff_saves_restore(skiff_storage *storage, const skiff_storage_roots *roots,
                              const char *unit_path, const char *save_target,
                              skiff_saves_restore_report *report);

/* What skiff_saves_recover() found. */
typedef enum skiff_saves_recovery_outcome {
    /* No restore was pending (a leftover stage from one cut before its journal was removed). */
    SKIFF_SAVES_RECOVERY_NONE,
    /* A restore cut during step 5 was finished from its stage. */
    SKIFF_SAVES_RECOVERY_FINISHED,
    /* A journal was left whose stage is gone or incomplete, or that cannot be read: the live saves
     * were left as they are, the journal and stage removed; the backup the restore made holds the
     * save as it was. */
    SKIFF_SAVES_RECOVERY_UNFINISHED,
} skiff_saves_recovery_outcome;

typedef struct skiff_saves_recovery {
    skiff_saves_recovery_outcome outcome;
    /* The game the journal named; empty when it could not be read or none was pending. */
    char save_target[SKIFF_SAVES_TARGET_SIZE];
    /* The backup the cut restore made; 0 when it made none or the journal could not be read. */
    uint32_t backup;
} skiff_saves_recovery;

/*
 * Finishes a restore cut short, at launch before anything else touches saves; safe to call when
 * nothing is pending. Returns SKIFF_OK when nothing was pending (a stage left by a restore cut
 * before its journal is removed), SKIFF_ERR_STORAGE_SAVE_RESTORE_FINISHED (a notice, not a
 * failure) when a journal was found and step 5 redone, SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED
 * when one could not be applied (see SKIFF_SAVES_RECOVERY_UNFINISHED). A storage error during the
 * redo keeps the journal and stage for the next call; SKIFF_ERR_NO_MEMORY likewise.
 * recovery (may be NULL) says what was found. SKIFF_ERR_INVALID_ARG for a NULL storage or roots.
 */
skiff_err skiff_saves_recover(skiff_storage *storage, const skiff_storage_roots *roots,
                              skiff_saves_recovery *recovery);

/* ---- Backups ---- */

typedef struct skiff_saves_backup {
    uint32_t number;
    uint64_t size;
    /* When it was written, as the Memory Stick dates it (skiff_storage_info.mtime). */
    int64_t mtime;
} skiff_saves_backup;

typedef struct skiff_saves_backup_list {
    /* Newest (highest number) first, at most SKIFF_SAVES_BACKUPS_PER_GAME. */
    size_t count;
    skiff_saves_backup backups[SKIFF_SAVES_BACKUPS_PER_GAME];
} skiff_saves_backup_list;

/* "app:/saves-backup/<save_target>/<number>.zip". SKIFF_ERR_INVALID_ARG for a NULL argument, an
 * invalid save_target or a number of 0 or over SKIFF_SAVES_BACKUP_NUMBER_MAX,
 * SKIFF_ERR_BUFFER_TOO_SMALL. */
skiff_err skiff_saves_backup_path(const char *save_target, uint32_t number, char *out,
                                  size_t out_size);

/*
 * Lists save_target's backups, newest first. After each backup a restore makes, the game keeps its
 * newest SKIFF_SAVES_BACKUPS_PER_GAME, and the oldest of every game are dropped while all of them
 * add up to more than SKIFF_SAVES_BACKUPS_BYTES_MAX (never the one just made). A game without
 * backups lists none (SKIFF_OK). SKIFF_ERR_INVALID_ARG for a NULL argument or an invalid
 * save_target, otherwise the storage's error; out is empty on any error.
 */
skiff_err skiff_saves_backups_list(skiff_storage *storage, const skiff_storage_roots *roots,
                                   const char *save_target, skiff_saves_backup_list *out);

/* Restores save_target's backup number: skiff_saves_restore() of its unit, which backs up the live
 * saves first like any restore (and may then drop the oldest backup, this one included: its saves
 * are live by then). SKIFF_ERR_STORAGE_NOT_FOUND when there is no such backup. */
skiff_err skiff_saves_restore_backup(skiff_storage *storage, const skiff_storage_roots *roots,
                                     const char *save_target, uint32_t number,
                                     skiff_saves_restore_report *report);

#endif
