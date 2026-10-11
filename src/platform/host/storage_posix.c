#include "storage_posix.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define NEW_FILE_PERMISSIONS 0644
#define NEW_FOLDER_PERMISSIONS 0755

typedef struct posix_file {
    skiff_file base; /* first, so a skiff_file * is a posix_file * */
    int fd;
} posix_file;

static skiff_err from_errno(int error) {
    switch (error) {
    case ENOENT:
    case ENOTDIR:
        return SKIFF_ERR_STORAGE_NOT_FOUND;
    case ENOSPC:
    case EDQUOT:
        return SKIFF_ERR_STORAGE_NO_SPACE;
    default:
        return SKIFF_ERR_STORAGE_IO;
    }
}

static int open_flags(skiff_file_mode mode) {
    switch (mode) {
    case SKIFF_FILE_READ:
        return O_RDONLY;
    case SKIFF_FILE_WRITE_AT:
        return O_WRONLY;
    case SKIFF_FILE_REPLACE:
    default:
        return O_WRONLY | O_CREAT | O_TRUNC;
    }
}

/* READ and WRITE_AT start at offset, which must lie within the file. */
static skiff_err position(int fd, skiff_file_mode mode, uint64_t offset) {
    if (mode == SKIFF_FILE_REPLACE || (mode == SKIFF_FILE_READ && offset == 0)) {
        return SKIFF_OK;
    }
    struct stat status;
    if (fstat(fd, &status) != 0) {
        return from_errno(errno);
    }
    if (offset > (uint64_t)status.st_size) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return lseek(fd, (off_t)offset, SEEK_SET) == (off_t)offset ? SKIFF_OK : from_errno(errno);
}

static skiff_err posix_open(skiff_storage *storage, const char *path, skiff_file_mode mode,
                            uint64_t offset, skiff_file **out) {
    const int fd = open(path, open_flags(mode) | O_CLOEXEC, NEW_FILE_PERMISSIONS);
    if (fd < 0) {
        return from_errno(errno);
    }
    const skiff_err err = position(fd, mode, offset);
    posix_file *file = err == SKIFF_OK ? calloc(1, sizeof *file) : NULL;
    if (file == NULL) {
        close(fd);
        return err != SKIFF_OK ? err : SKIFF_ERR_NO_MEMORY;
    }
    file->base.storage = storage;
    file->fd = fd;
    *out = &file->base;
    return SKIFF_OK;
}

static skiff_err posix_read(skiff_file *base, void *buffer, size_t size, size_t *got) {
    const posix_file *file = (const posix_file *)base;
    ssize_t result = 0;
    do {
        result = read(file->fd, buffer, size);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        return from_errno(errno);
    }
    *got = (size_t)result;
    return SKIFF_OK;
}

static skiff_err posix_write(skiff_file *base, const void *data, size_t size) {
    const posix_file *file = (const posix_file *)base;
    const unsigned char *next = data;
    while (size > 0) {
        const ssize_t written = write(file->fd, next, size);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return written < 0 ? from_errno(errno) : SKIFF_ERR_STORAGE_NO_SPACE;
        }
        next += written;
        size -= (size_t)written;
    }
    return SKIFF_OK;
}

static skiff_err posix_sync(skiff_file *base) {
    const posix_file *file = (const posix_file *)base;
    return fsync(file->fd) == 0 ? SKIFF_OK : from_errno(errno);
}

static skiff_err posix_close(skiff_file *base) {
    posix_file *file = (posix_file *)base;
    const int result = close(file->fd);
    const int error = errno;
    free(file);
    return result == 0 ? SKIFF_OK : from_errno(error);
}

static skiff_err posix_size(skiff_storage *storage, const char *path, uint64_t *out) {
    (void)storage;
    struct stat status;
    if (stat(path, &status) != 0) {
        return from_errno(errno);
    }
    if (!S_ISREG(status.st_mode)) {
        return SKIFF_ERR_STORAGE_IO;
    }
    *out = (uint64_t)status.st_size;
    return SKIFF_OK;
}

static skiff_err posix_rename(skiff_storage *storage, const char *from, const char *to) {
    (void)storage;
    struct stat status;
    if (stat(to, &status) == 0) {
        return SKIFF_ERR_STORAGE_IO;
    }
    return rename(from, to) == 0 ? SKIFF_OK : from_errno(errno);
}

static skiff_err posix_remove(skiff_storage *storage, const char *path) {
    (void)storage;
    return unlink(path) == 0 ? SKIFF_OK : from_errno(errno);
}

static skiff_err posix_mkdir(skiff_storage *storage, const char *path) {
    (void)storage;
    if (mkdir(path, NEW_FOLDER_PERMISSIONS) == 0) {
        return SKIFF_OK;
    }
    const int error = errno;
    struct stat status;
    if (error == EEXIST) {
        return stat(path, &status) == 0 && S_ISDIR(status.st_mode) ? SKIFF_OK
                                                                   : SKIFF_ERR_STORAGE_IO;
    }
    return from_errno(error);
}

static skiff_err posix_free_space(skiff_storage *storage, const char *path, uint64_t *out) {
    (void)storage;
    struct stat status;
    if (stat(path, &status) != 0) {
        return from_errno(errno);
    }
    if (!S_ISDIR(status.st_mode)) {
        return SKIFF_ERR_STORAGE_IO;
    }
    struct statvfs device;
    if (statvfs(path, &device) != 0) {
        return from_errno(errno);
    }
    *out = (uint64_t)device.f_bavail * (uint64_t)device.f_frsize;
    return SKIFF_OK;
}

/* What the PSP reports for a folder call on a file: SKIFF_ERR_STORAGE_IO, where POSIX says ENOTDIR
 * (which from_errno() keeps for a file in the middle of a path: not found). */
static skiff_err folder_error(const char *path, int error) {
    struct stat status;
    if (error == ENOTDIR && stat(path, &status) == 0 && !S_ISDIR(status.st_mode)) {
        return SKIFF_ERR_STORAGE_IO;
    }
    return from_errno(error);
}

static void info_from_stat(const struct stat *status, skiff_storage_info *out) {
    out->is_dir = S_ISDIR(status->st_mode) ? 1 : 0;
    out->size = out->is_dir ? 0 : (uint64_t)status->st_size;
    out->mtime = (int64_t)status->st_mtime;
}

static skiff_err posix_stat(skiff_storage *storage, const char *path, skiff_storage_info *out) {
    (void)storage;
    struct stat status;
    if (stat(path, &status) != 0) {
        return from_errno(errno);
    }
    info_from_stat(&status, out);
    return SKIFF_OK;
}

static skiff_err posix_list(skiff_storage *storage, const char *path, skiff_storage_visit_fn visit,
                            void *ctx) {
    (void)storage;
    DIR *folder = opendir(path);
    if (folder == NULL) {
        return folder_error(path, errno);
    }
    skiff_err err = SKIFF_OK;
    skiff_storage_info info;
    struct stat status;
    for (;;) {
        errno = 0;
        const struct dirent *entry = readdir(folder);
        if (entry == NULL) {
            err = errno != 0 ? from_errno(errno) : SKIFF_OK;
            break;
        }
        if (fstatat(dirfd(folder), entry->d_name, &status, 0) != 0) {
            if (errno == ENOENT) {
                continue; /* removed since readdir() saw it */
            }
            err = from_errno(errno);
            break;
        }
        info_from_stat(&status, &info);
        if (visit(ctx, entry->d_name, strlen(entry->d_name), &info) !=
            SKIFF_STORAGE_LIST_CONTINUE) {
            break;
        }
    }
    const int closed = closedir(folder);
    if (err == SKIFF_OK && closed != 0) {
        err = from_errno(errno);
    }
    return err;
}

static skiff_err posix_rmdir(skiff_storage *storage, const char *path) {
    (void)storage;
    if (rmdir(path) == 0) {
        return SKIFF_OK;
    }
    const int error = errno;
    return error == ENOTEMPTY || error == EEXIST ? SKIFF_ERR_STORAGE_IO : folder_error(path, error);
}

static void posix_destroy(skiff_storage *storage) { free(storage); }

static const skiff_storage_ops POSIX_STORAGE_OPS = {
    posix_open, posix_read,   posix_write,  posix_sync,    posix_close,
    posix_size, posix_rename, posix_remove, posix_mkdir,   posix_free_space,
    posix_stat, posix_list,   posix_rmdir,  posix_destroy,
};

skiff_err skiff_posix_storage_create(skiff_storage **out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = NULL;
    skiff_storage *storage = calloc(1, sizeof *storage);
    if (storage == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    storage->ops = &POSIX_STORAGE_OPS;
    *out = storage;
    return SKIFF_OK;
}
