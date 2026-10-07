#include "skiff/install.h"

#include <stdio.h>
#include <string.h>

#include "skiff/download.h"

#define PATH_SEPARATOR "/"
#define EXTENSION_MARK '.'
/* " [<rom id>]": a blank, brackets, and up to 20 digits for a 64-bit id. */
#define ROM_ID_SUFFIX_FORMAT " [%llu]"
#define ROM_ID_SUFFIX_MAX 24
/* UTF-8 continuation bytes are 10xxxxxx: a name is only cut before a byte that is not one. */
#define UTF8_CONTINUATION_MASK 0xC0U
#define UTF8_CONTINUATION 0x80U

static const char *const PSP_EXTENSIONS[] = {".iso", ".cso", ".zso", NULL};

/* One entry per platform Skiff installs (docs/development/adding-a-platform.md). */
static const skiff_installer INSTALLERS[] = {
    {"psp", PSP_EXTENSIONS, SKIFF_STORAGE_ROOT_GAMES, NULL},
};

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static int equals_ignoring_case(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (ascii_lower(*a) != ascii_lower(*b)) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

const skiff_installer *skiff_install_find_installer(const char *romm_platform_slug) {
    if (romm_platform_slug == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof INSTALLERS / sizeof INSTALLERS[0]; i++) {
        if (equals_ignoring_case(romm_platform_slug, INSTALLERS[i].romm_platform_slug)) {
            return &INSTALLERS[i];
        }
    }
    return NULL;
}

/* The name's extension, dot included ("" without one); a leading dot does not start one. */
static const char *extension_of(const char *name) {
    const char *dot = strrchr(name, EXTENSION_MARK);
    return dot != NULL && dot != name ? dot : name + strlen(name);
}

static int has_extension(const skiff_installer *installer, const char *name) {
    const char *extension = extension_of(name);
    for (const char *const *accepted = installer->extensions; *accepted != NULL; accepted++) {
        if (equals_ignoring_case(extension, *accepted)) {
            return 1;
        }
    }
    return 0;
}

skiff_install_support skiff_install_check(const skiff_installer *installer,
                                          const skiff_romm_rom_summary *rom,
                                          const skiff_romm_file *file) {
    if (installer == NULL || rom == NULL || file == NULL) {
        return SKIFF_INSTALL_UNKNOWN_EXTENSION;
    }
    if (rom->multiple_files) {
        return SKIFF_INSTALL_MULTIPLE_FILES;
    }
    if (rom->name_status != SKIFF_ROMM_NAME_OK || file->name_status != SKIFF_ROMM_NAME_OK) {
        return SKIFF_INSTALL_UNUSABLE_NAME;
    }
    return has_extension(installer, file->file_name) ? SKIFF_INSTALL_SUPPORTED
                                                     : SKIFF_INSTALL_UNKNOWN_EXTENSION;
}

skiff_install_state skiff_install_state_of(const skiff_install_manifest *manifest,
                                           const skiff_romm_rom_summary *rom) {
    if (manifest == NULL || rom == NULL) {
        return SKIFF_INSTALL_NOT_INSTALLED;
    }
    const skiff_install_record *record = skiff_install_manifest_find_rom(manifest, rom->id);
    if (record == NULL) {
        return SKIFF_INSTALL_NOT_INSTALLED;
    }
    const int crc_differs = record->has_crc32 && rom->has_crc32 && record->crc32 != rom->crc32;
    return record->size != rom->size || crc_differs ? SKIFF_INSTALL_CHANGED
                                                    : SKIFF_INSTALL_INSTALLED;
}

/* ---- Planning a download ---- */

/* What a candidate path is to this ROM file. */
typedef enum candidate_use {
    CANDIDATE_FREE,
    /* Skiff's recorded copy of this ROM file is there. */
    CANDIDATE_OWN,
    CANDIDATE_TAKEN,
} candidate_use;

typedef struct planner {
    const skiff_romm_rom_summary *rom;
    const skiff_romm_file *file;
    const skiff_storage_roots *roots;
    const skiff_install_manifest *manifest;
    skiff_storage *storage;
    skiff_install_taken_fn taken;
    void *taken_ctx;
} planner;

/* A download's .part or .resume file at path: the download engine trusts or deletes those, so a
 * name whose ones exist is not free, whoever left them. */
static skiff_err partial_files_present(const planner *p, const char *path, int *present) {
    static const char *const SUFFIXES[] = {SKIFF_DOWNLOAD_PART_SUFFIX, SKIFF_DOWNLOAD_STATE_SUFFIX};
    *present = 0;
    for (size_t i = 0; i < sizeof SUFFIXES / sizeof SUFFIXES[0]; i++) {
        char partial[SKIFF_DOWNLOAD_PATH_MAX];
        const int written = snprintf(partial, sizeof partial, "%s%s", path, SUFFIXES[i]);
        if (written < 0 || (size_t)written >= sizeof partial) {
            return SKIFF_ERR_INVALID_ARG;
        }
        uint64_t size = 0;
        const skiff_err err = skiff_storage_size(p->storage, partial, &size);
        if (err == SKIFF_OK) {
            *present = 1;
            return SKIFF_OK;
        }
        if (err != SKIFF_ERR_STORAGE_NOT_FOUND) {
            return err;
        }
    }
    return SKIFF_OK;
}

static int is_this_file(const planner *p, const skiff_install_record *record) {
    return record->rom_id == p->rom->id && strcmp(record->file_name, p->file->file_name) == 0;
}

/* Whether logical can take this ROM file, with path set to it resolved. */
static skiff_err judge(const planner *p, const char *logical, char *path, size_t path_size,
                       candidate_use *use) {
    const skiff_err err = skiff_storage_resolve(p->roots, logical, path, path_size);
    if (err != SKIFF_OK) {
        return err;
    }
    const skiff_install_record *record = skiff_install_manifest_find_path(p->manifest, logical);
    const int ours = record != NULL && is_this_file(p, record);
    if ((record != NULL && !ours) || (p->taken != NULL && p->taken(p->taken_ctx, logical))) {
        *use = CANDIDATE_TAKEN;
        return SKIFF_OK;
    }
    int partial = 0;
    const skiff_err partial_err = partial_files_present(p, path, &partial);
    if (partial_err != SKIFF_OK) {
        return partial_err;
    }
    if (partial) {
        *use = CANDIDATE_TAKEN;
        return SKIFF_OK;
    }
    uint64_t size = 0;
    const skiff_err size_err = skiff_storage_size(p->storage, path, &size);
    if (size_err == SKIFF_ERR_STORAGE_NOT_FOUND) {
        *use = CANDIDATE_FREE;
        return SKIFF_OK;
    }
    if (size_err != SKIFF_OK) {
        return size_err;
    }
    /* A file is there: Skiff's own copy of this ROM file, or one it must not touch. A copy of
     * another size was replaced outside Skiff since it was recorded, so it is no longer Skiff's.
     * (The CRC-32 is not checked: that would mean reading a whole game.) */
    *use = ours && size == record->size ? CANDIDATE_OWN : CANDIDATE_TAKEN;
    return SKIFF_OK;
}

static int join(const char *dir, const char *name, char *out, size_t out_size) {
    const int written = snprintf(out, out_size, "%s" PATH_SEPARATOR "%s", dir, name);
    return written > 0 && (size_t)written < out_size;
}

/*
 * name with " [<rom id>]" before its extension, the part before it shortened at a character
 * boundary when the whole would not fit a safe name.
 */
static int with_rom_id(const char *name, uint64_t rom_id, char *out, size_t out_size) {
    char suffix[ROM_ID_SUFFIX_MAX];
    snprintf(suffix, sizeof suffix, ROM_ID_SUFFIX_FORMAT, (unsigned long long)rom_id);
    const char *extension = extension_of(name);
    const size_t extension_length = strlen(extension);
    const size_t suffix_length = strlen(suffix);
    size_t stem_length = (size_t)(extension - name);
    if (suffix_length + extension_length >= out_size) {
        return 0;
    }
    const size_t stem_room = out_size - 1 - suffix_length - extension_length;
    if (stem_length > stem_room) {
        stem_length = stem_room;
        while (stem_length > 0 &&
               ((unsigned char)name[stem_length] & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION) {
            stem_length--;
        }
    }
    if (stem_length == 0) {
        return 0;
    }
    snprintf(out, out_size, "%.*s%s%s", (int)stem_length, name, suffix, extension);
    return 1;
}

/* Fills out from logical if it is free or Skiff's own copy; *placed says whether it was. */
static skiff_err try_candidate(const planner *p, const char *logical, int renamed,
                               skiff_install_plan *out, int *placed) {
    char path[SKIFF_STORAGE_PATH_MAX];
    candidate_use use = CANDIDATE_TAKEN;
    const skiff_err err = judge(p, logical, path, sizeof path, &use);
    *placed = err == SKIFF_OK && use != CANDIDATE_TAKEN;
    if (*placed) {
        snprintf(out->logical_path, sizeof out->logical_path, "%s", logical);
        snprintf(out->path, sizeof out->path, "%s", path);
        out->replaces_own = use == CANDIDATE_OWN;
        if (out->replaces_own) {
            const skiff_install_record *record =
                skiff_install_manifest_find_path(p->manifest, logical);
            out->own_size = record != NULL ? record->size : 0;
        }
        out->renamed = renamed;
    }
    return err;
}

/* path is a file directly in folder ("games:/Game.iso" in "games:"): a record pointing anywhere
 * else (config.ini, a save) is never trusted as a place to download to. */
static int in_folder(const char *folder, const char *path) {
    const size_t length = strlen(folder);
    return strncmp(path, folder, length) == 0 && path[length] == '/' && path[length + 1] != '\0' &&
           strchr(path + length + 1, '/') == NULL;
}

static skiff_err plan(const skiff_installer *installer, const planner *p, skiff_install_plan *out) {
    int placed = 0;
    /* Skiff's own recorded copy keeps its place, whatever RomM's name cleans to today. */
    const skiff_install_record *own =
        skiff_install_manifest_find(p->manifest, p->rom->id, p->file->file_name);
    if (own != NULL && in_folder(installer->target_dir, own->path)) {
        const skiff_err err = try_candidate(p, own->path, 0, out, &placed);
        if (err != SKIFF_OK && err != SKIFF_ERR_INVALID_ARG && err != SKIFF_ERR_BUFFER_TOO_SMALL) {
            return err;
        }
        if (placed) {
            return SKIFF_OK;
        }
    }
    char name[SKIFF_STORAGE_NAME_MAX];
    char logical[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = skiff_storage_safe_name(p->file->file_name, name, sizeof name);
    if (err != SKIFF_OK) {
        return err;
    }
    if (!join(installer->target_dir, name, logical, sizeof logical)) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    err = try_candidate(p, logical, 0, out, &placed);
    if (err != SKIFF_OK || placed) {
        return err;
    }
    char renamed[SKIFF_STORAGE_NAME_MAX];
    if (!with_rom_id(name, p->rom->id, renamed, sizeof renamed) ||
        !join(installer->target_dir, renamed, logical, sizeof logical)) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    err = try_candidate(p, logical, 1, out, &placed);
    if (err != SKIFF_OK) {
        return err;
    }
    return placed ? SKIFF_OK : SKIFF_ERR_STORAGE_NAME_TAKEN;
}

skiff_err skiff_install_plan_download(const skiff_installer *installer,
                                      const skiff_romm_rom_summary *rom,
                                      const skiff_romm_file *file, const skiff_storage_roots *roots,
                                      const skiff_install_manifest *manifest,
                                      skiff_storage *storage, skiff_install_taken_fn taken,
                                      void *taken_ctx, skiff_install_plan *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (installer == NULL || rom == NULL || file == NULL || roots == NULL || manifest == NULL ||
        storage == NULL || out == NULL ||
        skiff_install_check(installer, rom, file) != SKIFF_INSTALL_SUPPORTED) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (rom->id > SKIFF_INSTALL_ROM_ID_MAX) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const planner p = {rom, file, roots, manifest, storage, taken, taken_ctx};
    skiff_err err = plan(installer, &p, out);
    /* A download the manifest cannot record would leave a file Skiff no longer knows it owns. */
    if (err == SKIFF_OK && manifest->count == SKIFF_INSTALL_RECORDS_MAX &&
        skiff_install_manifest_find(manifest, rom->id, file->file_name) == NULL &&
        skiff_install_manifest_find_path(manifest, out->logical_path) == NULL) {
        err = SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    if (err != SKIFF_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}
