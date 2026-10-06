#include "fake_storage.h"

#include <stdlib.h>
#include <string.h>

typedef struct fake_file {
    skiff_file base;
    skiff_file *inner;
    int matches;
    int generation;
} fake_file;

static int matches(const fake_storage *fake, const char *path) {
    if (fake->fail_suffix == NULL || fake->fail_suffix[0] == '\0') {
        return 1;
    }
    const size_t length = strlen(path);
    const size_t suffix_length = strlen(fake->fail_suffix);
    return length >= suffix_length && strcmp(path + length - suffix_length, fake->fail_suffix) == 0;
}

static fake_storage *owner(const fake_file *file) { return (fake_storage *)file->base.storage; }

static int stale(const fake_file *file) { return file->generation != owner(file)->generation; }

static skiff_err fake_open(skiff_storage *base, const char *path, skiff_file_mode mode,
                           uint64_t offset, skiff_file **out) {
    fake_storage *fake = (fake_storage *)base;
    fake->opens++;
    fake_file *file = calloc(1, sizeof *file);
    if (file == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    const skiff_err err = skiff_storage_open(fake->inner, path, mode, offset, &file->inner);
    if (err != SKIFF_OK) {
        free(file);
        return err;
    }
    file->base.storage = base;
    file->matches = matches(fake, path);
    file->generation = fake->generation;
    *out = &file->base;
    return SKIFF_OK;
}

static skiff_err fake_read(skiff_file *base, void *buffer, size_t size, size_t *got) {
    const fake_file *file = (const fake_file *)base;
    return stale(file) ? SKIFF_ERR_STORAGE_IO : skiff_file_read(file->inner, buffer, size, got);
}

static skiff_err fake_write(skiff_file *base, const void *data, size_t size) {
    const fake_file *file = (const fake_file *)base;
    fake_storage *fake = owner(file);
    if (stale(file)) {
        return SKIFF_ERR_STORAGE_IO;
    }
    if (!file->matches) {
        return skiff_file_write(file->inner, data, size);
    }
    fake->writes++;
    if (fake->write_error != SKIFF_OK && fake->bytes_written + size > fake->write_budget) {
        const size_t fits = (size_t)(fake->write_budget - fake->bytes_written);
        fake->bytes_written += fits;
        const skiff_err err = skiff_file_write(file->inner, data, fits);
        return err != SKIFF_OK ? err : fake->write_error;
    }
    const skiff_err err = skiff_file_write(file->inner, data, size);
    fake->bytes_written += size;
    if (fake->stale_after_writes > 0 && fake->writes == fake->stale_after_writes) {
        fake->generation++;
        fake->handles_lost++;
    }
    return err;
}

static skiff_err fake_sync(skiff_file *base) {
    const fake_file *file = (const fake_file *)base;
    fake_storage *fake = owner(file);
    if (stale(file)) {
        return SKIFF_ERR_STORAGE_IO;
    }
    fake->syncs++;
    if (file->matches && fake->sync_error != SKIFF_OK) {
        return fake->sync_error;
    }
    return skiff_file_sync(file->inner);
}

static skiff_err fake_close(skiff_file *base) {
    fake_file *file = (fake_file *)base;
    const int was_stale = stale(file);
    const skiff_err err = skiff_file_close(file->inner);
    free(file);
    return was_stale ? SKIFF_ERR_STORAGE_IO : err;
}

static skiff_err fake_size(skiff_storage *base, const char *path, uint64_t *out) {
    return skiff_storage_size(((fake_storage *)base)->inner, path, out);
}

static skiff_err fake_rename(skiff_storage *base, const char *from, const char *to) {
    const fake_storage *fake = (const fake_storage *)base;
    return fake->rename_error != SKIFF_OK && matches(fake, from)
               ? fake->rename_error
               : skiff_storage_rename(fake->inner, from, to);
}

static skiff_err fake_remove(skiff_storage *base, const char *path) {
    const fake_storage *fake = (const fake_storage *)base;
    const skiff_err err = skiff_storage_remove(fake->inner, path);
    return err == SKIFF_OK && fake->remove_error != SKIFF_OK && matches(fake, path)
               ? fake->remove_error
               : err;
}

static void fake_destroy(skiff_storage *base) { (void)base; }

static const skiff_storage_ops FAKE_STORAGE_OPS = {
    fake_open, fake_read,   fake_write,  fake_sync,    fake_close,
    fake_size, fake_rename, fake_remove, fake_destroy,
};

void fake_storage_init(fake_storage *fake, skiff_storage *inner) {
    memset(fake, 0, sizeof *fake);
    fake->base.ops = &FAKE_STORAGE_OPS;
    fake->inner = inner;
}
