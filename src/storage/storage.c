#include "skiff/storage.h"

#include <string.h>

#define DEVICE_SEPARATOR ':'
#define PATH_SEPARATOR '/'

static int usable(const skiff_storage *storage) { return storage != NULL && storage->ops != NULL; }

static int usable_file(const skiff_file *file) { return file != NULL && usable(file->storage); }

skiff_err skiff_storage_open(skiff_storage *storage, const char *path, skiff_file_mode mode,
                             uint64_t offset, skiff_file **out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = NULL;
    const int known_mode =
        mode == SKIFF_FILE_READ || mode == SKIFF_FILE_WRITE_AT || mode == SKIFF_FILE_REPLACE;
    if (!usable(storage) || path == NULL || path[0] == '\0' || !known_mode ||
        (mode != SKIFF_FILE_WRITE_AT && offset != 0)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return storage->ops->open(storage, path, mode, offset, out);
}

skiff_err skiff_file_read(skiff_file *file, void *buffer, size_t size, size_t *got) {
    if (got != NULL) {
        *got = 0;
    }
    if (!usable_file(file) || got == NULL || (buffer == NULL && size > 0)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return size == 0 ? SKIFF_OK : file->storage->ops->read(file, buffer, size, got);
}

skiff_err skiff_file_write(skiff_file *file, const void *data, size_t size) {
    if (!usable_file(file) || (data == NULL && size > 0)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return size == 0 ? SKIFF_OK : file->storage->ops->write(file, data, size);
}

skiff_err skiff_file_sync(skiff_file *file) {
    return usable_file(file) ? file->storage->ops->sync(file) : SKIFF_ERR_INVALID_ARG;
}

skiff_err skiff_file_close(skiff_file *file) {
    if (file == NULL) {
        return SKIFF_OK;
    }
    return usable_file(file) ? file->storage->ops->close(file) : SKIFF_ERR_INVALID_ARG;
}

skiff_err skiff_storage_size(skiff_storage *storage, const char *path, uint64_t *out) {
    if (out != NULL) {
        *out = 0;
    }
    if (!usable(storage) || path == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return storage->ops->size(storage, path, out);
}

skiff_err skiff_storage_rename(skiff_storage *storage, const char *from, const char *to) {
    if (!usable(storage) || from == NULL || to == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return storage->ops->rename(storage, from, to);
}

skiff_err skiff_storage_remove(skiff_storage *storage, const char *path) {
    if (!usable(storage) || path == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return storage->ops->remove(storage, path);
}

skiff_err skiff_storage_mkdir(skiff_storage *storage, const char *path) {
    if (!usable(storage) || path == NULL || path[0] == '\0') {
        return SKIFF_ERR_INVALID_ARG;
    }
    return storage->ops->mkdir(storage, path);
}

/* Where the folders of path start: after "ms0:/" (or "ms0:"), or after a leading '/'. */
static size_t first_folder(const char *path) {
    const char *separator = strchr(path, DEVICE_SEPARATOR);
    const char *slash = strchr(path, PATH_SEPARATOR);
    size_t start = 0;
    if (separator != NULL && (slash == NULL || separator < slash)) {
        start = (size_t)(separator - path) + 1;
    }
    while (path[start] == PATH_SEPARATOR) {
        start++;
    }
    return start;
}

skiff_err skiff_storage_mkdirs(skiff_storage *storage, const char *path) {
    if (!usable(storage) || path == NULL || path[0] == '\0') {
        return SKIFF_ERR_INVALID_ARG;
    }
    char walk[SKIFF_STORAGE_PATH_MAX];
    size_t length = strlen(path);
    if (length >= sizeof walk) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memcpy(walk, path, length + 1);
    while (length > 0 && walk[length - 1] == PATH_SEPARATOR) {
        walk[--length] = '\0';
    }
    const size_t start = first_folder(walk);
    if (start >= length) {
        return SKIFF_ERR_INVALID_ARG; /* a device or "/" alone: nothing to create */
    }
    skiff_err err = SKIFF_OK;
    for (size_t i = start; err == SKIFF_OK && i < length; i++) {
        if (walk[i] == PATH_SEPARATOR && walk[i - 1] != PATH_SEPARATOR) {
            walk[i] = '\0';
            err = storage->ops->mkdir(storage, walk);
            walk[i] = PATH_SEPARATOR;
        }
    }
    return err == SKIFF_OK ? storage->ops->mkdir(storage, walk) : err;
}

skiff_err skiff_storage_free_space(skiff_storage *storage, const char *path, uint64_t *out) {
    if (out != NULL) {
        *out = 0;
    }
    if (!usable(storage) || path == NULL || path[0] == '\0' || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return storage->ops->free_space(storage, path, out);
}

skiff_err skiff_storage_check_room(skiff_storage *storage, const char *path, uint64_t needed) {
    if (needed > SKIFF_STORAGE_MAX_FILE_BYTES) {
        return SKIFF_ERR_STORAGE_FILE_TOO_LARGE;
    }
    uint64_t free_bytes = 0;
    const skiff_err err = skiff_storage_free_space(storage, path, &free_bytes);
    if (err != SKIFF_OK) {
        return err;
    }
    /* needed is below 4 GiB, so the sum cannot overflow. */
    return free_bytes < needed + SKIFF_STORAGE_FREE_MARGIN_BYTES ? SKIFF_ERR_STORAGE_NO_SPACE
                                                                 : SKIFF_OK;
}

void skiff_storage_destroy(skiff_storage *storage) {
    if (usable(storage) && storage->ops->destroy != NULL) {
        storage->ops->destroy(storage);
    }
}
