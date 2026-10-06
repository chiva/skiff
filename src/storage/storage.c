#include "skiff/storage.h"

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

void skiff_storage_destroy(skiff_storage *storage) {
    if (usable(storage) && storage->ops->destroy != NULL) {
        storage->ops->destroy(storage);
    }
}
