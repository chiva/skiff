#include <stdint.h>
#include <stdio.h>

#include "skiff/storage.h"

/* The two files a replacement goes through, next to the file. */
typedef struct save_paths {
    /* Written and synced first; never trusted, since a power cut can leave it cut short. */
    char draft[SKIFF_STORAGE_PATH_MAX];
    /* The draft renamed once it is complete: if it exists, it is a whole file. */
    char pending[SKIFF_STORAGE_PATH_MAX];
} save_paths;

static int with_suffix(const char *path, const char *suffix, char *out, size_t out_size) {
    const int written = snprintf(out, out_size, "%s%s", path, suffix);
    return written > 0 && (size_t)written < out_size;
}

static int make_save_paths(const char *path, save_paths *paths) {
    return with_suffix(path, SKIFF_STORAGE_DRAFT_SUFFIX, paths->draft, sizeof paths->draft) &&
           with_suffix(path, SKIFF_STORAGE_PENDING_SUFFIX, paths->pending, sizeof paths->pending);
}

static skiff_err read_all(skiff_storage *storage, const char *path, char *text, size_t text_size,
                          size_t *length) {
    skiff_file *file = NULL;
    skiff_err err = skiff_storage_open(storage, path, SKIFF_FILE_READ, 0, &file);
    if (err != SKIFF_OK) {
        return err;
    }
    size_t used = 0;
    size_t got = 1;
    while (err == SKIFF_OK && got > 0 && used < text_size - 1) {
        err = skiff_file_read(file, text + used, text_size - 1 - used, &got);
        used += err == SKIFF_OK ? got : 0;
    }
    if (err == SKIFF_OK && used == text_size - 1) {
        char extra = 0;
        err = skiff_file_read(file, &extra, sizeof extra, &got);
        if (err == SKIFF_OK && got > 0) {
            err = SKIFF_ERR_BUFFER_TOO_SMALL;
        }
    }
    const skiff_err close_err = skiff_file_close(file);
    if (err == SKIFF_OK) {
        err = close_err;
    }
    if (err != SKIFF_OK) {
        text[0] = '\0';
        return err;
    }
    text[used] = '\0';
    *length = used;
    return SKIFF_OK;
}

/* Makes a rename or remove durable before the next step relies on it. The PSP flushes a whole
 * device on any file's sync (sceIoSync), directory entries included, so syncing the file a rename
 * produced commits the rename. */
static skiff_err flush_device(skiff_storage *storage, const char *path) {
    skiff_file *file = NULL;
    skiff_err err = skiff_storage_open(storage, path, SKIFF_FILE_READ, 0, &file);
    if (err != SKIFF_OK) {
        return err;
    }
    err = skiff_file_sync(file);
    const skiff_err close_err = skiff_file_close(file);
    return err != SKIFF_OK ? err : close_err;
}

/*
 * Finishes or undoes a replacement cut short, so path holds the newest complete content. A draft
 * is dropped (it may be cut short). A pending file is complete: without the file the replacement
 * was cut between the remove and the rename, so the rename is finished; beside the file it was cut
 * before the remove, so the old file stands. Removing a file nobody trusts is only tidying, so a
 * failure there never stops the file from being read.
 */
static skiff_err recover(skiff_storage *storage, const char *path, const save_paths *paths) {
    (void)skiff_storage_remove(storage, paths->draft);
    uint64_t size = 0;
    const skiff_err err = skiff_storage_size(storage, path, &size);
    const skiff_err pending_err = skiff_storage_size(storage, paths->pending, &size);
    if (err == SKIFF_ERR_STORAGE_NOT_FOUND) {
        if (pending_err == SKIFF_ERR_STORAGE_NOT_FOUND) {
            return SKIFF_OK;
        }
        if (pending_err != SKIFF_OK) {
            return pending_err;
        }
        const skiff_err rename_err = skiff_storage_rename(storage, paths->pending, path);
        return rename_err == SKIFF_OK ? flush_device(storage, path) : rename_err;
    }
    if (err == SKIFF_OK && pending_err != SKIFF_ERR_STORAGE_NOT_FOUND) {
        (void)skiff_storage_remove(storage, paths->pending);
    }
    return err;
}

skiff_err skiff_storage_read_whole(skiff_storage *storage, const char *path, char *text,
                                   size_t text_size, size_t *length) {
    if (text != NULL && text_size > 0) {
        text[0] = '\0';
    }
    if (length != NULL) {
        *length = 0;
    }
    save_paths paths;
    if (storage == NULL || path == NULL || text == NULL || text_size == 0 || length == NULL ||
        !make_save_paths(path, &paths)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_err err = recover(storage, path, &paths);
    if (err == SKIFF_OK) {
        err = read_all(storage, path, text, text_size, length);
    }
    return err == SKIFF_ERR_STORAGE_NOT_FOUND ? SKIFF_OK : err;
}

static skiff_err write_synced(skiff_storage *storage, const char *path, const void *data,
                              size_t length) {
    skiff_file *file = NULL;
    skiff_err err = skiff_storage_open(storage, path, SKIFF_FILE_REPLACE, 0, &file);
    if (err != SKIFF_OK) {
        return err;
    }
    if (length > 0) {
        err = skiff_file_write(file, data, length);
    }
    if (err == SKIFF_OK) {
        err = skiff_file_sync(file);
    }
    const skiff_err close_err = skiff_file_close(file);
    return err != SKIFF_OK ? err : close_err;
}

skiff_err skiff_storage_replace_whole(skiff_storage *storage, const char *path, const void *data,
                                      size_t length) {
    save_paths paths;
    if (storage == NULL || path == NULL || (data == NULL && length > 0) ||
        !make_save_paths(path, &paths)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    /* A pending file from an earlier cut replacement may be the only copy of the content. */
    skiff_err err = recover(storage, path, &paths);
    if (err != SKIFF_OK && err != SKIFF_ERR_STORAGE_NOT_FOUND) {
        return err;
    }
    err = write_synced(storage, paths.draft, data, length);
    if (err == SKIFF_OK) {
        err = skiff_storage_rename(storage, paths.draft, paths.pending);
    }
    if (err != SKIFF_OK) {
        (void)skiff_storage_remove(storage, paths.draft);
        return err;
    }
    /* From here the pending file is complete. Whatever fails, it stays for the next load to
     * judge: a failed remove may still have taken the old file with it. The old file is only
     * removed once the pending file's name is on the device. */
    err = flush_device(storage, paths.pending);
    if (err == SKIFF_OK) {
        err = skiff_storage_remove(storage, path);
        err = err == SKIFF_ERR_STORAGE_NOT_FOUND ? SKIFF_OK : err;
    }
    if (err == SKIFF_OK) {
        err = skiff_storage_rename(storage, paths.pending, path);
    }
    return err == SKIFF_OK ? flush_device(storage, path) : err;
}
