#ifndef SKIFF_SAVES_H
#define SKIFF_SAVES_H

/*
 * Save units: one game's PSP save folders as the single zip RomM stores as a save. A game saves in
 * every folder of saves: (PSP/SAVEDATA) whose name starts with its save_target, the id RomM scans
 * from the disc (e.g. ULUS10064 for ULUS10064DATA00 and ULUS10064SYSTEM), and the folders travel
 * together: PARAM.SFO holds hashes of its folder's files, and a game's folders refer to each other.
 *
 * Skiff's unit is argosy-sigil's PSP unit byte for byte, so anything built on sigil restores it:
 * a STORED zip of files only (no folder entries), each named "<FOLDER>/<FILE>" exactly as the
 * Memory Stick lists it, in byte order of the names, dated 1980-01-01 00:00, with no extra fields,
 * no UTF-8 flag and no data descriptor. Units other clients wrote (Argosy, grout: deflated, with
 * folder entries, any order) are read too.
 *
 * Every unit has RomM's content_hash, the MD5 of its "<name>:<md5 of the file>" lines (see
 * skiff_saves_unit), which RomM computes the same way for any zip holding the same files, however
 * compressed or ordered: Skiff compares saves by it.
 *
 * A unit downloaded from RomM is untrusted, and so are the folders on the Memory Stick: names,
 * counts and sizes are checked before anything is written, and writes go only to
 * "<folder>/<file>" under a root the caller names, through skiff_storage_resolve(). Files are
 * opened and closed per step, since a suspend invalidates open files on the PSP, and streamed
 * through small heap buffers: the PSP's browsing thread has a 32 KB stack.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"
#include "skiff/md5.h"
#include "skiff/storage.h"
#include "skiff/storage_paths.h"

/* save_target is 4 to 16 of [A-Z0-9] (RomM's is the 9-character disc id, ULUS10064). */
#define SKIFF_SAVES_TARGET_MIN 4
#define SKIFF_SAVES_TARGET_MAX 16
#define SKIFF_SAVES_TARGET_SIZE (SKIFF_SAVES_TARGET_MAX + 1)
/* The longest folder or file name in a unit, in bytes; PSP save names are under 20. */
#define SKIFF_SAVES_NAME_MAX 64
#define SKIFF_SAVES_NAME_SIZE (SKIFF_SAVES_NAME_MAX + 1)
/* "<FOLDER>/<FILE>" with its terminator. */
#define SKIFF_SAVES_ENTRY_SIZE (2 * SKIFF_SAVES_NAME_MAX + 2)
#define SKIFF_SAVES_FOLDERS_MAX 64
#define SKIFF_SAVES_FILES_MAX 256
/* What a unit's files may add up to; the largest PSP saves are a few MB. */
#define SKIFF_SAVES_UNIT_BYTES_MAX ((uint64_t)64 * 1024 * 1024)
/* The largest unit file read: its files plus their zip headers, extra fields and deflate's
 * overhead. */
#define SKIFF_SAVES_UNIT_FILE_BYTES_MAX (SKIFF_SAVES_UNIT_BYTES_MAX + (uint64_t)4 * 1024 * 1024)
#define SKIFF_SAVES_HASH_SIZE SKIFF_MD5_HEX_SIZE
/* A unit travels as "<save_target>.zip", as sigil names it. */
#define SKIFF_SAVES_UNIT_EXTENSION ".zip"

/* Why a game's folders cannot be collected, or a unit was refused. */
typedef enum skiff_saves_problem {
    SKIFF_SAVES_PROBLEM_NONE,
    /* A folder inside a save folder, or a unit entry more than one folder deep. */
    SKIFF_SAVES_PROBLEM_NESTED_FOLDER,
    /* A name that is not printable ASCII, longer than SKIFF_SAVES_NAME_MAX, or invalid on FAT
     * (see skiff_saves_name_valid()); also a name too long for a folder listing. */
    SKIFF_SAVES_PROBLEM_NAME,
    /* Two folders or files whose names differ only in case (one file on FAT), or an entry twice. */
    SKIFF_SAVES_PROBLEM_DUPLICATE,
    SKIFF_SAVES_PROBLEM_TOO_MANY_FOLDERS,
    SKIFF_SAVES_PROBLEM_TOO_MANY_FILES,
    /* Files adding up to more than SKIFF_SAVES_UNIT_BYTES_MAX, or a unit file over
     * SKIFF_SAVES_UNIT_FILE_BYTES_MAX. */
    SKIFF_SAVES_PROBLEM_TOO_LARGE,
    /* Only in a unit: an absolute path, a "." or ".." part, '\', ':', an empty part, or a file
     * outside any folder. */
    SKIFF_SAVES_PROBLEM_PATH,
    /* Only in a unit: a folder whose name does not start with the game's save_target. */
    SKIFF_SAVES_PROBLEM_OTHER_GAME,
    SKIFF_SAVES_PROBLEM_ENCRYPTED,
    SKIFF_SAVES_PROBLEM_ZIP64,
    SKIFF_SAVES_PROBLEM_MULTI_DISK,
    /* A compression method other than stored or deflate. */
    SKIFF_SAVES_PROBLEM_METHOD,
    /* A file whose data is not the size its headers declare. */
    SKIFF_SAVES_PROBLEM_SIZE,
    SKIFF_SAVES_PROBLEM_CRC,
    /* Not a zip, cut short, a zip comment or bytes outside the entries, a central directory that
     * disagrees with the local headers, or damaged deflate data. */
    SKIFF_SAVES_PROBLEM_STRUCTURE,
} skiff_saves_problem;

/* A short stable name for logs ("nested-folder"); "unknown" outside the enum. */
const char *skiff_saves_problem_name(skiff_saves_problem problem);

typedef struct skiff_saves_file {
    /* "<FOLDER>/<FILE>", the entry's name in the unit. */
    char name[SKIFF_SAVES_ENTRY_SIZE];
    uint64_t size;
    /* From the folder listing (skiff/storage.h); SKIFF_STORAGE_MTIME_UNKNOWN in a unit read from a
     * zip. */
    int64_t mtime;
    /* The file's MD5 as lowercase hex, once written into or read from a unit; empty before. */
    char md5[SKIFF_MD5_HEX_SIZE];
} skiff_saves_file;

/* One game's save folders and their files, collected from the Memory Stick or read from a unit.
 * About 50 KB: allocate it with skiff_saves_unit_create(). */
typedef struct skiff_saves_unit {
    char save_target[SKIFF_SAVES_TARGET_SIZE];
    /* The logical folder the save folders are in: the root collected from ("saves:"), the staging
     * folder a unit was extracted into, or empty for a unit only checked. */
    char root[SKIFF_STORAGE_PATH_MAX];
    /* Sorted byte by byte. A folder without files is listed here, but has no entry in a unit. */
    size_t folder_count;
    char folders[SKIFF_SAVES_FOLDERS_MAX][SKIFF_SAVES_NAME_SIZE];
    /* Sorted by name, byte by byte: the order of a unit's entries. */
    size_t file_count;
    skiff_saves_file files[SKIFF_SAVES_FILES_MAX];
    uint64_t total_bytes;
    /* RomM's content_hash, as lowercase hex, once the unit was written or read; empty before. The
     * MD5 of the lines "<name>:<md5>" of every file, in the order of the names, joined by "\n"
     * without a final newline: RomM's hash_zip_contents, which skips folder entries, counts empty
     * files and takes names as they are. */
    char content_hash[SKIFF_SAVES_HASH_SIZE];
    /* Why the last collect or read refused (SKIFF_SAVES_PROBLEM_NONE otherwise), and the folder,
     * file or entry name at fault (cut to fit, and empty when none applies). */
    skiff_saves_problem problem;
    char problem_name[SKIFF_SAVES_ENTRY_SIZE];
} skiff_saves_unit;

/* Allocates an empty unit. SKIFF_ERR_INVALID_ARG for NULL, SKIFF_ERR_NO_MEMORY. */
skiff_err skiff_saves_unit_create(skiff_saves_unit **out);

/* Frees the unit. Does nothing for NULL. */
void skiff_saves_unit_destroy(skiff_saves_unit *unit);

/* Whether save_target is 4 to 16 characters of [A-Z0-9]: what Skiff accepts from RomM before the
 * id becomes part of a folder match. 0 for NULL. */
int skiff_saves_target_valid(const char *save_target);

/*
 * Whether name can be a save folder's or file's name in a unit: 1 to SKIFF_SAVES_NAME_MAX bytes of
 * printable ASCII (0x20 to 0x7E), none of \ / : * ? " < > |, not starting with a blank nor ending
 * in a blank or '.' (FAT drops those, which also refuses "." and ".."), and not a DOS device name
 * (skiff_storage_is_dos_device()). 0 for NULL.
 */
int skiff_saves_name_valid(const char *name);

/*
 * Collects the game's save folders in the logical folder root (SKIFF_STORAGE_ROOT_SAVES, or a
 * scratch folder in tests and the self-test) into unit: every folder directly in root whose name
 * starts with save_target, ignoring case (a listing may give 8.3 names in capitals), and the files
 * directly in each, with their sizes and dates. A folder whose PARAM.SFO parses and carries neither
 * SAVEDATA_PARAMS nor SAVEDATA_FILE_LIST holds game data a game installed, not saves, and is left
 * out; a folder without a PARAM.SFO, or with one that does not parse, stays (argosy-sigil's rule
 * from its documentation). Files are not read. A missing root holds no saves (SKIFF_OK, no
 * folders).
 *
 * SKIFF_ERR_STORAGE_SAVE_UNSYNCABLE when the folders hold what no unit may: a folder inside a save
 * folder, a name that is not valid (skiff_saves_name_valid(), or too long to list), two names that
 * differ only in case, more than SKIFF_SAVES_FOLDERS_MAX folders or SKIFF_SAVES_FILES_MAX files, or
 * more than SKIFF_SAVES_UNIT_BYTES_MAX in all; unit->problem and unit->problem_name say which.
 * Names in root that do not match save_target are never checked. SKIFF_ERR_INVALID_ARG for a NULL
 * argument or an invalid save_target, otherwise the storage's error.
 */
skiff_err skiff_saves_collect(skiff_storage *storage, const skiff_storage_roots *roots,
                              const char *root, const char *save_target, skiff_saves_unit *unit);

/*
 * Writes unit's files, read from unit->root, as a unit to the logical path unit_path (its folder
 * must exist; a file there is replaced), and fills each file's md5 and unit->content_hash. Each
 * file is read twice, through a heap buffer: once for its CRC-32 and MD5, then to copy it after its
 * header. SKIFF_ERR_STORAGE_IO when a file is no longer the size the collect saw, or changed
 * between the two reads; otherwise the storage's error. On any error the partial unit is removed,
 * and the files' md5 and unit->content_hash are empty. SKIFF_ERR_INVALID_ARG for a NULL argument, a
 * unit without a root, or one over its caps.
 */
skiff_err skiff_saves_write_unit(skiff_storage *storage, const skiff_storage_roots *roots,
                                 skiff_saves_unit *unit, const char *unit_path);

/*
 * Reads the unit at the logical path unit_path, checks it as a unit of save_target's saves, and
 * fills unit (root set to staging, or empty), each file's md5 and unit->content_hash. With a
 * staging folder (a logical path, created if missing; give an empty one), the unit's folders and
 * files are written into it too; without (NULL), nothing is written.
 *
 * The whole unit is refused, with SKIFF_ERR_STORAGE_SAVE_DAMAGED and unit->problem saying why,
 * when an entry's name is not "<FOLDER>/<FILE>" (or "<FOLDER>/" for a folder entry, which is
 * otherwise ignored) with both parts valid (skiff_saves_name_valid()) and the folder starting with
 * save_target, ignoring case; when an entry appears twice, also differing only in case, or a folder
 * is spelled two ways; for an encrypted, ZIP64 or multi-disk unit or a method other than stored or
 * deflate; for more than SKIFF_SAVES_FOLDERS_MAX folders, SKIFF_SAVES_FILES_MAX files or
 * SKIFF_SAVES_UNIT_BYTES_MAX in all (checked before anything is inflated); when data is not the
 * size declared (inflate stops there) or fails its CRC-32; and when the zip is cut short, has a
 * comment or bytes outside its entries, or its central directory and local headers disagree.
 * Nothing is extracted before the names, counts and headers pass; a later refusal or storage error
 * removes what was extracted. SKIFF_ERR_INVALID_ARG for a NULL argument (but staging) or an
 * invalid save_target, SKIFF_ERR_NO_MEMORY, otherwise the storage's error.
 */
skiff_err skiff_saves_read_unit(skiff_storage *storage, const skiff_storage_roots *roots,
                                const char *unit_path, const char *save_target, const char *staging,
                                skiff_saves_unit *unit);

#endif
