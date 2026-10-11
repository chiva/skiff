/*
 * Restoring a unit: staged, journalled and finished at launch when cut short. The steps, and what a
 * cut at each leaves, are in skiff/saves_restore.h; the one rule behind them is that the live saves
 * change only in step 5, and only while a journal on the device says how to finish it from a stage
 * that is complete.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/saves_restore.h"

#include "saves_internal.h"

#define KEY_VERSION "version"
#define KEY_DONE "done"
#define KEY_SAVE_TARGET "save_target"
#define KEY_BACKUP "backup"
#define KEY_REPLACE "replace"
#define KEY_STAGE "stage"
#define KEY_LIVE "live"
#define KEY_REMOVE "remove"
#define KEY_FILES "files"
#define KEY_NAME "name"
#define KEY_SIZE "size"
/* Structural characters a journal Skiff wrote holds at most: 4 per file, 4 per folder replaced, 1
 * per folder removed, and the top level's, with room to spare. */
#define JOURNAL_TOKENS_MAX                                                                         \
    ((size_t)SKIFF_SAVES_FILES_MAX * 6 + (size_t)SKIFF_SAVES_FOLDERS_MAX * 8 + 32)
/* Entries one listing keeps: a save folder's files, or the stage's folders. */
#define LISTING_MAX SKIFF_SAVES_FILES_MAX
/* The largest cluster FAT32 gives a Memory Stick (32 GB and larger cards): every file and folder
 * takes whole clusters, so many small save files take far more than their bytes. */
#define SAVES_CLUSTER_BYTES ((uint64_t)32 * 1024)
/* A step that fails with SKIFF_ERR_STORAGE_IO is tried once more: on the PSP that is what a handle
 * lost to a suspend returns, and reopening the file is enough. */
#define ATTEMPTS 2

typedef struct journal_folder {
    /* The folder's name in the unit and the stage, and on the Memory Stick: the live folder's own
     * spelling when it exists, since a listing may give it in capitals. */
    char stage[SKIFF_SAVES_NAME_SIZE];
    char live[SKIFF_SAVES_NAME_SIZE];
} journal_folder;

typedef struct journal_file {
    /* "<stage folder>/<file>". */
    char name[SKIFF_SAVES_ENTRY_SIZE];
    uint64_t size;
} journal_file;

typedef struct journal {
    /* Step 5 finished: only the stage and the journal are left to remove. */
    int done;
    char save_target[SKIFF_SAVES_TARGET_SIZE];
    uint32_t backup;
    size_t replace_count;
    journal_folder replace[SKIFF_SAVES_FOLDERS_MAX];
    size_t remove_count;
    char remove[SKIFF_SAVES_FOLDERS_MAX][SKIFF_SAVES_NAME_SIZE];
    size_t file_count;
    journal_file files[SKIFF_SAVES_FILES_MAX];
} journal;

typedef struct listing {
    /* Only names starting with this, ignoring case (a game's folders in the save root, which may
     * hold hundreds of other entries), or every name (NULL). */
    const char *prefix;
    size_t count;
    /* A name too long to list, or more than LISTING_MAX entries: the folder cannot be emptied
     * safely, since not every entry was seen. */
    int incomplete;
    char names[LISTING_MAX][SKIFF_STORAGE_NAME_MAX];
    unsigned char is_dir[LISTING_MAX];
} listing;

typedef struct restorer {
    skiff_storage *storage;
    const skiff_storage_roots *roots;
    skiff_saves_unit *incoming;
    skiff_saves_unit *live;
    journal journal;
    listing folders;
    listing files;
    char text[SKIFF_SAVES_JOURNAL_BYTES_MAX];
    unsigned char buffer[SAVES_BUFFER_BYTES];
} restorer;

static skiff_err close_after(skiff_file *file, skiff_err err) {
    const skiff_err close_err = skiff_file_close(file);
    return err != SKIFF_OK ? err : close_err;
}

static skiff_err resolve(const restorer *r, const char *logical, char *out) {
    return skiff_storage_resolve(r->roots, logical, out, SKIFF_STORAGE_PATH_MAX);
}

/* "<folder>/<name>", for a device path and a name a listing gave. */
static skiff_err join(const char *folder, const char *name, char *out) {
    const int written = snprintf(out, SKIFF_STORAGE_PATH_MAX, "%s/%s", folder, name);
    return written > 0 && written < SKIFF_STORAGE_PATH_MAX ? SKIFF_OK : SKIFF_ERR_BUFFER_TOO_SMALL;
}

static int is_gone(skiff_err err) { return err == SKIFF_OK || err == SKIFF_ERR_STORAGE_NOT_FOUND; }

/* ---- Folders ---- */

static skiff_storage_list_step note_entry(void *ctx, const skiff_storage_entry *entry) {
    listing *l = ctx;
    if (l->prefix != NULL && !saves_has_prefix(entry->name, l->prefix)) {
        return SKIFF_STORAGE_LIST_CONTINUE;
    }
    if (l->count == LISTING_MAX) {
        l->incomplete = 1;
        return SKIFF_STORAGE_LIST_STOP;
    }
    memcpy(l->names[l->count], entry->name, strlen(entry->name) + 1);
    l->is_dir[l->count] = (unsigned char)(entry->info.is_dir != 0);
    l->count++;
    return SKIFF_STORAGE_LIST_CONTINUE;
}

/* Lists the folder at the device path into l (only names starting with prefix, unless NULL),
 * again when the listing fails part-way. */
static skiff_err list_folder(restorer *r, const char *path, const char *prefix, listing *l) {
    l->prefix = prefix;
    skiff_err err = SKIFF_ERR_STORAGE_IO;
    for (int attempt = 0; attempt < ATTEMPTS && err == SKIFF_ERR_STORAGE_IO; attempt++) {
        l->count = 0;
        l->incomplete = 0;
        size_t skipped = 0;
        err = skiff_storage_list(r->storage, path, note_entry, l, &skipped);
        l->incomplete = l->incomplete || skipped > 0;
    }
    return err;
}

/* Removes every file directly in the folder at path. SKIFF_ERR_STORAGE_IO when it holds a folder
 * or an entry that could not be listed: nothing is removed then. SKIFF_ERR_STORAGE_NOT_FOUND for a
 * missing folder. */
static skiff_err clear_files(restorer *r, const char *path) {
    skiff_err err = list_folder(r, path, NULL, &r->files);
    if (err != SKIFF_OK) {
        return err;
    }
    if (r->files.incomplete) {
        return SKIFF_ERR_STORAGE_IO;
    }
    for (size_t i = 0; i < r->files.count; i++) {
        if (r->files.is_dir[i]) {
            return SKIFF_ERR_STORAGE_IO;
        }
    }
    char file[SKIFF_STORAGE_PATH_MAX];
    for (size_t i = 0; err == SKIFF_OK && i < r->files.count; i++) {
        err = join(path, r->files.names[i], file);
        if (err == SKIFF_OK) {
            err = skiff_storage_remove(r->storage, file);
            err = is_gone(err) ? SKIFF_OK : err;
        }
    }
    return err;
}

/* Removes the folder at path whole: its files, then the emptied folder. A missing folder is gone
 * already. */
static skiff_err remove_folder(restorer *r, const char *path) {
    skiff_err err = clear_files(r, path);
    if (err == SKIFF_OK) {
        err = skiff_storage_rmdir(r->storage, path);
    }
    return is_gone(err) ? SKIFF_OK : err;
}

/* Removes the stage: each folder's files, the folders, then the stage. A missing stage is fine. */
static skiff_err remove_stage(restorer *r) {
    char stage[SKIFF_STORAGE_PATH_MAX];
    char child[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = resolve(r, SKIFF_SAVES_STAGE_FOLDER, stage);
    if (err == SKIFF_OK) {
        err = list_folder(r, stage, NULL, &r->folders);
    }
    if (err == SKIFF_ERR_STORAGE_NOT_FOUND) {
        return SKIFF_OK;
    }
    if (err == SKIFF_OK && r->folders.incomplete) {
        err = SKIFF_ERR_STORAGE_IO;
    }
    for (size_t i = 0; err == SKIFF_OK && i < r->folders.count; i++) {
        err = join(stage, r->folders.names[i], child);
        if (err == SKIFF_OK) {
            err = r->folders.is_dir[i] ? remove_folder(r, child)
                                       : skiff_storage_remove(r->storage, child);
            err = is_gone(err) ? SKIFF_OK : err;
        }
    }
    if (err == SKIFF_OK) {
        err = skiff_storage_rmdir(r->storage, stage);
    }
    return is_gone(err) ? SKIFF_OK : err;
}

/* ---- The journal ---- */

static skiff_err add_folder_name(cJSON *array, const char *name) {
    cJSON *item = cJSON_CreateString(name);
    if (item == NULL || !cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return SKIFF_ERR_NO_MEMORY;
    }
    return SKIFF_OK;
}

static skiff_err add_replace(cJSON *array, const journal_folder *folder) {
    cJSON *item = cJSON_CreateObject();
    if (item == NULL || cJSON_AddStringToObject(item, KEY_STAGE, folder->stage) == NULL ||
        cJSON_AddStringToObject(item, KEY_LIVE, folder->live) == NULL ||
        !cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return SKIFF_ERR_NO_MEMORY;
    }
    return SKIFF_OK;
}

static skiff_err add_file(cJSON *array, const journal_file *file) {
    cJSON *item = cJSON_CreateObject();
    if (item == NULL || cJSON_AddStringToObject(item, KEY_NAME, file->name) == NULL ||
        cJSON_AddNumberToObject(item, KEY_SIZE, (double)file->size) == NULL ||
        !cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return SKIFF_ERR_NO_MEMORY;
    }
    return SKIFF_OK;
}

/*
 * {"version":1,"save_target":"SKIF00001","backup":7,"replace":[{"stage":"SKIF00001DATA00",
 * "live":"SKIF00001DATA00"}],"remove":["SKIF00001DATA01"],"files":[{"name":"SKIF00001DATA00/
 * DATA.BIN","size":4096}]}, or {"version":1,"done":true} once step 5 finished.
 */
static skiff_err format_journal(const journal *j, char *out, size_t out_size, size_t *length) {
    cJSON *root = cJSON_CreateObject();
    int built = root != NULL &&
                cJSON_AddNumberToObject(root, KEY_VERSION, SKIFF_SAVES_JOURNAL_VERSION) != NULL;
    if (built && j->done) {
        built = cJSON_AddTrueToObject(root, KEY_DONE) != NULL;
    } else if (built) {
        cJSON *replace = NULL;
        cJSON *remove = NULL;
        cJSON *files = NULL;
        built = cJSON_AddStringToObject(root, KEY_SAVE_TARGET, j->save_target) != NULL &&
                cJSON_AddNumberToObject(root, KEY_BACKUP, j->backup) != NULL &&
                (replace = cJSON_AddArrayToObject(root, KEY_REPLACE)) != NULL &&
                (remove = cJSON_AddArrayToObject(root, KEY_REMOVE)) != NULL &&
                (files = cJSON_AddArrayToObject(root, KEY_FILES)) != NULL;
        for (size_t i = 0; built && i < j->replace_count; i++) {
            built = add_replace(replace, &j->replace[i]) == SKIFF_OK;
        }
        for (size_t i = 0; built && i < j->remove_count; i++) {
            built = add_folder_name(remove, j->remove[i]) == SKIFF_OK;
        }
        for (size_t i = 0; built && i < j->file_count; i++) {
            built = add_file(files, &j->files[i]) == SKIFF_OK;
        }
    }
    skiff_err err = built ? saves_json_print(root, out, out_size, length) : SKIFF_ERR_NO_MEMORY;
    cJSON_Delete(root);
    return err;
}

/* A folder name of this game's: valid, starting with its save_target. */
static int game_folder(const journal *j, const char *name) {
    return skiff_saves_name_valid(name) && saves_has_prefix(name, j->save_target);
}

static int read_replace(journal *j, const cJSON *item) {
    journal_folder *folder = &j->replace[j->replace_count];
    if (!cJSON_IsObject(item) ||
        !saves_json_string(item, KEY_STAGE, 0, folder->stage, sizeof folder->stage) ||
        !saves_json_string(item, KEY_LIVE, 0, folder->live, sizeof folder->live) ||
        !game_folder(j, folder->stage) || !game_folder(j, folder->live) ||
        !saves_same_name(folder->stage, strlen(folder->stage), folder->live,
                         strlen(folder->live))) {
        return 0;
    }
    /* FAT folds case: two such folders are one live folder, cleared twice. */
    for (size_t i = 0; i < j->replace_count; i++) {
        if (saves_same_name(j->replace[i].live, strlen(j->replace[i].live), folder->live,
                            strlen(folder->live))) {
            return 0;
        }
    }
    j->replace_count++;
    return 1;
}

static int read_remove(journal *j, const cJSON *item) {
    char *name = j->remove[j->remove_count];
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) >= sizeof j->remove[0]) {
        return 0;
    }
    memcpy(name, item->valuestring, strlen(item->valuestring) + 1);
    if (!game_folder(j, name)) {
        return 0;
    }
    /* A folder replaced and removed would lose the save just restored. */
    for (size_t i = 0; i < j->replace_count; i++) {
        if (saves_same_name(j->replace[i].live, strlen(j->replace[i].live), name, strlen(name))) {
            return 0;
        }
    }
    j->remove_count++;
    return 1;
}

/* A file "<FOLDER>/<FILE>" whose folder is one replaced, with a valid name and a size a unit can
 * hold. */
static int read_file_entry(journal *j, const cJSON *item) {
    journal_file *file = &j->files[j->file_count];
    double size = 0.0;
    if (!cJSON_IsObject(item) ||
        !saves_json_string(item, KEY_NAME, 0, file->name, sizeof file->name) ||
        !saves_json_number(item, KEY_SIZE, (double)SKIFF_SAVES_UNIT_BYTES_MAX, &size)) {
        return 0;
    }
    const char *slash = strchr(file->name, SAVES_PATH_SEPARATOR);
    if (slash == NULL || !skiff_saves_name_valid(slash + 1)) {
        return 0;
    }
    const size_t folder_length = (size_t)(slash - file->name);
    int known = 0;
    for (size_t i = 0; i < j->replace_count && !known; i++) {
        known = strlen(j->replace[i].stage) == folder_length &&
                memcmp(j->replace[i].stage, file->name, folder_length) == 0;
    }
    file->size = (uint64_t)size;
    j->file_count += known ? 1 : 0;
    return known;
}

static int fill_journal(journal *j, const cJSON *root) {
    double version = 0.0;
    if (!cJSON_IsObject(root) ||
        !saves_json_number(root, KEY_VERSION, SAVES_JSON_INTEGER_LIMIT, &version) ||
        version != SKIFF_SAVES_JOURNAL_VERSION) {
        return 0;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, KEY_DONE))) {
        j->done = 1;
        return 1;
    }
    double backup = 0.0;
    const cJSON *replace = cJSON_GetObjectItemCaseSensitive(root, KEY_REPLACE);
    const cJSON *remove = cJSON_GetObjectItemCaseSensitive(root, KEY_REMOVE);
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(root, KEY_FILES);
    if (!saves_json_string(root, KEY_SAVE_TARGET, 0, j->save_target, sizeof j->save_target) ||
        !skiff_saves_target_valid(j->save_target) ||
        !saves_json_number(root, KEY_BACKUP, SKIFF_SAVES_BACKUP_NUMBER_MAX, &backup) ||
        !cJSON_IsArray(replace) || cJSON_GetArraySize(replace) > SKIFF_SAVES_FOLDERS_MAX ||
        !cJSON_IsArray(remove) || cJSON_GetArraySize(remove) > SKIFF_SAVES_FOLDERS_MAX ||
        !cJSON_IsArray(files) || cJSON_GetArraySize(files) > SKIFF_SAVES_FILES_MAX) {
        return 0;
    }
    j->backup = (uint32_t)backup;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, replace) {
        if (!read_replace(j, item)) {
            return 0;
        }
    }
    cJSON_ArrayForEach(item, remove) {
        if (!read_remove(j, item)) {
            return 0;
        }
    }
    cJSON_ArrayForEach(item, files) {
        if (!read_file_entry(j, item)) {
            return 0;
        }
    }
    return 1;
}

/* SKIFF_ERR_INVALID_ARG for a journal Skiff cannot read, SKIFF_ERR_NO_MEMORY. */
static skiff_err parse_journal(journal *j, const char *text, size_t length) {
    memset(j, 0, sizeof *j);
    cJSON *root = NULL;
    skiff_err err = saves_json_parse(text, length, JOURNAL_TOKENS_MAX, &root);
    if (err == SKIFF_OK && !fill_journal(j, root)) {
        err = SKIFF_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);
    if (err != SKIFF_OK) {
        memset(j, 0, sizeof *j);
    }
    return err;
}

/* What reading the journal found. */
typedef enum journal_state { JOURNAL_NONE, JOURNAL_READ, JOURNAL_DAMAGED } journal_state;

static skiff_err read_journal(restorer *r, journal_state *state) {
    *state = JOURNAL_NONE;
    char path[SKIFF_STORAGE_PATH_MAX];
    size_t length = 0;
    skiff_err err = resolve(r, SKIFF_SAVES_JOURNAL_PATH, path);
    if (err == SKIFF_OK) {
        err = skiff_storage_read_whole(r->storage, path, r->text, sizeof r->text, &length);
    }
    if (err == SKIFF_ERR_BUFFER_TOO_SMALL) {
        *state = JOURNAL_DAMAGED;
        return SKIFF_OK;
    }
    if (err == SKIFF_OK && length == 0) {
        /* No journal, or an empty one: a cut that emptied it still left a restore unfinished. */
        skiff_storage_info info;
        err = skiff_storage_stat(r->storage, path, &info);
        *state = err == SKIFF_OK ? JOURNAL_DAMAGED : JOURNAL_NONE;
        return err == SKIFF_ERR_STORAGE_NOT_FOUND ? SKIFF_OK : err;
    }
    if (err != SKIFF_OK) {
        return err;
    }
    err = parse_journal(&r->journal, r->text, length);
    if (err == SKIFF_ERR_INVALID_ARG) {
        *state = JOURNAL_DAMAGED;
        return SKIFF_OK;
    }
    *state = err == SKIFF_OK ? JOURNAL_READ : JOURNAL_NONE;
    return err;
}

static skiff_err write_journal(restorer *r) {
    char path[SKIFF_STORAGE_PATH_MAX];
    size_t length = 0;
    skiff_err err = format_journal(&r->journal, r->text, sizeof r->text, &length);
    if (err == SKIFF_OK) {
        err = resolve(r, SKIFF_SAVES_JOURNAL_PATH, path);
    }
    return err == SKIFF_OK ? skiff_storage_replace_whole(r->storage, path, r->text, length) : err;
}

/* Removes the journal and what a replacement of it may have left (its draft first, then the
 * pending copy, which a read would otherwise put back, then the journal). SKIFF_OK only when none
 * is left. */
static skiff_err clear_journal(restorer *r) {
    static const char *const SUFFIXES[] = {SKIFF_STORAGE_DRAFT_SUFFIX, SKIFF_STORAGE_PENDING_SUFFIX,
                                           ""};
    char journal_path[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = resolve(r, SKIFF_SAVES_JOURNAL_PATH, journal_path);
    for (size_t i = 0; err == SKIFF_OK && i < sizeof SUFFIXES / sizeof SUFFIXES[0]; i++) {
        char path[SKIFF_STORAGE_PATH_MAX];
        const int written = snprintf(path, sizeof path, "%s%s", journal_path, SUFFIXES[i]);
        err = written > 0 && (size_t)written < sizeof path ? SKIFF_OK : SKIFF_ERR_BUFFER_TOO_SMALL;
        if (err == SKIFF_OK) {
            err = skiff_storage_remove(r->storage, path);
            err = is_gone(err) ? SKIFF_OK : err;
        }
    }
    return err;
}

/* ---- Step 5: the live folders ---- */

static skiff_err copy_once(restorer *r, const char *from, const char *to, uint64_t size) {
    skiff_file *in = NULL;
    skiff_file *out = NULL;
    skiff_err err = skiff_storage_open(r->storage, from, SKIFF_FILE_READ, 0, &in);
    if (err == SKIFF_OK) {
        err = skiff_storage_open(r->storage, to, SKIFF_FILE_REPLACE, 0, &out);
    }
    uint64_t copied = 0;
    size_t got = 1;
    while (err == SKIFF_OK && got > 0) {
        err = skiff_file_read(in, r->buffer, sizeof r->buffer, &got);
        if (err == SKIFF_OK && got > 0) {
            copied += got;
            err = copied > size ? SKIFF_ERR_STORAGE_IO : skiff_file_write(out, r->buffer, got);
        }
    }
    if (err == SKIFF_OK && copied != size) {
        err = SKIFF_ERR_STORAGE_IO;
    }
    if (err == SKIFF_OK) {
        err = skiff_file_sync(out);
    }
    err = close_after(out, err);
    return close_after(in, err);
}

/* Copies a staged file over, reopening both files and starting again once after an I/O error. A
 * copy that still fails is removed. */
static skiff_err copy_file(restorer *r, const char *from, const char *to, uint64_t size) {
    skiff_err err = SKIFF_ERR_STORAGE_IO;
    for (int attempt = 0; attempt < ATTEMPTS && err == SKIFF_ERR_STORAGE_IO; attempt++) {
        err = copy_once(r, from, to, size);
    }
    if (err != SKIFF_OK) {
        (void)skiff_storage_remove(r->storage, to);
    }
    return err;
}

/* Every journalled folder and file is in the stage, each file its journalled size:
 * SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED otherwise, the storage's error when it cannot tell. */
static skiff_err check_stage(restorer *r) {
    const journal *j = &r->journal;
    char path[SKIFF_STORAGE_PATH_MAX];
    skiff_storage_info info;
    skiff_err err = SKIFF_OK;
    for (size_t i = 0; err == SKIFF_OK && i < j->replace_count; i++) {
        err = saves_resolve(r->roots, SKIFF_SAVES_STAGE_FOLDER, j->replace[i].stage, NULL, path,
                            sizeof path);
        if (err == SKIFF_OK) {
            err = skiff_storage_stat(r->storage, path, &info);
        }
        if (err == SKIFF_ERR_STORAGE_NOT_FOUND || (err == SKIFF_OK && !info.is_dir)) {
            err = SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED;
        }
    }
    for (size_t i = 0; err == SKIFF_OK && i < j->file_count; i++) {
        err = saves_resolve(r->roots, SKIFF_SAVES_STAGE_FOLDER, j->files[i].name, NULL, path,
                            sizeof path);
        if (err == SKIFF_OK) {
            err = skiff_storage_stat(r->storage, path, &info);
        }
        if (err == SKIFF_ERR_STORAGE_NOT_FOUND ||
            (err == SKIFF_OK && (info.is_dir || info.size != j->files[i].size))) {
            err = SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED;
        }
    }
    return err;
}

/* The live folder exists as a folder: created when missing, with the save root (a Memory Stick
 * that never held a save has no PSP/SAVEDATA). */
static skiff_err ensure_folder(restorer *r, const char *path) {
    skiff_storage_info info;
    skiff_err err = skiff_storage_stat(r->storage, path, &info);
    if (err == SKIFF_ERR_STORAGE_NOT_FOUND) {
        return skiff_storage_mkdirs(r->storage, path);
    }
    return err == SKIFF_OK && !info.is_dir ? SKIFF_ERR_STORAGE_IO : err;
}

/* One folder of step 5: the live files removed, the staged ones copied in, each synced. */
static skiff_err replace_folder(restorer *r, const journal_folder *folder) {
    char live[SKIFF_STORAGE_PATH_MAX];
    char from[SKIFF_STORAGE_PATH_MAX];
    char to[SKIFF_STORAGE_PATH_MAX];
    skiff_err err =
        saves_resolve(r->roots, SKIFF_STORAGE_ROOT_SAVES, folder->live, NULL, live, sizeof live);
    if (err == SKIFF_OK) {
        err = ensure_folder(r, live);
    }
    if (err == SKIFF_OK) {
        err = clear_files(r, live);
    }
    const size_t folder_length = strlen(folder->stage);
    for (size_t i = 0; err == SKIFF_OK && i < r->journal.file_count; i++) {
        const journal_file *file = &r->journal.files[i];
        if (strncmp(file->name, folder->stage, folder_length) != 0 ||
            file->name[folder_length] != SAVES_PATH_SEPARATOR) {
            continue;
        }
        err =
            saves_resolve(r->roots, SKIFF_SAVES_STAGE_FOLDER, file->name, NULL, from, sizeof from);
        if (err == SKIFF_OK) {
            err = saves_resolve(r->roots, SKIFF_STORAGE_ROOT_SAVES, folder->live,
                                file->name + folder_length + 1, to, sizeof to);
        }
        if (err == SKIFF_OK) {
            err = copy_file(r, from, to, file->size);
        }
    }
    return err;
}

/* Step 5, from the journal: idempotent, so a redo after a cut anywhere in it ends the same. */
static skiff_err apply_journal(restorer *r) {
    skiff_err err = check_stage(r);
    for (size_t i = 0; err == SKIFF_OK && i < r->journal.replace_count; i++) {
        err = replace_folder(r, &r->journal.replace[i]);
    }
    char live[SKIFF_STORAGE_PATH_MAX];
    for (size_t i = 0; err == SKIFF_OK && i < r->journal.remove_count; i++) {
        err = saves_resolve(r->roots, SKIFF_STORAGE_ROOT_SAVES, r->journal.remove[i], NULL, live,
                            sizeof live);
        if (err == SKIFF_OK) {
            err = remove_folder(r, live);
        }
    }
    return err;
}

/* Step 6: the done mark is on the device (replace_whole syncs it) before the stage goes, so a cut
 * while removing the stage never leads a recovery to a half-removed stage. */
static skiff_err finish(restorer *r) {
    r->journal.done = 1;
    skiff_err err = write_journal(r);
    if (err == SKIFF_OK) {
        err = remove_stage(r);
    }
    return err == SKIFF_OK ? clear_journal(r) : err;
}

/* ---- Recovery ---- */

/* A journal that cannot be applied: the live saves stay as they are, and the journal goes before
 * the stage, so a cut here never leaves a journal pointing at a half-removed stage. */
static skiff_err give_up(restorer *r) {
    skiff_err err = clear_journal(r);
    if (err == SKIFF_OK) {
        err = remove_stage(r);
    }
    return err != SKIFF_OK ? err : SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED;
}

static skiff_err recover(restorer *r, skiff_saves_recovery *out) {
    memset(out, 0, sizeof *out);
    journal_state state = JOURNAL_NONE;
    skiff_err err = read_journal(r, &state);
    if (err != SKIFF_OK) {
        return err;
    }
    if (state == JOURNAL_NONE) {
        /* A stage without a journal: a restore cut before step 5, the live saves untouched. */
        return remove_stage(r);
    }
    if (state == JOURNAL_DAMAGED) {
        out->outcome = SKIFF_SAVES_RECOVERY_UNFINISHED;
        return give_up(r);
    }
    if (r->journal.done) {
        err = remove_stage(r);
        return err == SKIFF_OK ? clear_journal(r) : err;
    }
    memcpy(out->save_target, r->journal.save_target, sizeof out->save_target);
    out->backup = r->journal.backup;
    err = apply_journal(r);
    if (err == SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED) {
        out->outcome = SKIFF_SAVES_RECOVERY_UNFINISHED;
        return give_up(r);
    }
    if (err == SKIFF_OK) {
        out->outcome = SKIFF_SAVES_RECOVERY_FINISHED;
        err = finish(r);
    }
    /* The cut restore never reached its pruning; its own backup is kept. */
    if (err == SKIFF_OK && out->backup != 0) {
        saves_backups_prune(r->storage, r->roots, out->backup);
    }
    return err == SKIFF_OK ? SKIFF_ERR_STORAGE_SAVE_RESTORE_FINISHED : err;
}

/* ---- Restoring ---- */

static skiff_err read_unit(restorer *r, const char *unit_path, const char *save_target,
                           const char *staging) {
    skiff_err err = SKIFF_ERR_STORAGE_IO;
    for (int attempt = 0; attempt < ATTEMPTS && err == SKIFF_ERR_STORAGE_IO; attempt++) {
        err = skiff_saves_read_unit(r->storage, r->roots, unit_path, save_target, staging,
                                    r->incoming);
    }
    return err;
}

static skiff_err collect_live(restorer *r, const char *save_target) {
    skiff_err err = SKIFF_ERR_STORAGE_IO;
    for (int attempt = 0; attempt < ATTEMPTS && err == SKIFF_ERR_STORAGE_IO; attempt++) {
        err = skiff_saves_collect(r->storage, r->roots, SKIFF_STORAGE_ROOT_SAVES, save_target,
                                  r->live);
    }
    return err;
}

/* The live folder name matches, ignoring case; NULL when none does. */
static const char *live_match(const skiff_saves_unit *live, const char *name) {
    for (size_t i = 0; i < live->folder_count; i++) {
        if (saves_same_name(live->folders[i], strlen(live->folders[i]), name, strlen(name))) {
            return live->folders[i];
        }
    }
    return NULL;
}

/* What the root's listing holds under name, ignoring case: 0 nothing, 1 a folder, 2 a file. */
static int listed_as(const listing *l, const char *name) {
    for (size_t i = 0; i < l->count; i++) {
        if (saves_same_name(l->names[i], strlen(l->names[i]), name, strlen(name))) {
            return l->is_dir[i] ? 1 : 2;
        }
    }
    return 0;
}

/*
 * The journal for this restore: each unit folder replaces its live folder (in the live spelling),
 * unless the Memory Stick holds a game's installed data under that name (a prefix folder the
 * collect left out), which is kept; live save folders the unit lacks are removed. A unit folder
 * whose name a file holds is refused (307): step 5 could never create it.
 */
static skiff_err plan(restorer *r, const char *save_target, skiff_saves_restore_report *report) {
    journal *j = &r->journal;
    memset(j, 0, sizeof *j);
    memcpy(j->save_target, save_target, strlen(save_target) + 1);
    char root[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = resolve(r, SKIFF_STORAGE_ROOT_SAVES, root);
    if (err == SKIFF_OK) {
        err = list_folder(r, root, save_target, &r->folders);
    }
    if (err == SKIFF_ERR_STORAGE_NOT_FOUND) {
        r->folders.count = 0;
        err = SKIFF_OK;
    }
    /* Every one of the game's names must be seen, or an install could pass for a missing folder;
     * a name too long to list cannot be a save folder's and is ignored, as the collect does. */
    if (err == SKIFF_OK && r->folders.count == LISTING_MAX) {
        err = SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE;
    }
    const skiff_saves_unit *in = r->incoming;
    for (size_t i = 0; err == SKIFF_OK && i < in->folder_count; i++) {
        const char *live = live_match(r->live, in->folders[i]);
        const int listed = listed_as(&r->folders, in->folders[i]);
        if (live == NULL && listed == 2) {
            err = SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE;
        } else if (live == NULL && listed == 1) {
            report->installs_kept++;
        } else {
            journal_folder *folder = &j->replace[j->replace_count++];
            memcpy(folder->stage, in->folders[i], sizeof folder->stage);
            memcpy(folder->live, live != NULL ? live : in->folders[i], sizeof folder->live);
        }
    }
    for (size_t i = 0; err == SKIFF_OK && i < in->file_count; i++) {
        const char *name = in->files[i].name;
        const size_t folder_length = (size_t)(strchr(name, SAVES_PATH_SEPARATOR) - name);
        for (size_t f = 0; f < j->replace_count; f++) {
            if (strlen(j->replace[f].stage) == folder_length &&
                memcmp(j->replace[f].stage, name, folder_length) == 0) {
                journal_file *file = &j->files[j->file_count++];
                memcpy(file->name, name, sizeof file->name);
                file->size = in->files[i].size;
                break;
            }
        }
    }
    for (size_t i = 0; err == SKIFF_OK && i < r->live->folder_count; i++) {
        const char *name = r->live->folders[i];
        int kept = 0;
        for (size_t f = 0; f < in->folder_count && !kept; f++) {
            kept = saves_same_name(in->folders[f], strlen(in->folders[f]), name, strlen(name));
        }
        if (!kept) {
            memcpy(j->remove[j->remove_count++], name, sizeof j->remove[0]);
        }
    }
    return err;
}

/* What a file of size bytes takes on FAT: whole clusters, and at least one even when empty (its
 * directory entry). */
static uint64_t on_disk(uint64_t size) {
    return (size / SAVES_CLUSTER_BYTES + 1) * SAVES_CLUSTER_BYTES;
}

/* The unit's files, written once, with a cluster per folder for its directory. */
static uint64_t unit_on_disk(const skiff_saves_unit *unit) {
    uint64_t bytes = (uint64_t)unit->folder_count * SAVES_CLUSTER_BYTES;
    for (size_t i = 0; i < unit->file_count; i++) {
        bytes += on_disk(unit->files[i].size);
    }
    return bytes;
}

/* A unit of the live files: their bytes, and per file a local and a central header with the name,
 * and the end record, in one file; and the game's backup folder. */
static uint64_t backup_on_disk(const skiff_saves_unit *live) {
    if (live->file_count == 0) {
        return 0;
    }
    uint64_t bytes = ZIP_END_BYTES;
    for (size_t i = 0; i < live->file_count; i++) {
        bytes += live->files[i].size + ZIP_LOCAL_BYTES + ZIP_CENTRAL_BYTES +
                 2 * (uint64_t)strlen(live->files[i].name);
    }
    return on_disk(bytes) + SAVES_CLUSTER_BYTES;
}

/* The stage and the new live folders (each the unit's files; the old ones are only freed folder by
 * folder), the backup and the journal, counted in the largest clusters a Memory Stick uses, with
 * the free margin: a full Memory Stick must never stop step 5, or every recovery would fail the
 * same way. */
static skiff_err check_room(restorer *r) {
    char app[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = resolve(r, SKIFF_STORAGE_ROOT_APP, app);
    if (err == SKIFF_OK) {
        const uint64_t needed = 2 * unit_on_disk(r->incoming) + backup_on_disk(r->live) +
                                on_disk(SKIFF_SAVES_JOURNAL_BYTES_MAX) + SAVES_CLUSTER_BYTES;
        err = skiff_storage_check_room(r->storage, app, needed);
    }
    return err == SKIFF_ERR_NOT_IMPLEMENTED ? SKIFF_OK : err;
}

static skiff_err extract(restorer *r, const char *unit_path, const char *save_target) {
    char checked[SKIFF_SAVES_HASH_SIZE];
    memcpy(checked, r->incoming->content_hash, sizeof checked);
    skiff_err err = read_unit(r, unit_path, save_target, SKIFF_SAVES_STAGE_FOLDER);
    if (err == SKIFF_OK && strcmp(checked, r->incoming->content_hash) != 0) {
        err = SKIFF_ERR_STORAGE_IO;
    }
    if (err != SKIFF_OK) {
        (void)remove_stage(r);
    }
    return err;
}

/* Steps 1 to 6, after any earlier restore was finished. Before the journal is on the device every
 * failure removes the stage, so the Memory Stick is as it was but for a backup. */
static skiff_err restore(restorer *r, const char *unit_path, const char *save_target,
                         skiff_saves_restore_report *report) {
    skiff_saves_recovery recovery;
    skiff_err err = recover(r, &recovery);
    if (err == SKIFF_ERR_STORAGE_SAVE_RESTORE_FINISHED ||
        err == SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED) {
        report->recovered = err;
        err = SKIFF_OK;
    }
    if (err == SKIFF_OK) {
        err = read_unit(r, unit_path, save_target, NULL);
    }
    if (err == SKIFF_OK) {
        err = collect_live(r, save_target);
    }
    if (err == SKIFF_OK) {
        err = plan(r, save_target, report);
    }
    if (err == SKIFF_OK) {
        err = check_room(r);
    }
    if (err == SKIFF_OK) {
        err = extract(r, unit_path, save_target);
    }
    if (err != SKIFF_OK) {
        return err;
    }
    if (r->live->file_count > 0) {
        err = saves_backup_write(r->storage, r->roots, r->live, &r->journal.backup);
    }
    if (err == SKIFF_OK) {
        err = write_journal(r);
        if (err != SKIFF_OK && clear_journal(r) != SKIFF_OK) {
            /* A journal may still be on the device: its stage must stay for the recovery. */
            return err;
        }
    }
    if (err != SKIFF_OK) {
        (void)remove_stage(r);
        return err;
    }
    err = apply_journal(r);
    if (err != SKIFF_OK) {
        return err;
    }
    memcpy(report->content_hash, r->incoming->content_hash, sizeof report->content_hash);
    report->backup = r->journal.backup;
    report->folders_replaced = r->journal.replace_count;
    report->folders_removed = r->journal.remove_count;
    err = finish(r);
    if (err == SKIFF_OK && report->backup != 0) {
        saves_backups_prune(r->storage, r->roots, report->backup);
    }
    return err;
}

static skiff_err restorer_create(skiff_storage *storage, const skiff_storage_roots *roots,
                                 restorer **out) {
    restorer *r = calloc(1, sizeof *r);
    if (r == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    r->storage = storage;
    r->roots = roots;
    if (skiff_saves_unit_create(&r->incoming) != SKIFF_OK ||
        skiff_saves_unit_create(&r->live) != SKIFF_OK) {
        skiff_saves_unit_destroy(r->incoming);
        free(r);
        return SKIFF_ERR_NO_MEMORY;
    }
    *out = r;
    return SKIFF_OK;
}

static void restorer_destroy(restorer *r) {
    skiff_saves_unit_destroy(r->incoming);
    skiff_saves_unit_destroy(r->live);
    free(r);
}

skiff_err skiff_saves_work_unit_path(const char *save_target, char *out, size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (out == NULL || !skiff_saves_target_valid(save_target)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const int written = snprintf(out, out_size, "%s/%s%s", SKIFF_SAVES_WORK_FOLDER, save_target,
                                 SKIFF_SAVES_UNIT_EXTENSION);
    if (written < 0 || (size_t)written >= out_size) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    return SKIFF_OK;
}

skiff_err skiff_saves_restore(skiff_storage *storage, const skiff_storage_roots *roots,
                              const char *unit_path, const char *save_target,
                              skiff_saves_restore_report *report) {
    skiff_saves_restore_report done;
    memset(&done, 0, sizeof done);
    if (report != NULL) {
        memset(report, 0, sizeof *report);
    }
    if (storage == NULL || roots == NULL || unit_path == NULL ||
        !skiff_saves_target_valid(save_target)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    restorer *r = NULL;
    skiff_err err = restorer_create(storage, roots, &r);
    if (err != SKIFF_OK) {
        return err;
    }
    err = restore(r, unit_path, save_target, &done);
    restorer_destroy(r);
    if (err == SKIFF_OK && report != NULL) {
        *report = done;
    }
    return err;
}

skiff_err skiff_saves_recover(skiff_storage *storage, const skiff_storage_roots *roots,
                              skiff_saves_recovery *recovery) {
    skiff_saves_recovery found;
    memset(&found, 0, sizeof found);
    if (recovery != NULL) {
        *recovery = found;
    }
    if (storage == NULL || roots == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    restorer *r = NULL;
    skiff_err err = restorer_create(storage, roots, &r);
    if (err != SKIFF_OK) {
        return err;
    }
    err = recover(r, &found);
    restorer_destroy(r);
    if (recovery != NULL) {
        *recovery = found;
    }
    return err;
}
