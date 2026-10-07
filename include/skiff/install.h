#ifndef SKIFF_INSTALL_H
#define SKIFF_INSTALL_H

/*
 * Where a game from RomM goes, and what Skiff installed. An installer maps a RomM platform to the
 * extensions it accepts and the folder they go to (one entry for now: PSP games to games:, which
 * is <device>/ISO). Planning a download turns a RomM file into a target path that never replaces a
 * file Skiff did not install, and the manifest (app:/installed.json) records every file Skiff
 * installed, so the library can show what is on the Memory Stick.
 *
 * Overwrite policy: a download may replace only Skiff's own earlier copy of the same ROM file. When
 * the target's name is taken by anything else (a game copied by hand, or another ROM whose name
 * cleans to the same safe name, e.g. "a/b.iso" and "a:b.iso", or differs only in case, which FAT
 * ignores), the RomM id is added to the name ("Game [1234].iso"); when that name is taken too, the
 * download is refused with SKIFF_ERR_STORAGE_NAME_TAKEN rather than overwrite anything.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"
#include "skiff/romm.h"
#include "skiff/storage.h"
#include "skiff/storage_paths.h"

/* ---- Installers ---- */

typedef struct skiff_installer {
    /* RomM's slug for the platform, e.g. "psp". */
    const char *romm_platform_slug;
    /* Accepted extensions, dot included, NULL-terminated; compared ignoring case. */
    const char *const *extensions;
    /* A logical root path (skiff/storage_paths.h), e.g. "games:" (<device>/ISO). */
    const char *target_dir;
    /* Run after a file is in place; NULL for none. */
    skiff_err (*post_install)(const char *installed_path);
} skiff_installer;

/* The installer for a RomM platform slug, ignoring case; NULL when Skiff has none. */
const skiff_installer *skiff_install_find_installer(const char *romm_platform_slug);

/* Whether a ROM file can be installed, and why not: a reason the UI can show. */
typedef enum skiff_install_support {
    SKIFF_INSTALL_SUPPORTED,
    /* A folder of several files (a PSP game is one file). */
    SKIFF_INSTALL_MULTIPLE_FILES,
    /* RomM names the ROM or the file in a way Skiff cannot use (skiff_romm_name_status). */
    SKIFF_INSTALL_UNUSABLE_NAME,
    /* Not one of the installer's extensions (e.g. a .zip or a .pbp for PSP). */
    SKIFF_INSTALL_UNKNOWN_EXTENSION,
} skiff_install_support;

/* Checks rom's file against installer. A NULL argument counts as unsupported (unknown
 * extension). */
skiff_install_support skiff_install_check(const skiff_installer *installer,
                                          const skiff_romm_rom_summary *rom,
                                          const skiff_romm_file *file);

/* ---- The manifest: what Skiff installed ---- */

#define SKIFF_INSTALL_MANIFEST_NAME "installed.json"
#define SKIFF_INSTALL_MANIFEST_VERSION 1
/* Files the manifest records. A PSP-1000 Memory Stick holds a few dozen games; 512 leaves room for
 * the largest cards and small CSO files, and keeps the manifest in memory under 300 KB. */
#define SKIFF_INSTALL_RECORDS_MAX 512
/* The largest installed.json read or written. A record is about 200 bytes of JSON with typical
 * names and at most about 1.2 KB with the longest ones (every '"' and '\\' written twice), so 512
 * records always fit 640 KB: whatever the manifest can record, it can save. Read and save hold it
 * in memory only while they run. A larger file is treated as damaged. */
#define SKIFF_INSTALL_MANIFEST_BYTES_MAX ((size_t)640 * 1024)
/* RomM ids are written as JSON numbers, which cJSON prints with 15 significant digits: an id must
 * stay below 10^15 to come back unchanged. */
#define SKIFF_INSTALL_ROM_ID_MAX 999999999999999ULL

typedef struct skiff_install_record {
    uint64_t rom_id;
    /* The file's name in RomM (skiff_romm_file.file_name). */
    char file_name[SKIFF_ROMM_FILE_NAME_MAX];
    /* Where it was installed, as a logical path ("games:/Game.iso"), so it follows the device. */
    char path[SKIFF_STORAGE_PATH_MAX];
    uint64_t size;
    int has_crc32;
    uint32_t crc32;
    /* When it was installed, ms since 1970 (UTC); 0 when unknown. */
    int64_t installed_ms;
} skiff_install_record;

typedef struct skiff_install_manifest {
    size_t count;
    skiff_install_record records[SKIFF_INSTALL_RECORDS_MAX];
    /* SKIFF_OK, or the storage error the last load hit: the records on the Memory Stick are then
     * unknown, so skiff_install_manifest_save() refuses with it instead of replacing them with an
     * empty list. A later successful load clears it. */
    skiff_err load_error;
    /* The last load found a file it could not read: the next save first moves that file to
     * "<path>" SKIFF_INSTALL_DAMAGED_SUFFIX, so its records stay on the Memory Stick. */
    int set_aside_damaged;
} skiff_install_manifest;

/* Allocates an empty manifest (about 290 KB, too large for a PSP thread's stack). Free it with
 * skiff_install_manifest_destroy(). SKIFF_ERR_INVALID_ARG for NULL, SKIFF_ERR_NO_MEMORY. */
skiff_err skiff_install_manifest_create(skiff_install_manifest **out);

/* Frees the manifest. Does nothing for NULL. */
void skiff_install_manifest_destroy(skiff_install_manifest *manifest);

/* A manifest Skiff could not read is kept under this suffix rather than overwritten (replacing an
 * older one kept there), so a misread never loses the records. */
#define SKIFF_INSTALL_DAMAGED_SUFFIX ".damaged"

/* What skiff_install_manifest_load() found. */
typedef enum skiff_install_load_result {
    /* The file was read; its records are loaded. */
    SKIFF_INSTALL_LOADED,
    /* No file yet: nothing installed. */
    SKIFF_INSTALL_NO_MANIFEST,
    /* The file is not one Skiff can read (cut, edited by hand, too large, written by a newer
     * Skiff): the manifest starts empty, the next save moves the file aside (see
     * SKIFF_INSTALL_DAMAGED_SUFFIX) before writing a new one, and the caller logs a warning. Games
     * it listed then count as copied by hand, so their names are never overwritten. */
    SKIFF_INSTALL_DAMAGED,
} skiff_install_load_result;

/*
 * Replaces manifest's records with those in the file at path (crash-safe read,
 * skiff_storage_read_whole()). A missing or damaged file gives an empty manifest and SKIFF_OK, with
 * *result saying which (result may be NULL). A storage error, or SKIFF_ERR_NO_MEMORY when the
 * memory to parse the file is not there (it is checked first, since cJSON cannot tell a failed
 * allocation from a damaged file), leaves the manifest empty, returns the error and keeps it in
 * load_error (see there). SKIFF_ERR_INVALID_ARG for a NULL manifest,
 * storage or path.
 */
skiff_err skiff_install_manifest_load(skiff_install_manifest *manifest, skiff_storage *storage,
                                      const char *path, skiff_install_load_result *result);

/*
 * Writes the manifest to path, replacing the file whole (crash-safe: a failure leaves the old
 * file). After a load that found the file damaged, that file is first moved to
 * "<path>" SKIFF_INSTALL_DAMAGED_SUFFIX; if that fails, nothing is written. The manifest's
 * load_error when the last load failed; SKIFF_ERR_INVALID_ARG for a NULL argument or a path too
 * long for the suffix; otherwise the storage's error.
 */
skiff_err skiff_install_manifest_save(skiff_install_manifest *manifest, skiff_storage *storage,
                                      const char *path);

/* The record for rom_id's file file_name; NULL when there is none. */
const skiff_install_record *skiff_install_manifest_find(const skiff_install_manifest *manifest,
                                                        uint64_t rom_id, const char *file_name);

/* The first record of rom_id (a PSP game has one file); NULL when there is none. */
const skiff_install_record *skiff_install_manifest_find_rom(const skiff_install_manifest *manifest,
                                                            uint64_t rom_id);

/* The record installed at the logical path, comparing names ignoring ASCII case as FAT does; NULL
 * when there is none. */
const skiff_install_record *skiff_install_manifest_find_path(const skiff_install_manifest *manifest,
                                                             const char *path);

/*
 * Records a finished install, replacing the record of the same ROM file and any record of another
 * file at the same path (that file was just replaced). The record is copied; its path must be a
 * logical path. SKIFF_ERR_INVALID_ARG for a NULL argument, an empty file name or path, a rom_id
 * over SKIFF_INSTALL_ROM_ID_MAX or a size over SKIFF_STORAGE_MAX_FILE_BYTES;
 * SKIFF_ERR_BUFFER_TOO_SMALL when SKIFF_INSTALL_RECORDS_MAX are already recorded. Save afterwards.
 */
skiff_err skiff_install_manifest_record(skiff_install_manifest *manifest,
                                        const skiff_install_record *record);

/* Drops the record of rom_id's file file_name; 1 if there was one. Save afterwards. */
int skiff_install_manifest_forget(skiff_install_manifest *manifest, uint64_t rom_id,
                                  const char *file_name);

/*
 * Drops the records whose file is no longer there (deleted on a computer, or by the XMB) or no
 * longer the size Skiff recorded (replaced outside Skiff, so no longer Skiff's to replace), so the
 * library shows them as not installed; *forgotten (may be NULL) counts them. A record whose path
 * does not resolve is dropped too. Any storage error but SKIFF_ERR_STORAGE_NOT_FOUND stops and is
 * returned, keeping the records not yet checked. Save afterwards when *forgotten > 0.
 */
skiff_err skiff_install_manifest_reconcile(skiff_install_manifest *manifest, skiff_storage *storage,
                                           const skiff_storage_roots *roots, size_t *forgotten);

/* ---- The file format, exposed for the tests and the self-test ---- */

/*
 * installed.json: {"version":1,"installed":[{"rom_id":12,"file_name":"Game.iso",
 * "path":"games:/Game.iso","size":1048576,"crc32":"0a1b2c3d","installed_ms":1790000000000}]},
 * "crc32" absent when unknown. Writes it into out, terminated, with its length in *length.
 * SKIFF_ERR_BUFFER_TOO_SMALL when it does not fit out_size with its terminator,
 * SKIFF_ERR_NO_MEMORY, SKIFF_ERR_INVALID_ARG for a NULL argument.
 */
skiff_err skiff_install_manifest_format(const skiff_install_manifest *manifest, char *out,
                                        size_t out_size, size_t *length);

/*
 * Reads what skiff_install_manifest_format() writes into manifest; unknown fields are ignored.
 * Anything that is not a manifest Skiff wrote is refused with SKIFF_ERR_INVALID_ARG, as a damaged
 * .resume file is (skiff/download.h): a NULL argument, text over SKIFF_INSTALL_MANIFEST_BYTES_MAX,
 * bad JSON, a missing or mistyped field, a value that does not fit, a version other than
 * SKIFF_INSTALL_MANIFEST_VERSION, more than SKIFF_INSTALL_RECORDS_MAX records, or two records of
 * the same ROM file or the same path. manifest is empty after any error.
 */
skiff_err skiff_install_manifest_parse(skiff_install_manifest *manifest, const char *text,
                                       size_t length);

/* ---- Library status ---- */

typedef enum skiff_install_state {
    SKIFF_INSTALL_NOT_INSTALLED,
    SKIFF_INSTALL_INSTALLED,
    /* Recorded, but RomM now lists another size or CRC-32: the file changed on the server. */
    SKIFF_INSTALL_CHANGED,
} skiff_install_state;

/* What the manifest says about rom (by its id, size and CRC-32); NOT_INSTALLED for NULL. Run
 * skiff_install_manifest_reconcile() at startup so files deleted meanwhile count as not installed.
 */
skiff_install_state skiff_install_state_of(const skiff_install_manifest *manifest,
                                           const skiff_romm_rom_summary *rom);

/* ---- Planning a download ---- */

/* Asked whether a logical path is already promised to something not yet on the Memory Stick, such
 * as a download waiting in the queue; nonzero for taken. */
typedef int (*skiff_install_taken_fn)(void *ctx, const char *logical_path);

typedef struct skiff_install_plan {
    /* Where the file goes: "games:/Game.iso" (for the manifest), and resolved (for the download).
     */
    char logical_path[SKIFF_STORAGE_PATH_MAX];
    char path[SKIFF_STORAGE_PATH_MAX];
    /* Skiff's own earlier copy of this ROM file is there and will be replaced: pass it on as the
     * job's replace_target (skiff/jobs.h). Otherwise the download never removes a file it finds at
     * path when it finishes, even one copied there after this plan was made. */
    int replaces_own;
    /* The size Skiff recorded for that copy: the download replaces only a file of exactly this
     * size (pass it on as the job's replace_size). */
    uint64_t own_size;
    /* The RomM id was added to the name because the plain name is taken. */
    int renamed;
} skiff_install_plan;

/*
 * Picks where rom's file is downloaded to under installer, applying the overwrite policy above:
 * Skiff's own recorded copy keeps its path (replaces_own; a file there of another size than
 * recorded was replaced outside Skiff and counts as taken); otherwise the safe name
 * (skiff_storage_safe_name()) in installer's folder, unless a file is there that Skiff did not
 * install for this ROM file, another record holds that path (ignoring case), or taken (may be
 * NULL) says it is promised; then the name with " [<rom id>]" before its extension, under the same
 * checks. Errors: SKIFF_ERR_STORAGE_NAME_TAKEN when both names are taken;
 * SKIFF_ERR_BUFFER_TOO_SMALL when the download would need a new record and the manifest is full;
 * SKIFF_ERR_INVALID_ARG for a NULL argument, a file skiff_install_check() does not support, a rom
 * id over SKIFF_INSTALL_ROM_ID_MAX, or a name that cleans to nothing; the storage's error when it
 * cannot tell whether a name is free; skiff_storage_resolve()'s error. out is zeroed on any error.
 */
skiff_err skiff_install_plan_download(const skiff_installer *installer,
                                      const skiff_romm_rom_summary *rom,
                                      const skiff_romm_file *file, const skiff_storage_roots *roots,
                                      const skiff_install_manifest *manifest,
                                      skiff_storage *storage, skiff_install_taken_fn taken,
                                      void *taken_ctx, skiff_install_plan *out);

#endif
