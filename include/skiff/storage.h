#ifndef SKIFF_STORAGE_H
#define SKIFF_STORAGE_H

/*
 * The seam between Skiff and the Memory Stick. Layers above (jobs/) never call the C library's file
 * functions: on the PSP newlib's off_t is 32 bits, so stdio cannot place a file position past 2
 * GiB, and the PSP implementation uses sceIo with 64-bit offsets instead. Host tests use a POSIX
 * implementation over a temporary directory (src/platform/host/storage_posix.h), wrapped by a fake
 * that injects failures (tests/support/fake_storage.h). A file is used by one thread at a time.
 *
 * Every call returns SKIFF_ERR_STORAGE_NO_MEDIA, _NO_SPACE, _IO or _NOT_FOUND for what the device
 * reports, SKIFF_ERR_INVALID_ARG for a NULL argument.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"

/* FAT32 cannot hold a file of 4 GiB or more. */
#define SKIFF_STORAGE_MAX_FILE_BYTES 0xFFFFFFFFULL
/* The longest path the storage helpers build or walk, terminator included. */
#define SKIFF_STORAGE_PATH_MAX 256
/* A name's buffer, terminator included: a safe name from RomM (skiff/storage_paths.h) or one a
 * folder listing reports. FAT allows 255 UTF-16 units per name; Skiff stays well under it so a name
 * plus its folder and a ".resume" suffix fits SKIFF_STORAGE_PATH_MAX. */
#define SKIFF_STORAGE_NAME_MAX 128
/* skiff_storage_info.mtime when the device holds no valid date for the entry. */
#define SKIFF_STORAGE_MTIME_UNKNOWN 0
/* Left free on top of what a download needs: its .resume file, skiff.log, config.ini, and the
 * file system's own room for directory entries, so a full Memory Stick never stops Skiff saving its
 * settings or the player saving a game. */
#define SKIFF_STORAGE_FREE_MARGIN_BYTES ((uint64_t)8 * 1024 * 1024)

typedef enum skiff_file_mode {
    /* An existing file, read from offset (at most its size). */
    SKIFF_FILE_READ,
    /* An existing file, written from offset (at most its size); nothing is truncated, so bytes past
     * what is written stay as they were. */
    SKIFF_FILE_WRITE_AT,
    /* Created, or emptied if it exists, and written from its start. */
    SKIFF_FILE_REPLACE,
} skiff_file_mode;

typedef struct skiff_storage skiff_storage;
typedef struct skiff_file skiff_file;

/*
 * What a folder listing or skiff_storage_stat() reports for a file or folder. mtime is when it was
 * last written, in seconds since 1970-01-01 00:00 of the clock that stamped it: on the PSP that is
 * the console's local time as FAT records it (no time zone), counted as if it were UTC; on the host
 * it is UTC. Compare it only with values read from the same device (equal: the file is unchanged),
 * and show it as a date without a time zone. SKIFF_STORAGE_MTIME_UNKNOWN when the device holds no
 * valid date.
 */
typedef struct skiff_storage_info {
    int is_dir;
    uint64_t size; /* 0 for a folder */
    int64_t mtime;
} skiff_storage_info;

typedef struct skiff_storage_entry {
    char name[SKIFF_STORAGE_NAME_MAX];
    skiff_storage_info info;
} skiff_storage_entry;

/* What a listing callback returns: keep going, or end the listing now (skiff_storage_list() then
 * returns SKIFF_OK). */
typedef enum skiff_storage_list_step {
    SKIFF_STORAGE_LIST_CONTINUE,
    SKIFF_STORAGE_LIST_STOP,
} skiff_storage_list_step;

/* Called once per entry of a listing; entry is only valid during the call. */
typedef skiff_storage_list_step (*skiff_storage_entry_fn)(void *ctx,
                                                          const skiff_storage_entry *entry);

/* What an implementation's list op calls for every entry the device reports, "." and ".." included:
 * name is length bytes long and NUL-terminated when length is below the implementation's own
 * buffer. skiff_storage_list() filters and bounds these into skiff_storage_entry. */
typedef skiff_storage_list_step (*skiff_storage_visit_fn)(void *ctx, const char *name,
                                                          size_t length,
                                                          const skiff_storage_info *info);

/* A date and time of day as a device's clock shows it (the PSP's ScePspDateTime). */
typedef struct skiff_storage_date {
    int year;
    int month; /* 1 to 12 */
    int day;   /* 1 to 31 */
    int hour;
    int minute;
    int second;
} skiff_storage_date;

/*
 * Turns date into skiff_storage_info.mtime. SKIFF_ERR_INVALID_ARG for a NULL argument or a field
 * out of range (years 1 to 9999, a day the month has, 23:59:59 at most), with *out (when not NULL)
 * SKIFF_STORAGE_MTIME_UNKNOWN.
 */
skiff_err skiff_storage_mtime_from_date(const skiff_storage_date *date, int64_t *out);

typedef struct skiff_storage_ops {
    skiff_err (*open)(skiff_storage *storage, const char *path, skiff_file_mode mode,
                      uint64_t offset, skiff_file **out);
    skiff_err (*read)(skiff_file *file, void *buffer, size_t size, size_t *got);
    skiff_err (*write)(skiff_file *file, const void *data, size_t size);
    skiff_err (*sync)(skiff_file *file);
    skiff_err (*close)(skiff_file *file);
    skiff_err (*size)(skiff_storage *storage, const char *path, uint64_t *out);
    skiff_err (*rename)(skiff_storage *storage, const char *from, const char *to);
    skiff_err (*remove)(skiff_storage *storage, const char *path);
    skiff_err (*mkdir)(skiff_storage *storage, const char *path);
    skiff_err (*free_space)(skiff_storage *storage, const char *path, uint64_t *out);
    skiff_err (*stat)(skiff_storage *storage, const char *path, skiff_storage_info *out);
    skiff_err (*list)(skiff_storage *storage, const char *path, skiff_storage_visit_fn visit,
                      void *ctx);
    skiff_err (*rmdir)(skiff_storage *storage, const char *path);
    void (*destroy)(skiff_storage *storage);
} skiff_storage_ops;

/* Implementations embed these as their first member. */
struct skiff_storage {
    const skiff_storage_ops *ops;
};

struct skiff_file {
    skiff_storage *storage;
};

/*
 * Opens path in mode. READ and WRITE_AT need an existing file at least offset bytes long (reading
 * from the end reads nothing); REPLACE takes offset 0. *out is NULL on error. Close the file with
 * skiff_file_close().
 */
skiff_err skiff_storage_open(skiff_storage *storage, const char *path, skiff_file_mode mode,
                             uint64_t offset, skiff_file **out);

/* Reads up to size bytes; *got is 0 at the end of the file. */
skiff_err skiff_file_read(skiff_file *file, void *buffer, size_t size, size_t *got);

/* Writes all size bytes, or fails. */
skiff_err skiff_file_write(skiff_file *file, const void *data, size_t size);

/* Returns once what was written is on the device, so a power cut afterwards cannot lose it. */
skiff_err skiff_file_sync(skiff_file *file);

/* Closes and frees the file, also when it reports an error. Does nothing for NULL. */
skiff_err skiff_file_close(skiff_file *file);

skiff_err skiff_storage_size(skiff_storage *storage, const char *path, uint64_t *out);

/* Fails when to exists: FAT cannot replace a file in one step, so remove it first. */
skiff_err skiff_storage_rename(skiff_storage *storage, const char *from, const char *to);

skiff_err skiff_storage_remove(skiff_storage *storage, const char *path);

/* Creates the folder path; one that already exists is not an error, a file by that name is
 * (SKIFF_ERR_STORAGE_IO). Its parent must exist: see skiff_storage_mkdirs(). */
skiff_err skiff_storage_mkdir(skiff_storage *storage, const char *path);

/* Creates path and every missing folder above it, from the device ("ms0:") or the file system's
 * root ("/") down. */
skiff_err skiff_storage_mkdirs(skiff_storage *storage, const char *path);

/* Free bytes on the device that holds the folder path (or the device itself, "ms0:").
 * SKIFF_ERR_STORAGE_NOT_FOUND for a missing folder, SKIFF_ERR_STORAGE_IO when path is a file,
 * SKIFF_ERR_NOT_IMPLEMENTED when the device cannot tell (PPSSPP, for some devices). */
skiff_err skiff_storage_free_space(skiff_storage *storage, const char *path, uint64_t *out);

/*
 * Whether a file of needed bytes fits in the folder path, with
 * SKIFF_STORAGE_FREE_MARGIN_BYTES to spare: SKIFF_ERR_STORAGE_FILE_TOO_LARGE for more than
 * SKIFF_STORAGE_MAX_FILE_BYTES (FAT32 cannot store it whatever the space),
 * SKIFF_ERR_STORAGE_NO_SPACE when it does not fit, otherwise skiff_storage_free_space()'s error or
 * SKIFF_OK. For a download being resumed, needed is what is still to come.
 */
skiff_err skiff_storage_check_room(skiff_storage *storage, const char *path, uint64_t needed);

/* ---- Folders: listing, entry details, removal (save sync walks PSP/SAVEDATA) ---- */

/* What path is: SKIFF_ERR_STORAGE_NOT_FOUND when it does not exist. *out is zeroed on any error. */
skiff_err skiff_storage_stat(skiff_storage *storage, const char *path, skiff_storage_info *out);

/*
 * Calls on_entry for each file and folder directly in the folder path (not recursive), in the
 * device's order, without "." and "..". Names come as the device reports them: FAT ignores case and
 * PPSSPP reports a name that fits 8.3 in capitals ("folder" lists as "FOLDER"), so compare them
 * without regard to case. A name of SKIFF_STORAGE_NAME_MAX bytes or more (or an empty
 * one) is skipped and counted in *skipped rather than cut, since a cut name would point at another
 * file: one stray name must not hide the rest of the folder, and a caller that has to see every
 * entry (before removing the folder) checks *skipped. The listing ends early when on_entry returns
 * SKIFF_STORAGE_LIST_STOP. SKIFF_ERR_STORAGE_NOT_FOUND for a missing folder, SKIFF_ERR_STORAGE_IO
 * when path is a file. An error part-way through (on the PSP a suspend invalidates the open folder)
 * comes after on_entry has seen some entries: discard them and list again. Every return closes the
 * folder.
 */
skiff_err skiff_storage_list(skiff_storage *storage, const char *path,
                             skiff_storage_entry_fn on_entry, void *ctx, size_t *skipped);

/* Removes the empty folder path. SKIFF_ERR_STORAGE_IO when it holds anything or is a file,
 * SKIFF_ERR_STORAGE_NOT_FOUND when it does not exist. The folder is listed first and refused if it
 * holds anything, since some devices (PPSSPP) remove a full folder; a file written into it between
 * the two steps by another thread is not guarded against. */
skiff_err skiff_storage_rmdir(skiff_storage *storage, const char *path);

/* ---- Small files replaced whole (config.ini, the download queue, installed.json) ---- */

/* FAT cannot replace a file in one step, so a replacement writes "<path>.tmp", renames it
 * "<path>.new" once synced (so a .new file is always complete), then replaces the file. */
#define SKIFF_STORAGE_DRAFT_SUFFIX ".tmp"
#define SKIFF_STORAGE_PENDING_SUFFIX ".new"

/*
 * Reads the file at path into buffer (capacity bytes; any data, not terminated); its length goes to
 * *length. First finishes or undoes a replacement cut short by a power loss (see
 * skiff_storage_replace_whole()): a .tmp file is deleted; a .new file without the file becomes the
 * file; a .new file beside the file is deleted when it can be, and the file is read either way. No
 * file at all reads as empty (length 0). SKIFF_ERR_BUFFER_TOO_SMALL when the file holds more than
 * capacity bytes, SKIFF_ERR_INVALID_ARG for a NULL argument or a path too long for
 * SKIFF_STORAGE_PATH_MAX with a suffix, otherwise the storage's error; *length is 0 on any error.
 */
skiff_err skiff_storage_read_whole(skiff_storage *storage, const char *path, void *buffer,
                                   size_t capacity, size_t *length);

/*
 * Replaces the file at path with length bytes of data: write and sync "<path>.tmp", rename it
 * "<path>.new" and sync the device, remove the file, rename .new into place and sync again. A power
 * cut leaves the old file, or a complete .new file the next read or replacement puts in place; a
 * cut .tmp file is never used. A replacement first finishes an earlier cut one the same way. On the
 * PSP a sync flushes the whole device (sceIoSync), directory entries included, which is what makes
 * each rename durable before the next step. Returns SKIFF_ERR_INVALID_ARG for a NULL argument or a
 * path too long, otherwise the storage's error; after a failure the next read still finds either
 * the old content or the new, whole.
 */
skiff_err skiff_storage_replace_whole(skiff_storage *storage, const char *path, const void *data,
                                      size_t length);

/* Frees the storage. Does nothing for NULL. */
void skiff_storage_destroy(skiff_storage *storage);

#endif
