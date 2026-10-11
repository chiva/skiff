/*
 * Skiff's sync baseline (app:/saves.json), written crash-safely and loaded tolerantly like
 * installed.json, and the folder-listing summary that spares hashing a game nothing changed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/saves_sync.h"

#include "saves_internal.h"

#define KEY_VERSION "version"
#define KEY_GAMES "games"
#define KEY_SAVE_TARGET "save_target"
#define KEY_ROM_ID "rom_id"
#define KEY_SAVE_ID "save_id"
#define KEY_SERVER_HASH "server_hash"
#define KEY_LOCAL_HASH "local_hash"
#define KEY_SYNCED_MS "synced_ms"
#define KEY_FILES "files"
#define KEY_TREE "tree"
/* "files" for SKIFF_SAVES_TREE_UNKNOWN. */
#define FILES_UNKNOWN (-1)
/* Structural characters per game (9 keys, their commas and the object) and the top level's, with
 * room to spare. */
#define BASELINE_TOKENS_MAX ((size_t)SKIFF_SAVES_BASELINE_GAMES_MAX * 24 + 64)
/* "<FOLDER>/<FILE>:<size>:<mtime>\n" with 64-bit numbers. */
#define TREE_LINE_SIZE (SKIFF_SAVES_ENTRY_SIZE + 48)

/* ---- Entries ---- */

static int entry_valid(const skiff_saves_baseline_entry *entry) {
    return memchr(entry->save_target, '\0', sizeof entry->save_target) != NULL &&
           skiff_saves_target_valid(entry->save_target) && entry->rom_id > 0 &&
           entry->rom_id <= SKIFF_SAVES_ID_MAX && entry->server_save_id > 0 &&
           entry->server_save_id <= SKIFF_SAVES_ID_MAX &&
           memchr(entry->server_hash, '\0', sizeof entry->server_hash) != NULL &&
           saves_hash_valid(entry->server_hash) &&
           memchr(entry->local_hash, '\0', sizeof entry->local_hash) != NULL &&
           saves_hash_valid(entry->local_hash) && entry->synced_ms >= 0 &&
           (double)entry->synced_ms < SAVES_JSON_INTEGER_LIMIT &&
           memchr(entry->tree.digest, '\0', sizeof entry->tree.digest) != NULL &&
           (entry->tree.file_count == SKIFF_SAVES_TREE_UNKNOWN
                ? entry->tree.digest[0] == '\0'
                : entry->tree.file_count <= SKIFF_SAVES_FILES_MAX &&
                      saves_hash_valid(entry->tree.digest));
}

static long index_of(const skiff_saves_baseline *baseline, const char *save_target) {
    for (size_t i = 0; i < baseline->count; i++) {
        if (strcmp(baseline->entries[i].save_target, save_target) == 0) {
            return (long)i;
        }
    }
    return -1;
}

const skiff_saves_baseline_entry *skiff_saves_baseline_find(const skiff_saves_baseline *baseline,
                                                            const char *save_target) {
    if (baseline == NULL || save_target == NULL) {
        return NULL;
    }
    const long i = index_of(baseline, save_target);
    return i < 0 ? NULL : &baseline->entries[i];
}

skiff_err skiff_saves_baseline_record(skiff_saves_baseline *baseline,
                                      const skiff_saves_baseline_entry *entry) {
    if (baseline == NULL || entry == NULL || !entry_valid(entry)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    long slot = index_of(baseline, entry->save_target);
    if (slot < 0) {
        if (baseline->count == SKIFF_SAVES_BASELINE_GAMES_MAX) {
            return SKIFF_ERR_BUFFER_TOO_SMALL;
        }
        slot = (long)baseline->count++;
    }
    baseline->entries[slot] = *entry;
    return SKIFF_OK;
}

int skiff_saves_baseline_forget(skiff_saves_baseline *baseline, const char *save_target) {
    if (baseline == NULL || save_target == NULL) {
        return 0;
    }
    const long i = index_of(baseline, save_target);
    if (i < 0) {
        return 0;
    }
    memmove(&baseline->entries[i], &baseline->entries[i + 1],
            (baseline->count - (size_t)i - 1) * sizeof baseline->entries[0]);
    baseline->count--;
    memset(&baseline->entries[baseline->count], 0, sizeof baseline->entries[0]);
    return 1;
}

void skiff_saves_baseline_forget_all(skiff_saves_baseline *baseline) {
    if (baseline != NULL) {
        baseline->count = 0;
        memset(baseline->entries, 0, sizeof baseline->entries);
    }
}

/* ---- The file format ---- */

static cJSON *entry_to_json(const skiff_saves_baseline_entry *entry) {
    const int unknown = entry->tree.file_count == SKIFF_SAVES_TREE_UNKNOWN;
    cJSON *item = cJSON_CreateObject();
    if (item == NULL ||
        cJSON_AddStringToObject(item, KEY_SAVE_TARGET, entry->save_target) == NULL ||
        cJSON_AddNumberToObject(item, KEY_ROM_ID, (double)entry->rom_id) == NULL ||
        cJSON_AddNumberToObject(item, KEY_SAVE_ID, (double)entry->server_save_id) == NULL ||
        cJSON_AddStringToObject(item, KEY_SERVER_HASH, entry->server_hash) == NULL ||
        cJSON_AddStringToObject(item, KEY_LOCAL_HASH, entry->local_hash) == NULL ||
        cJSON_AddNumberToObject(item, KEY_SYNCED_MS, (double)entry->synced_ms) == NULL ||
        cJSON_AddNumberToObject(item, KEY_FILES,
                                unknown ? FILES_UNKNOWN : (double)entry->tree.file_count) == NULL ||
        cJSON_AddStringToObject(item, KEY_TREE, entry->tree.digest) == NULL) {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

skiff_err skiff_saves_baseline_format(const skiff_saves_baseline *baseline, char *out,
                                      size_t out_size, size_t *length) {
    if (baseline == NULL || out == NULL || length == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *length = 0;
    cJSON *root = cJSON_CreateObject();
    cJSON *games = root != NULL ? cJSON_AddArrayToObject(root, KEY_GAMES) : NULL;
    int built = games != NULL &&
                cJSON_AddNumberToObject(root, KEY_VERSION, SKIFF_SAVES_BASELINE_VERSION) != NULL;
    for (size_t i = 0; built && i < baseline->count; i++) {
        cJSON *item = entry_to_json(&baseline->entries[i]);
        built = item != NULL && cJSON_AddItemToArray(games, item);
        if (item != NULL && !built) {
            cJSON_Delete(item);
        }
    }
    const skiff_err err =
        built ? saves_json_print(root, out, out_size, length) : SKIFF_ERR_NO_MEMORY;
    cJSON_Delete(root);
    return err;
}

/* "files": a count up to SKIFF_SAVES_FILES_MAX, or FILES_UNKNOWN. */
static int read_file_count(const cJSON *item, uint32_t *out) {
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(item, KEY_FILES);
    if (cJSON_IsNumber(files) && files->valuedouble == FILES_UNKNOWN) {
        *out = SKIFF_SAVES_TREE_UNKNOWN;
        return 1;
    }
    double count = 0.0;
    if (!saves_json_number(item, KEY_FILES, SKIFF_SAVES_FILES_MAX, &count)) {
        return 0;
    }
    *out = (uint32_t)count;
    return 1;
}

static int read_entry(const cJSON *item, skiff_saves_baseline_entry *entry) {
    double rom_id = 0.0;
    double save_id = 0.0;
    double synced_ms = 0.0;
    memset(entry, 0, sizeof *entry);
    if (!cJSON_IsObject(item) ||
        !saves_json_string(item, KEY_SAVE_TARGET, 0, entry->save_target,
                           sizeof entry->save_target) ||
        !saves_json_number(item, KEY_ROM_ID, (double)SKIFF_SAVES_ID_MAX, &rom_id) ||
        !saves_json_number(item, KEY_SAVE_ID, (double)SKIFF_SAVES_ID_MAX, &save_id) ||
        !saves_json_string(item, KEY_SERVER_HASH, 0, entry->server_hash,
                           sizeof entry->server_hash) ||
        !saves_json_string(item, KEY_LOCAL_HASH, 0, entry->local_hash, sizeof entry->local_hash) ||
        !saves_json_number(item, KEY_SYNCED_MS, SAVES_JSON_INTEGER_LIMIT - 1.0, &synced_ms) ||
        !read_file_count(item, &entry->tree.file_count) ||
        !saves_json_string(item, KEY_TREE, 1, entry->tree.digest, sizeof entry->tree.digest)) {
        return 0;
    }
    entry->rom_id = (uint64_t)rom_id;
    entry->server_save_id = (uint64_t)save_id;
    entry->synced_ms = (int64_t)synced_ms;
    return entry_valid(entry);
}

static int fill(skiff_saves_baseline *baseline, const cJSON *root) {
    double version = 0.0;
    const cJSON *games = cJSON_GetObjectItemCaseSensitive(root, KEY_GAMES);
    if (!cJSON_IsObject(root) ||
        !saves_json_number(root, KEY_VERSION, SAVES_JSON_INTEGER_LIMIT, &version) ||
        version != SKIFF_SAVES_BASELINE_VERSION || !cJSON_IsArray(games) ||
        cJSON_GetArraySize(games) > SKIFF_SAVES_BASELINE_GAMES_MAX) {
        return 0;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, games) {
        skiff_saves_baseline_entry entry;
        if (!read_entry(item, &entry) || index_of(baseline, entry.save_target) >= 0) {
            return 0;
        }
        baseline->entries[baseline->count++] = entry;
    }
    return 1;
}

skiff_err skiff_saves_baseline_parse(skiff_saves_baseline *baseline, const char *text,
                                     size_t length) {
    if (baseline == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_saves_baseline_forget_all(baseline);
    if (length > SKIFF_SAVES_BASELINE_BYTES_MAX) {
        return SKIFF_ERR_INVALID_ARG;
    }
    cJSON *root = NULL;
    skiff_err err = saves_json_parse(text, length, BASELINE_TOKENS_MAX, &root);
    if (err == SKIFF_OK && !fill(baseline, root)) {
        err = SKIFF_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);
    if (err != SKIFF_OK) {
        skiff_saves_baseline_forget_all(baseline);
    }
    return err;
}

/* ---- Loading and saving ---- */

skiff_err skiff_saves_baseline_create(skiff_saves_baseline **out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = calloc(1, sizeof **out);
    return *out != NULL ? SKIFF_OK : SKIFF_ERR_NO_MEMORY;
}

void skiff_saves_baseline_destroy(skiff_saves_baseline *baseline) { free(baseline); }

skiff_err skiff_saves_baseline_load(skiff_saves_baseline *baseline, skiff_storage *storage,
                                    const char *path, skiff_saves_baseline_load_result *result) {
    if (result != NULL) {
        *result = SKIFF_SAVES_BASELINE_NO_FILE;
    }
    if (baseline == NULL || storage == NULL || path == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_saves_baseline_forget_all(baseline);
    char *text = malloc(SKIFF_SAVES_BASELINE_BYTES_MAX);
    if (text == NULL) {
        baseline->load_error = SKIFF_ERR_NO_MEMORY;
        return SKIFF_ERR_NO_MEMORY;
    }
    size_t length = 0;
    skiff_err err =
        skiff_storage_read_whole(storage, path, text, SKIFF_SAVES_BASELINE_BYTES_MAX, &length);
    skiff_saves_baseline_load_result found = SKIFF_SAVES_BASELINE_NO_FILE;
    if (err == SKIFF_ERR_BUFFER_TOO_SMALL) {
        found = SKIFF_SAVES_BASELINE_DAMAGED;
        err = SKIFF_OK;
    } else if (err == SKIFF_OK && length > 0) {
        err = skiff_saves_baseline_parse(baseline, text, length);
        found = err == SKIFF_OK ? SKIFF_SAVES_BASELINE_LOADED : SKIFF_SAVES_BASELINE_DAMAGED;
        err = err == SKIFF_ERR_INVALID_ARG ? SKIFF_OK : err;
    }
    free(text);
    if (err != SKIFF_OK) {
        skiff_saves_baseline_forget_all(baseline);
        baseline->load_error = err;
        return err;
    }
    baseline->load_error = SKIFF_OK;
    baseline->set_aside_damaged = found == SKIFF_SAVES_BASELINE_DAMAGED;
    if (result != NULL) {
        *result = found;
    }
    return SKIFF_OK;
}

/* Moves the file the last load could not read to "<path>.damaged", replacing an older one. */
static skiff_err set_aside(skiff_storage *storage, const char *path) {
    char kept[SKIFF_STORAGE_PATH_MAX];
    const int written = snprintf(kept, sizeof kept, "%s" SKIFF_SAVES_BASELINE_DAMAGED_SUFFIX, path);
    if (written <= 0 || (size_t)written >= sizeof kept) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_err err = skiff_storage_remove(storage, kept);
    if (err == SKIFF_OK || err == SKIFF_ERR_STORAGE_NOT_FOUND) {
        err = skiff_storage_rename(storage, path, kept);
    }
    return err == SKIFF_ERR_STORAGE_NOT_FOUND ? SKIFF_OK : err;
}

skiff_err skiff_saves_baseline_save(skiff_saves_baseline *baseline, skiff_storage *storage,
                                    const char *path) {
    if (baseline == NULL || storage == NULL || path == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (baseline->load_error != SKIFF_OK) {
        return baseline->load_error;
    }
    if (baseline->set_aside_damaged) {
        const skiff_err err = set_aside(storage, path);
        if (err != SKIFF_OK) {
            return err;
        }
        baseline->set_aside_damaged = 0;
    }
    char *text = malloc(SKIFF_SAVES_BASELINE_BYTES_MAX);
    if (text == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    size_t length = 0;
    skiff_err err =
        skiff_saves_baseline_format(baseline, text, SKIFF_SAVES_BASELINE_BYTES_MAX, &length);
    if (err == SKIFF_OK) {
        err = skiff_storage_replace_whole(storage, path, text, length);
    }
    free(text);
    return err;
}

/* ---- The tree summary ---- */

typedef struct summarizer {
    skiff_storage *storage;
    const skiff_storage_roots *roots;
    const char *root;
    const char *save_target;
    /* The folder being listed; set when the tree holds what no unit may. */
    const char *folder;
    int unknown;
    size_t folder_count;
    char folders[SKIFF_SAVES_FOLDERS_MAX][SKIFF_SAVES_NAME_SIZE];
    /* Files as "<FOLDER>/<FILE>" with size and date (the unit's file records, unhashed). */
    skiff_saves_unit *unit;
} summarizer;

static skiff_storage_list_step note_tree_folder(void *ctx, const skiff_storage_entry *entry) {
    summarizer *s = ctx;
    if (!entry->info.is_dir || !saves_has_prefix(entry->name, s->save_target)) {
        return SKIFF_STORAGE_LIST_CONTINUE;
    }
    if (s->folder_count == SKIFF_SAVES_FOLDERS_MAX || strlen(entry->name) >= sizeof s->folders[0]) {
        s->unknown = 1;
        return SKIFF_STORAGE_LIST_STOP;
    }
    memcpy(s->folders[s->folder_count++], entry->name, strlen(entry->name) + 1);
    return SKIFF_STORAGE_LIST_CONTINUE;
}

static skiff_storage_list_step note_tree_file(void *ctx, const skiff_storage_entry *entry) {
    summarizer *s = ctx;
    skiff_saves_unit *unit = s->unit;
    char name[SKIFF_SAVES_ENTRY_SIZE];
    const int written = snprintf(name, sizeof name, "%s/%s", s->folder, entry->name);
    /* A file without a date could be rewritten at the same size unseen. */
    if (entry->info.is_dir || entry->info.mtime == SKIFF_STORAGE_MTIME_UNKNOWN ||
        unit->file_count == SKIFF_SAVES_FILES_MAX || written < 0 ||
        (size_t)written >= sizeof name) {
        s->unknown = 1;
        return SKIFF_STORAGE_LIST_STOP;
    }
    skiff_saves_file *file = &unit->files[unit->file_count++];
    memcpy(file->name, name, (size_t)written + 1);
    file->size = entry->info.size;
    file->mtime = entry->info.mtime;
    return SKIFF_STORAGE_LIST_CONTINUE;
}

static skiff_err summarize(summarizer *s) {
    char path[SKIFF_STORAGE_PATH_MAX];
    size_t skipped = 0;
    skiff_err err = skiff_storage_resolve(s->roots, s->root, path, sizeof path);
    if (err == SKIFF_OK) {
        err = skiff_storage_list(s->storage, path, note_tree_folder, s, &skipped);
    }
    if (err == SKIFF_ERR_STORAGE_NOT_FOUND) {
        return SKIFF_OK;
    }
    for (size_t i = 0; err == SKIFF_OK && !s->unknown && i < s->folder_count; i++) {
        s->folder = s->folders[i];
        err = saves_resolve(s->roots, s->root, s->folder, NULL, path, sizeof path);
        if (err == SKIFF_OK) {
            err = skiff_storage_list(s->storage, path, note_tree_file, s, &skipped);
        }
        s->unknown = s->unknown || skipped > 0;
    }
    return err;
}

static void digest_tree(const skiff_saves_unit *unit, skiff_saves_tree_summary *out) {
    skiff_md5 md5;
    skiff_md5_init(&md5);
    for (size_t i = 0; i < unit->file_count; i++) {
        char line[TREE_LINE_SIZE];
        const int written =
            snprintf(line, sizeof line, "%s:%llu:%lld\n", unit->files[i].name,
                     (unsigned long long)unit->files[i].size, (long long)unit->files[i].mtime);
        if (written > 0 && (size_t)written < sizeof line) {
            skiff_md5_update(&md5, line, (size_t)written);
        }
    }
    unsigned char digest[SKIFF_MD5_DIGEST_BYTES];
    skiff_md5_final(&md5, digest);
    skiff_md5_hex(digest, out->digest);
    out->file_count = (uint32_t)unit->file_count;
}

skiff_err skiff_saves_tree_summarize(skiff_storage *storage, const skiff_storage_roots *roots,
                                     const char *root, const char *save_target,
                                     skiff_saves_tree_summary *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (storage == NULL || roots == NULL || root == NULL || out == NULL ||
        !skiff_saves_target_valid(save_target)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    summarizer *s = calloc(1, sizeof *s);
    if (s == NULL || skiff_saves_unit_create(&s->unit) != SKIFF_OK) {
        free(s);
        return SKIFF_ERR_NO_MEMORY;
    }
    s->storage = storage;
    s->roots = roots;
    s->root = root;
    s->save_target = save_target;
    const skiff_err err = summarize(s);
    if (err == SKIFF_OK && s->unknown) {
        out->file_count = SKIFF_SAVES_TREE_UNKNOWN;
    } else if (err == SKIFF_OK) {
        saves_sort(s->unit);
        digest_tree(s->unit, out);
    }
    skiff_saves_unit_destroy(s->unit);
    free(s);
    return err;
}

int skiff_saves_tree_changed(const skiff_saves_tree_summary *then,
                             const skiff_saves_tree_summary *now) {
    return then == NULL || now == NULL || then->file_count == SKIFF_SAVES_TREE_UNKNOWN ||
           now->file_count == SKIFF_SAVES_TREE_UNKNOWN || then->file_count != now->file_count ||
           strcmp(then->digest, now->digest) != 0;
}
