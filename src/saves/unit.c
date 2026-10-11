/*
 * Save units, the parts both directions share: names, the unit's sort order and content_hash, and
 * collecting a game's folders from the Memory Stick.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/param_sfo.h"

#include "saves_internal.h"

#define PRINTABLE_FIRST 0x20
#define PRINTABLE_LAST 0x7E
/* FAT refuses these in a name; '/' also separates the folder from the file in an entry. */
#define FAT_FORBIDDEN "\\/:*?\"<>|"
#define NAME_BLANK ' '
#define NAME_DOT '.'

static const char *const PROBLEM_NAMES[] = {
    "none",           "nested-folder", "name",   "duplicate",  "too-many-folders",
    "too-many-files", "too-large",     "path",   "other-game", "encrypted",
    "zip64",          "multi-disk",    "method", "size",       "crc",
    "structure"};

_Static_assert(sizeof PROBLEM_NAMES / sizeof PROBLEM_NAMES[0] == SKIFF_SAVES_PROBLEM_STRUCTURE + 1,
               "every skiff_saves_problem needs a name for logs");

const char *skiff_saves_problem_name(skiff_saves_problem problem) {
    return (int)problem >= 0 && problem <= SKIFF_SAVES_PROBLEM_STRUCTURE ? PROBLEM_NAMES[problem]
                                                                         : "unknown";
}

skiff_err skiff_saves_unit_create(skiff_saves_unit **out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = calloc(1, sizeof **out);
    return *out != NULL ? SKIFF_OK : SKIFF_ERR_NO_MEMORY;
}

void skiff_saves_unit_destroy(skiff_saves_unit *unit) { free(unit); }

int skiff_saves_target_valid(const char *save_target) {
    if (save_target == NULL) {
        return 0;
    }
    size_t length = 0;
    for (; save_target[length] != '\0'; length++) {
        const char c = save_target[length];
        if (length >= SKIFF_SAVES_TARGET_MAX ||
            !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return 0;
        }
    }
    return length >= SKIFF_SAVES_TARGET_MIN;
}

int saves_name_valid(const char *name, size_t length) {
    if (length == 0 || length > SKIFF_SAVES_NAME_MAX || name[0] == NAME_BLANK ||
        name[length - 1] == NAME_BLANK || name[length - 1] == NAME_DOT) {
        return 0;
    }
    for (size_t i = 0; i < length; i++) {
        const unsigned char c = (unsigned char)name[i];
        if (c < PRINTABLE_FIRST || c > PRINTABLE_LAST || strchr(FAT_FORBIDDEN, c) != NULL) {
            return 0;
        }
    }
    return !skiff_storage_is_dos_device(name, length);
}

/* strlen(), stopping at max: strnlen() is POSIX, not C11. */
static size_t bounded_length(const char *text, size_t max) {
    size_t length = 0;
    while (length < max && text[length] != '\0') {
        length++;
    }
    return length;
}

int skiff_saves_name_valid(const char *name) {
    return name != NULL && saves_name_valid(name, bounded_length(name, SKIFF_SAVES_NAME_MAX + 1));
}

int saves_same_name(const char *a, size_t a_length, const char *b, size_t b_length) {
    if (a_length != b_length) {
        return 0;
    }
    for (size_t i = 0; i < a_length; i++) {
        if (saves_fold((unsigned char)a[i]) != saves_fold((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

int saves_has_prefix(const char *name, const char *save_target) {
    const size_t length = strlen(save_target);
    return bounded_length(name, length) == length &&
           saves_same_name(name, length, save_target, length);
}

skiff_err saves_resolve(const skiff_storage_roots *roots, const char *root, const char *a,
                        const char *b, char *out, size_t out_size) {
    char logical[SKIFF_STORAGE_PATH_MAX];
    const int written = b != NULL ? snprintf(logical, sizeof logical, "%s/%s/%s", root, a, b)
                                  : snprintf(logical, sizeof logical, "%s/%s", root, a);
    if (written < 0 || (size_t)written >= sizeof logical) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    return skiff_storage_resolve(roots, logical, out, out_size);
}

void saves_refuse(skiff_saves_unit *unit, skiff_saves_problem problem, const char *name,
                  size_t length) {
    unit->problem = problem;
    const size_t kept = length < sizeof unit->problem_name ? length : sizeof unit->problem_name - 1;
    memcpy(unit->problem_name, name, kept);
    unit->problem_name[kept] = '\0';
}

static int compare_folders(const void *a, const void *b) { return strcmp(a, b); }

static int compare_files(const void *a, const void *b) {
    return strcmp(((const skiff_saves_file *)a)->name, ((const skiff_saves_file *)b)->name);
}

void saves_sort(skiff_saves_unit *unit) {
    qsort(unit->folders, unit->folder_count, sizeof unit->folders[0], compare_folders);
    qsort(unit->files, unit->file_count, sizeof unit->files[0], compare_files);
}

void saves_content_hash(skiff_saves_unit *unit) {
    skiff_md5 md5;
    skiff_md5_init(&md5);
    for (size_t i = 0; i < unit->file_count; i++) {
        if (i > 0) {
            skiff_md5_update(&md5, SAVES_LINE_SEPARATOR, strlen(SAVES_LINE_SEPARATOR));
        }
        skiff_md5_update(&md5, unit->files[i].name, strlen(unit->files[i].name));
        skiff_md5_update(&md5, SAVES_HASH_SEPARATOR, strlen(SAVES_HASH_SEPARATOR));
        skiff_md5_update(&md5, unit->files[i].md5, strlen(unit->files[i].md5));
    }
    unsigned char digest[SKIFF_MD5_DIGEST_BYTES];
    skiff_md5_final(&md5, digest);
    skiff_md5_hex(digest, unit->content_hash);
}

/* ---- Collecting a game's folders ---- */

typedef struct collector {
    skiff_storage *storage;
    const skiff_storage_roots *roots;
    skiff_saves_unit *unit;
    /* The folder being listed, and where its files start in unit->files. */
    const char *folder;
    size_t first_file;
    char path[SKIFF_STORAGE_PATH_MAX];
    unsigned char sfo[SKIFF_SFO_BYTES_MAX + 1];
} collector;

static void empty_unit(skiff_saves_unit *unit, const char *save_target, const char *root) {
    memset(unit, 0, sizeof *unit);
    snprintf(unit->save_target, sizeof unit->save_target, "%s", save_target);
    snprintf(unit->root, sizeof unit->root, "%s", root);
}

/* The listing of root: every folder whose name starts with the save_target. */
static skiff_storage_list_step note_folder(void *ctx, const skiff_storage_entry *entry) {
    collector *c = ctx;
    skiff_saves_unit *unit = c->unit;
    if (!entry->info.is_dir || !saves_has_prefix(entry->name, unit->save_target)) {
        return SKIFF_STORAGE_LIST_CONTINUE;
    }
    const size_t length = strlen(entry->name);
    if (!saves_name_valid(entry->name, length)) {
        saves_refuse(unit, SKIFF_SAVES_PROBLEM_NAME, entry->name, length);
        return SKIFF_STORAGE_LIST_STOP;
    }
    for (size_t i = 0; i < unit->folder_count; i++) {
        if (saves_same_name(unit->folders[i], strlen(unit->folders[i]), entry->name, length)) {
            saves_refuse(unit, SKIFF_SAVES_PROBLEM_DUPLICATE, entry->name, length);
            return SKIFF_STORAGE_LIST_STOP;
        }
    }
    if (unit->folder_count == SKIFF_SAVES_FOLDERS_MAX) {
        saves_refuse(unit, SKIFF_SAVES_PROBLEM_TOO_MANY_FOLDERS, entry->name, length);
        return SKIFF_STORAGE_LIST_STOP;
    }
    memcpy(unit->folders[unit->folder_count++], entry->name, length + 1);
    return SKIFF_STORAGE_LIST_CONTINUE;
}

/* The listing of one folder: its files. */
static skiff_storage_list_step note_file(void *ctx, const skiff_storage_entry *entry) {
    collector *c = ctx;
    skiff_saves_unit *unit = c->unit;
    const size_t length = strlen(entry->name);
    char entry_name[SKIFF_SAVES_ENTRY_SIZE];
    snprintf(entry_name, sizeof entry_name, "%s/%.*s", c->folder, SKIFF_SAVES_NAME_MAX,
             entry->name);
    if (entry->info.is_dir) {
        saves_refuse(unit, SKIFF_SAVES_PROBLEM_NESTED_FOLDER, entry_name, strlen(entry_name));
        return SKIFF_STORAGE_LIST_STOP;
    }
    if (!saves_name_valid(entry->name, length)) {
        saves_refuse(unit, SKIFF_SAVES_PROBLEM_NAME, entry_name, strlen(entry_name));
        return SKIFF_STORAGE_LIST_STOP;
    }
    for (size_t i = c->first_file; i < unit->file_count; i++) {
        const char *other = unit->files[i].name + strlen(c->folder) + 1;
        if (saves_same_name(other, strlen(other), entry->name, length)) {
            saves_refuse(unit, SKIFF_SAVES_PROBLEM_DUPLICATE, entry_name, strlen(entry_name));
            return SKIFF_STORAGE_LIST_STOP;
        }
    }
    if (unit->file_count == SKIFF_SAVES_FILES_MAX) {
        saves_refuse(unit, SKIFF_SAVES_PROBLEM_TOO_MANY_FILES, entry_name, strlen(entry_name));
        return SKIFF_STORAGE_LIST_STOP;
    }
    skiff_saves_file *file = &unit->files[unit->file_count++];
    memcpy(file->name, entry_name, strlen(entry_name) + 1);
    file->size = entry->info.size;
    file->mtime = entry->info.mtime;
    unit->total_bytes += entry->info.size;
    return SKIFF_STORAGE_LIST_CONTINUE;
}

/* Whether the folder's PARAM.SFO marks it as game data a game installed: it parses and carries
 * neither save key. No PARAM.SFO, or one too large or damaged, leaves the folder with the saves. */
static skiff_err is_install(collector *c, const char *folder, int *install) {
    *install = 0;
    skiff_err err = saves_resolve(c->roots, c->unit->root, folder, SKIFF_SFO_FILE_NAME, c->path,
                                  sizeof c->path);
    skiff_file *file = NULL;
    if (err == SKIFF_OK) {
        err = skiff_storage_open(c->storage, c->path, SKIFF_FILE_READ, 0, &file);
    }
    if (err == SKIFF_ERR_STORAGE_NOT_FOUND) {
        return SKIFF_OK;
    }
    size_t used = 0;
    size_t got = 1;
    while (err == SKIFF_OK && got > 0 && used < sizeof c->sfo) {
        err = skiff_file_read(file, c->sfo + used, sizeof c->sfo - used, &got);
        used += err == SKIFF_OK ? got : 0;
    }
    const skiff_err close_err = skiff_file_close(file);
    if (err == SKIFF_OK) {
        err = close_err;
    }
    skiff_sfo sfo;
    if (err == SKIFF_OK && skiff_sfo_parse(c->sfo, used, &sfo) == SKIFF_OK) {
        *install = !skiff_sfo_is_save(&sfo);
    }
    return err;
}

/* Lists folder's files into the unit, unless it holds a game's installed data, which is dropped
 * from the folder list (*kept 0). */
static skiff_err collect_folder(collector *c, const char *folder, int *kept) {
    int install = 0;
    skiff_err err = is_install(c, folder, &install);
    *kept = err == SKIFF_OK && !install;
    if (!*kept) {
        return err;
    }
    err = saves_resolve(c->roots, c->unit->root, folder, NULL, c->path, sizeof c->path);
    if (err != SKIFF_OK) {
        return err;
    }
    c->folder = folder;
    c->first_file = c->unit->file_count;
    size_t skipped = 0;
    err = skiff_storage_list(c->storage, c->path, note_file, c, &skipped);
    if (err == SKIFF_OK && c->unit->problem == SKIFF_SAVES_PROBLEM_NONE && skipped > 0) {
        saves_refuse(c->unit, SKIFF_SAVES_PROBLEM_NAME, folder, strlen(folder));
    }
    return err;
}

static skiff_err collect(collector *c) {
    skiff_saves_unit *unit = c->unit;
    char root[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = skiff_storage_resolve(c->roots, unit->root, root, sizeof root);
    size_t skipped = 0;
    if (err == SKIFF_OK) {
        /* A name too long to list cannot be a valid save folder's, so skipped ones are ignored. */
        err = skiff_storage_list(c->storage, root, note_folder, c, &skipped);
    }
    if (err == SKIFF_ERR_STORAGE_NOT_FOUND) {
        return SKIFF_OK;
    }
    size_t kept_folders = 0;
    for (size_t i = 0;
         err == SKIFF_OK && unit->problem == SKIFF_SAVES_PROBLEM_NONE && i < unit->folder_count;
         i++) {
        int kept = 0;
        err = collect_folder(c, unit->folders[i], &kept);
        if (kept && kept_folders != i) {
            memcpy(unit->folders[kept_folders], unit->folders[i], sizeof unit->folders[i]);
        }
        kept_folders += kept ? 1 : 0;
    }
    if (err != SKIFF_OK || unit->problem != SKIFF_SAVES_PROBLEM_NONE) {
        return err;
    }
    unit->folder_count = kept_folders;
    if (unit->total_bytes > SKIFF_SAVES_UNIT_BYTES_MAX) {
        saves_refuse(unit, SKIFF_SAVES_PROBLEM_TOO_LARGE, "", 0);
        return SKIFF_OK;
    }
    saves_sort(unit);
    return SKIFF_OK;
}

skiff_err skiff_saves_collect(skiff_storage *storage, const skiff_storage_roots *roots,
                              const char *root, const char *save_target, skiff_saves_unit *unit) {
    if (storage == NULL || roots == NULL || root == NULL || unit == NULL ||
        !skiff_saves_target_valid(save_target) || strlen(root) >= sizeof unit->root) {
        return SKIFF_ERR_INVALID_ARG;
    }
    empty_unit(unit, save_target, root);
    collector *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    c->storage = storage;
    c->roots = roots;
    c->unit = unit;
    skiff_err err = collect(c);
    free(c);
    if (err == SKIFF_OK && unit->problem != SKIFF_SAVES_PROBLEM_NONE) {
        err = SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE;
    }
    if (err != SKIFF_OK) {
        const skiff_saves_problem problem = unit->problem;
        char name[SKIFF_SAVES_ENTRY_SIZE];
        memcpy(name, unit->problem_name, sizeof name);
        empty_unit(unit, save_target, root);
        unit->problem = problem;
        memcpy(unit->problem_name, name, sizeof name);
    }
    return err;
}
