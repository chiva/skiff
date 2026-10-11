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
    if (fake->on_call != NULL) {
        fake->on_call(fake->on_call_ctx, "open", path);
    }
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
    fake_storage *fake = owner(file);
    if (stale(file)) {
        return SKIFF_ERR_STORAGE_IO;
    }
    if (file->matches && fake->read_error != SKIFF_OK) {
        if (fake->bytes_read >= fake->read_budget) {
            return fake->read_error;
        }
        const uint64_t left = fake->read_budget - fake->bytes_read;
        size = left < size ? (size_t)left : size;
    }
    const skiff_err err = skiff_file_read(file->inner, buffer, size, got);
    if (err == SKIFF_OK && file->matches) {
        fake->bytes_read += *got;
    }
    return err;
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
    owner(file)->closes++;
    const int was_stale = stale(file);
    const skiff_err err = skiff_file_close(file->inner);
    free(file);
    return was_stale ? SKIFF_ERR_STORAGE_IO : err;
}

static skiff_err fake_size(skiff_storage *base, const char *path, uint64_t *out) {
    fake_storage *fake = (fake_storage *)base;
    if (fake->size_failures > 0 && matches(fake, path)) {
        fake->size_failures--;
        return SKIFF_ERR_STORAGE_IO;
    }
    return skiff_storage_size(fake->inner, path, out);
}

static skiff_err fake_rename(skiff_storage *base, const char *from, const char *to) {
    const fake_storage *fake = (const fake_storage *)base;
    return fake->rename_error != SKIFF_OK && matches(fake, from)
               ? fake->rename_error
               : skiff_storage_rename(fake->inner, from, to);
}

static skiff_err fake_remove(skiff_storage *base, const char *path) {
    const fake_storage *fake = (const fake_storage *)base;
    if (fake->on_call != NULL) {
        fake->on_call(fake->on_call_ctx, "remove", path);
    }
    const skiff_err err = skiff_storage_remove(fake->inner, path);
    return err == SKIFF_OK && fake->remove_error != SKIFF_OK && matches(fake, path)
               ? fake->remove_error
               : err;
}

static skiff_err fake_mkdir(skiff_storage *base, const char *path) {
    fake_storage *fake = (fake_storage *)base;
    fake->mkdirs++;
    return fake->mkdir_error != SKIFF_OK && matches(fake, path)
               ? fake->mkdir_error
               : skiff_storage_mkdir(fake->inner, path);
}

static skiff_err fake_free_space(skiff_storage *base, const char *path, uint64_t *out) {
    fake_storage *fake = (fake_storage *)base;
    fake->free_space_queries++;
    if (fake->free_space_error != SKIFF_OK) {
        return fake->free_space_error;
    }
    if (fake->has_free_bytes) {
        *out = fake->free_bytes;
        return SKIFF_OK;
    }
    return skiff_storage_free_space(fake->inner, path, out);
}

static skiff_err fake_stat(skiff_storage *base, const char *path, skiff_storage_info *out) {
    const fake_storage *fake = (const fake_storage *)base;
    return fake->stat_error != SKIFF_OK && matches(fake, path)
               ? fake->stat_error
               : skiff_storage_stat(fake->inner, path, out);
}

typedef struct fake_listing {
    skiff_storage_visit_fn visit;
    void *ctx;
    /* Entries left to pass on before the listing fails; negative for no failure. */
    int left;
    int failed;
} fake_listing;

static skiff_storage_list_step relay_entry(void *ctx, const char *name, size_t length,
                                           const skiff_storage_info *info) {
    fake_listing *listing = ctx;
    const int dot =
        (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.');
    if (!dot && listing->left == 0) {
        listing->failed = 1;
        return SKIFF_STORAGE_LIST_STOP;
    }
    if (!dot && listing->left > 0) {
        listing->left--;
    }
    return listing->visit(listing->ctx, name, length, info);
}

/* The inner storage's own list op, so the entries reach skiff_storage_list()'s filter as the
 * device reports them. */
static skiff_err fake_list(skiff_storage *base, const char *path, skiff_storage_visit_fn visit,
                           void *ctx) {
    const fake_storage *fake = (const fake_storage *)base;
    const int fails = fake->list_error != SKIFF_OK && matches(fake, path);
    if (fails && fake->list_error_after == 0) {
        return fake->list_error;
    }
    fake_listing listing = {visit, ctx, fails ? fake->list_error_after : -1, 0};
    const skiff_err err = fake->inner->ops->list(fake->inner, path, relay_entry, &listing);
    return listing.failed ? fake->list_error : err;
}

static skiff_err fake_rmdir(skiff_storage *base, const char *path) {
    fake_storage *fake = (fake_storage *)base;
    fake->rmdirs++;
    return fake->rmdir_error != SKIFF_OK && matches(fake, path)
               ? fake->rmdir_error
               : skiff_storage_rmdir(fake->inner, path);
}

static void fake_destroy(skiff_storage *base) { (void)base; }

static const skiff_storage_ops FAKE_STORAGE_OPS = {
    fake_open,   fake_read,  fake_write,      fake_sync, fake_close, fake_size,  fake_rename,
    fake_remove, fake_mkdir, fake_free_space, fake_stat, fake_list,  fake_rmdir, fake_destroy,
};

void fake_storage_init(fake_storage *fake, skiff_storage *inner) {
    memset(fake, 0, sizeof *fake);
    fake->base.ops = &FAKE_STORAGE_OPS;
    fake->inner = inner;
}
