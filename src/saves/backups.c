/*
 * Backups of a game's live saves, made before every restore: each a unit at
 * "app:/saves-backup/<save_target>/<number>.zip". Numbers count up across every game, so the
 * lowest is the oldest without asking the clock; a backup is written as "<number>.zip.tmp" and
 * renamed once whole, so a cut never leaves a damaged backup under a backup's name.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/saves_restore.h"

#include "saves_internal.h"

/* Backups one scan keeps track of: SKIFF_SAVES_BACKUPS_PER_GAME for 512 games, and leftovers; and
 * game folders. */
#define SCAN_MAX 2048
#define SCAN_GAMES_MAX 1024
/* "<number>.zip" and "<number>.zip.tmp": up to 9 digits. */
#define BACKUP_DIGITS_MAX 9
#define BACKUP_NAME_SIZE 24
#define DECIMAL_BASE 10U

typedef struct backup_entry {
    /* The game's folder and the file's name as the listing gave them. */
    char folder[SKIFF_SAVES_TARGET_SIZE];
    char name[BACKUP_NAME_SIZE];
    uint32_t number;
    uint64_t size;
    int64_t mtime;
    /* A "<number>.zip.tmp" a cut backup left. */
    int draft;
    int dropped;
} backup_entry;

typedef struct backup_scan {
    skiff_storage *storage;
    const skiff_storage_roots *roots;
    /* Only this game's folder, or every game's (NULL). */
    const char *only;
    /* The game folder being listed. */
    const char *folder;
    size_t count;
    uint32_t highest;
    /* Game folders or backups past what the scan keeps: the highest number may be unknown, so no
     * backup is numbered and nothing is pruned. */
    int overflow;
    backup_entry entries[SCAN_MAX];
    size_t folder_count;
    char folders[SCAN_GAMES_MAX][SKIFF_SAVES_TARGET_SIZE];
} backup_scan;

static int ends_with_ignoring_case(const char *name, size_t length, const char *suffix) {
    const size_t suffix_length = strlen(suffix);
    return length >= suffix_length &&
           saves_same_name(name + length - suffix_length, suffix_length, suffix, suffix_length);
}

/* "<number>.zip" (a backup) or "<number>.zip.tmp" (a draft), the number without leading zeros, 1
 * to SKIFF_SAVES_BACKUP_NUMBER_MAX; anything else is not Skiff's and is left alone. The listing
 * may give the name in capitals ("4.ZIP"). */
static int parse_backup_name(const char *name, uint32_t *number, int *draft) {
    size_t length = strlen(name);
    *draft = ends_with_ignoring_case(name, length,
                                     SKIFF_SAVES_UNIT_EXTENSION SKIFF_STORAGE_DRAFT_SUFFIX);
    if (*draft) {
        length -= strlen(SKIFF_STORAGE_DRAFT_SUFFIX);
    }
    if (!ends_with_ignoring_case(name, length, SKIFF_SAVES_UNIT_EXTENSION)) {
        return 0;
    }
    const size_t digits = length - strlen(SKIFF_SAVES_UNIT_EXTENSION);
    if (digits == 0 || digits > BACKUP_DIGITS_MAX || name[0] == '0') {
        return 0;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < digits; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return 0;
        }
        value = value * DECIMAL_BASE + (uint32_t)(name[i] - '0');
    }
    *number = value;
    return 1;
}

static skiff_storage_list_step note_backup(void *ctx, const skiff_storage_entry *entry) {
    backup_scan *scan = ctx;
    uint32_t number = 0;
    int draft = 0;
    if (entry->info.is_dir || strlen(entry->name) >= BACKUP_NAME_SIZE ||
        !parse_backup_name(entry->name, &number, &draft)) {
        return SKIFF_STORAGE_LIST_CONTINUE;
    }
    /* A draft's number may be taken again: the backup written then replaces it. */
    scan->highest = !draft && number > scan->highest ? number : scan->highest;
    if (scan->count == SCAN_MAX) {
        scan->overflow = 1;
        return SKIFF_STORAGE_LIST_CONTINUE;
    }
    backup_entry *backup = &scan->entries[scan->count++];
    memset(backup, 0, sizeof *backup);
    memcpy(backup->folder, scan->folder, strlen(scan->folder) + 1);
    memcpy(backup->name, entry->name, strlen(entry->name) + 1);
    backup->number = number;
    backup->size = entry->info.size;
    backup->mtime = entry->info.mtime;
    backup->draft = draft;
    return SKIFF_STORAGE_LIST_CONTINUE;
}

static skiff_storage_list_step note_game(void *ctx, const skiff_storage_entry *entry) {
    backup_scan *scan = ctx;
    const size_t length = strlen(entry->name);
    if (!entry->info.is_dir || !skiff_saves_target_valid(entry->name) ||
        (scan->only != NULL && strcmp(entry->name, scan->only) != 0)) {
        return SKIFF_STORAGE_LIST_CONTINUE;
    }
    if (scan->folder_count == SCAN_GAMES_MAX) {
        scan->overflow = 1;
        return SKIFF_STORAGE_LIST_STOP;
    }
    memcpy(scan->folders[scan->folder_count++], entry->name, length + 1);
    return SKIFF_STORAGE_LIST_CONTINUE;
}

/* Lists the backups: every game's, or only's. A missing backup folder holds none. */
static skiff_err scan_backups(backup_scan *scan) {
    char root[SKIFF_STORAGE_PATH_MAX];
    char folder[SKIFF_STORAGE_PATH_MAX];
    size_t skipped = 0;
    scan->count = 0;
    scan->folder_count = 0;
    scan->highest = 0;
    scan->overflow = 0;
    skiff_err err =
        skiff_storage_resolve(scan->roots, SKIFF_SAVES_BACKUP_FOLDER, root, sizeof root);
    if (err == SKIFF_OK) {
        err = skiff_storage_list(scan->storage, root, note_game, scan, &skipped);
    }
    for (size_t i = 0; err == SKIFF_OK && i < scan->folder_count; i++) {
        scan->folder = scan->folders[i];
        err = saves_resolve(scan->roots, SKIFF_SAVES_BACKUP_FOLDER, scan->folder, NULL, folder,
                            sizeof folder);
        if (err == SKIFF_OK) {
            err = skiff_storage_list(scan->storage, folder, note_backup, scan, &skipped);
        }
        err = err == SKIFF_ERR_STORAGE_NOT_FOUND ? SKIFF_OK : err;
    }
    return err == SKIFF_ERR_STORAGE_NOT_FOUND ? SKIFF_OK : err;
}

static skiff_err scan_create(skiff_storage *storage, const skiff_storage_roots *roots,
                             const char *only, backup_scan **out) {
    backup_scan *scan = calloc(1, sizeof *scan);
    if (scan == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    scan->storage = storage;
    scan->roots = roots;
    scan->only = only;
    *out = scan;
    return SKIFF_OK;
}

skiff_err skiff_saves_backup_path(const char *save_target, uint32_t number, char *out,
                                  size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (out == NULL || !skiff_saves_target_valid(save_target) || number == 0 ||
        number > SKIFF_SAVES_BACKUP_NUMBER_MAX) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const int written = snprintf(out, out_size, "%s/%s/%u%s", SKIFF_SAVES_BACKUP_FOLDER,
                                 save_target, (unsigned)number, SKIFF_SAVES_UNIT_EXTENSION);
    if (written < 0 || (size_t)written >= out_size) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    return SKIFF_OK;
}

/* Writes live as a unit to a draft, then renames it to the backup's name, retrying the write once
 * after an I/O error (a suspend). */
static skiff_err write_backup(skiff_storage *storage, const skiff_storage_roots *roots,
                              skiff_saves_unit *live, uint32_t number) {
    char logical[SKIFF_STORAGE_PATH_MAX];
    char draft_logical[SKIFF_STORAGE_PATH_MAX];
    char path[SKIFF_STORAGE_PATH_MAX];
    char draft[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = skiff_saves_backup_path(live->save_target, number, logical, sizeof logical);
    if (err == SKIFF_OK) {
        const int written = snprintf(draft_logical, sizeof draft_logical, "%s%s", logical,
                                     SKIFF_STORAGE_DRAFT_SUFFIX);
        err = written > 0 && (size_t)written < sizeof draft_logical ? SKIFF_OK
                                                                    : SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    if (err == SKIFF_OK) {
        err = skiff_storage_resolve(roots, logical, path, sizeof path);
    }
    if (err == SKIFF_OK) {
        err = skiff_storage_resolve(roots, draft_logical, draft, sizeof draft);
    }
    if (err != SKIFF_OK) {
        return err;
    }
    err = SKIFF_ERR_STORAGE_IO;
    for (int attempt = 0; attempt < 2 && err == SKIFF_ERR_STORAGE_IO; attempt++) {
        err = skiff_saves_write_unit(storage, roots, live, draft_logical);
    }
    if (err == SKIFF_OK) {
        err = skiff_storage_rename(storage, draft, path);
    }
    if (err != SKIFF_OK) {
        (void)skiff_storage_remove(storage, draft);
    }
    return err;
}

skiff_err saves_backup_write(skiff_storage *storage, const skiff_storage_roots *roots,
                             skiff_saves_unit *live, uint32_t *number) {
    *number = 0;
    backup_scan *scan = NULL;
    skiff_err err = scan_create(storage, roots, NULL, &scan);
    if (err != SKIFF_OK) {
        return err;
    }
    err = scan_backups(scan);
    const uint32_t next = scan->highest + 1;
    const int overflow = scan->overflow;
    free(scan);
    if (err == SKIFF_OK && (overflow || next > SKIFF_SAVES_BACKUP_NUMBER_MAX)) {
        err = SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    char folder[SKIFF_STORAGE_PATH_MAX];
    if (err == SKIFF_OK) {
        err = saves_resolve(roots, SKIFF_SAVES_BACKUP_FOLDER, live->save_target, NULL, folder,
                            sizeof folder);
    }
    if (err == SKIFF_OK) {
        err = skiff_storage_mkdirs(storage, folder);
    }
    if (err == SKIFF_OK) {
        err = write_backup(storage, roots, live, next);
    }
    if (err == SKIFF_OK) {
        *number = next;
    }
    return err;
}

static void drop(backup_scan *scan, backup_entry *backup) {
    char path[SKIFF_STORAGE_PATH_MAX];
    if (saves_resolve(scan->roots, SKIFF_SAVES_BACKUP_FOLDER, backup->folder, backup->name, path,
                      sizeof path) == SKIFF_OK) {
        (void)skiff_storage_remove(scan->storage, path);
    }
    backup->dropped = 1;
}

/* Backups of the same game with a higher number than backup's. */
static size_t newer_of_game(const backup_scan *scan, const backup_entry *backup) {
    size_t newer = 0;
    for (size_t i = 0; i < scan->count; i++) {
        const backup_entry *other = &scan->entries[i];
        newer += !other->draft && other->number > backup->number &&
                         strcmp(other->folder, backup->folder) == 0
                     ? 1
                     : 0;
    }
    return newer;
}

static void prune(backup_scan *scan, uint32_t keep) {
    uint64_t total = 0;
    for (size_t i = 0; i < scan->count; i++) {
        backup_entry *backup = &scan->entries[i];
        if (backup->draft || newer_of_game(scan, backup) >= SKIFF_SAVES_BACKUPS_PER_GAME) {
            drop(scan, backup);
        } else {
            total += backup->size;
        }
    }
    while (total > SKIFF_SAVES_BACKUPS_BYTES_MAX) {
        backup_entry *oldest = NULL;
        for (size_t i = 0; i < scan->count; i++) {
            backup_entry *backup = &scan->entries[i];
            if (!backup->dropped && backup->number != keep &&
                (oldest == NULL || backup->number < oldest->number)) {
                oldest = backup;
            }
        }
        if (oldest == NULL) {
            break;
        }
        total -= oldest->size;
        drop(scan, oldest);
    }
    char folder[SKIFF_STORAGE_PATH_MAX];
    for (size_t i = 0; i < scan->folder_count; i++) {
        /* An emptied game folder goes; rmdir refuses one that still holds anything. */
        if (saves_resolve(scan->roots, SKIFF_SAVES_BACKUP_FOLDER, scan->folders[i], NULL, folder,
                          sizeof folder) == SKIFF_OK) {
            (void)skiff_storage_rmdir(scan->storage, folder);
        }
    }
}

void saves_backups_prune(skiff_storage *storage, const skiff_storage_roots *roots, uint32_t keep) {
    backup_scan *scan = NULL;
    if (scan_create(storage, roots, NULL, &scan) != SKIFF_OK) {
        return;
    }
    if (scan_backups(scan) == SKIFF_OK && !scan->overflow) {
        prune(scan, keep);
    }
    free(scan);
}

static int newest_first(const void *a, const void *b) {
    const uint32_t x = ((const skiff_saves_backup *)a)->number;
    const uint32_t y = ((const skiff_saves_backup *)b)->number;
    return (x < y) - (x > y);
}

skiff_err skiff_saves_backups_list(skiff_storage *storage, const skiff_storage_roots *roots,
                                   const char *save_target, skiff_saves_backup_list *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (storage == NULL || roots == NULL || out == NULL || !skiff_saves_target_valid(save_target)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    backup_scan *scan = NULL;
    skiff_err err = scan_create(storage, roots, save_target, &scan);
    if (err != SKIFF_OK) {
        return err;
    }
    err = scan_backups(scan);
    if (err == SKIFF_OK && scan->overflow) {
        err = SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    /* The newest SKIFF_SAVES_BACKUPS_PER_GAME, found one place at a time. */
    skiff_saves_backup kept[SKIFF_SAVES_BACKUPS_PER_GAME + 1];
    size_t count = 0;
    for (size_t i = 0; err == SKIFF_OK && i < scan->count; i++) {
        const backup_entry *backup = &scan->entries[i];
        if (backup->draft) {
            continue;
        }
        kept[count].number = backup->number;
        kept[count].size = backup->size;
        kept[count].mtime = backup->mtime;
        count++;
        qsort(kept, count, sizeof kept[0], newest_first);
        count = count > SKIFF_SAVES_BACKUPS_PER_GAME ? SKIFF_SAVES_BACKUPS_PER_GAME : count;
    }
    free(scan);
    if (err != SKIFF_OK) {
        return err;
    }
    out->count = count;
    memcpy(out->backups, kept, count * sizeof kept[0]);
    return SKIFF_OK;
}

skiff_err skiff_saves_restore_backup(skiff_storage *storage, const skiff_storage_roots *roots,
                                     const char *save_target, uint32_t number,
                                     skiff_saves_restore_report *report) {
    if (report != NULL) {
        memset(report, 0, sizeof *report);
    }
    if (storage == NULL || roots == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    char logical[SKIFF_STORAGE_PATH_MAX];
    char path[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = skiff_saves_backup_path(save_target, number, logical, sizeof logical);
    if (err == SKIFF_OK) {
        err = skiff_storage_resolve(roots, logical, path, sizeof path);
    }
    skiff_storage_info info;
    if (err == SKIFF_OK) {
        err = skiff_storage_stat(storage, path, &info);
    }
    if (err == SKIFF_OK && info.is_dir) {
        err = SKIFF_ERR_STORAGE_NOT_FOUND;
    }
    return err == SKIFF_OK ? skiff_saves_restore(storage, roots, logical, save_target, report)
                           : err;
}
