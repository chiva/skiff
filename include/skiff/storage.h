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

typedef enum skiff_file_mode {
    /* An existing file, read from its start. */
    SKIFF_FILE_READ,
    /* An existing file, written from offset (at most its size); nothing is truncated, so bytes past
     * what is written stay as they were. */
    SKIFF_FILE_WRITE_AT,
    /* Created, or emptied if it exists, and written from its start. */
    SKIFF_FILE_REPLACE,
} skiff_file_mode;

typedef struct skiff_storage skiff_storage;
typedef struct skiff_file skiff_file;

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
 * Opens path in mode. WRITE_AT needs an existing file at least offset bytes long; READ and REPLACE
 * take offset 0. *out is NULL on error. Close the file with skiff_file_close().
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

/* Frees the storage. Does nothing for NULL. */
void skiff_storage_destroy(skiff_storage *storage);

#endif
