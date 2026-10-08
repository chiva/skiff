#ifndef SKIFF_STORAGE_PATHS_H
#define SKIFF_STORAGE_PATHS_H

/*
 * Where Skiff's files go, and names that are safe to put there. Code above storage/ addresses
 * logical roots ("games:/Game.iso") rather than devices: the roots follow the device the EBOOT runs
 * from (the Memory Stick, ms0:, or a PSP Go's internal storage, ef0:), and point at a temporary
 * directory in host tests.
 *
 *   app:    the EBOOT's folder (PSP/GAME/Skiff): config.ini, skiff.log, certificates
 *   games:  <device>/ISO, where PSP games go
 *   saves:  <device>/PSP/SAVEDATA, where the firmware keeps saves
 *
 * Names that come from RomM pass through skiff_storage_safe_name() before they become part of a
 * path (AGENTS.md: paths built from RomM data are sanitised before touching the Memory Stick).
 */

#include <stddef.h>

#include "skiff/error.h"
#include "skiff/storage.h"

#define SKIFF_STORAGE_ROOT_APP "app:"
#define SKIFF_STORAGE_ROOT_GAMES "games:"
#define SKIFF_STORAGE_ROOT_SAVES "saves:"
#define SKIFF_STORAGE_GAMES_FOLDER "/ISO"
#define SKIFF_STORAGE_SAVES_FOLDER "/PSP/SAVEDATA"

/* A safe name's buffer, terminator included. FAT allows 255 UTF-16 units per name; Skiff stays well
 * under it so a name plus its folder and a ".resume" suffix fits SKIFF_STORAGE_PATH_MAX. */
#define SKIFF_STORAGE_NAME_MAX 128
/* A name from RomM longer than this (in bytes) is refused rather than sanitised: no file system
 * RomM runs on allows one. */
#define SKIFF_STORAGE_NAME_INPUT_MAX 512
/* The longest ending, dot included, that a shortened name keeps as its extension (".iso", ".cso").
 */
#define SKIFF_STORAGE_EXTENSION_MAX 16

typedef struct skiff_storage_roots {
    char app[SKIFF_STORAGE_PATH_MAX];
    char games[SKIFF_STORAGE_PATH_MAX];
    char saves[SKIFF_STORAGE_PATH_MAX];
} skiff_storage_roots;

/*
 * Roots on device_root ("ms0:", or a host directory in tests), with app_dir as app:. Neither may be
 * empty or end in '/'. SKIFF_ERR_INVALID_ARG for a NULL or unusable argument,
 * SKIFF_ERR_BUFFER_TOO_SMALL when a root does not fit SKIFF_STORAGE_PATH_MAX.
 */
skiff_err skiff_storage_roots_init(const char *device_root, const char *app_dir,
                                   skiff_storage_roots *out);

/*
 * Roots for the EBOOT at program_path, argv[0] as the PSP passes it
 * ("ms0:/PSP/GAME/Skiff/EBOOT.PBP"): the device is what precedes the first ':', app: the EBOOT's
 * folder. SKIFF_ERR_INVALID_ARG for a path without a device or a folder.
 */
skiff_err skiff_storage_roots_from_program(const char *program_path, skiff_storage_roots *out);

/*
 * Turns "games:/Game.iso" into "ms0:/ISO/Game.iso" (a root alone, "games:", gives the root's
 * folder). Every part after the root must be a plain name: SKIFF_ERR_INVALID_ARG for an unknown
 * root, an empty part ("games://x", a trailing '/'), a part ending in '.' or a blank (FAT drops
 * them, so ".. ." would become ".."; this also refuses "." and ".."), '\', ':' or a control
 * character, so nothing resolves outside its root. SKIFF_ERR_BUFFER_TOO_SMALL when the result does
 * not fit; out is empty on any error.
 */
skiff_err skiff_storage_resolve(const skiff_storage_roots *roots, const char *logical, char *out,
                                size_t out_size);

/*
 * The reverse of skiff_storage_resolve(): turns "ms0:/ISO/Game.iso" back into "games:/Game.iso" (a
 * root's folder alone gives the root, "games:"), for a path that resolve could have produced. The
 * path must start with a root's folder exactly, followed by nothing or by '/' and plain names (the
 * same rule resolve applies); when two roots' folders match, the longer wins. SKIFF_ERR_INVALID_ARG
 * for a NULL argument or a path under no root or with a part resolve refuses,
 * SKIFF_ERR_BUFFER_TOO_SMALL when the result does not fit; out is empty on any error.
 */
skiff_err skiff_storage_logical_path(const skiff_storage_roots *roots, const char *path, char *out,
                                     size_t out_size);

/*
 * Writes "<directory of program_path>/<file_name>" into out. program_path is argv[0] as the PSP
 * passes it, e.g. "ms0:/PSP/GAME/SkiffSelftest/EBOOT.PBP". Returns SKIFF_ERR_INVALID_ARG for a NULL
 * argument or a path without a directory, SKIFF_ERR_BUFFER_TOO_SMALL if the result does not fit; on
 * any error out is left empty (when out_size allows).
 */
skiff_err skiff_storage_sibling_path(const char *program_path, const char *file_name, char *out,
                                     size_t out_size);

/*
 * Makes a file name from RomM safe for FAT and the PSP:
 *   - '\', '/', ':', '*', '?', '"', '<', '>', '|', control characters and bytes that are not UTF-8
 *     become '_' (so a name can never add a folder or a device);
 *   - blanks at the start, and blanks and dots at the end, are removed (FAT drops trailing dots);
 *   - a DOS device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9, any case, with or without an
 *     extension) gets a leading '_';
 *   - a name over SKIFF_STORAGE_NAME_MAX - 1 bytes is shortened at a character boundary, keeping
 *     its extension (up to SKIFF_STORAGE_EXTENSION_MAX bytes).
 * SKIFF_ERR_INVALID_ARG for a NULL argument, a name over SKIFF_STORAGE_NAME_INPUT_MAX bytes, or one
 * that is empty once cleaned ("", ".", "..", "  "); SKIFF_ERR_BUFFER_TOO_SMALL if the result does
 * not fit out (SKIFF_STORAGE_NAME_MAX always does). out is empty on any error.
 *
 * Safe is not unique: "a/b.iso" and "a:b.iso" both become "a_b.iso", and FAT ignores case. Whoever
 * picks the file a download replaces (the installer) must not trust the name alone.
 */
skiff_err skiff_storage_safe_name(const char *name, char *out, size_t out_size);

#endif
