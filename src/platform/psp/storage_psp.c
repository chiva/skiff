#include "storage_psp.h"

#include <pspiofilemgr.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_SEPARATOR ':'
/* Firmware errors of the file system carry an errno in their low half. */
#define SCE_ERRNO_BASE 0x80010000U
#define SCE_ERRNO_MASK 0xFFFF0000U
#define SCE_ERRNO_VALUE 0x0000FFFFU
/* The device in a path is gone: no Memory Stick inserted. */
#define SCE_KERNEL_NO_SUCH_DEVICE 0x80020321U
/* SCE_KERNEL_ERROR_UNSUP (pspkerror.h): the device does not answer this request. */
#define SCE_KERNEL_UNSUPPORTED 0x80020325U
/* What sceIoMkdir returns for a name that exists (errno EEXIST): a folder, or a file. */
#define SCE_EEXIST_RESULT (SCE_ERRNO_BASE | 17U)

enum {
    NEW_FILE_PERMISSIONS = 0777,
    NEW_FOLDER_PERMISSIONS = 0777,
    /* errno values the firmware reports in SCE_ERRNO_BASE | errno. */
    SCE_ENOENT = 2,
    SCE_ENXIO = 6,
    SCE_ENODEV = 19,
    SCE_ENOSPC = 28,
    /* "ms0:" or "ef0:", and room to spare. */
    DEVICE_MAX = 16,
    SYNC_FLAGS = 0,
};

typedef struct psp_storage {
    skiff_storage base; /* first, so a skiff_storage * is a psp_storage * */
    skiff_psp_storage_failure failure;
} psp_storage;

typedef struct psp_file {
    skiff_file base;
    SceUID fd;
    char device[DEVICE_MAX];
} psp_file;

static skiff_err from_sce(int result) {
    const unsigned code = (unsigned)result;
    if (code == SCE_KERNEL_NO_SUCH_DEVICE) {
        return SKIFF_ERR_STORAGE_NO_MEDIA;
    }
    if ((code & SCE_ERRNO_MASK) == SCE_ERRNO_BASE) {
        switch (code & SCE_ERRNO_VALUE) {
        case SCE_ENOENT:
            return SKIFF_ERR_STORAGE_NOT_FOUND;
        case SCE_ENOSPC:
            return SKIFF_ERR_STORAGE_NO_SPACE;
        case SCE_ENXIO:
        case SCE_ENODEV:
            return SKIFF_ERR_STORAGE_NO_MEDIA;
        default:
            break;
        }
    }
    return SKIFF_ERR_STORAGE_IO;
}

/* Records a failed call and maps it; SKIFF_OK for a result that is not an error. */
static skiff_err step(psp_storage *storage, const char *call, int result) {
    if (result >= 0) {
        return SKIFF_OK;
    }
    storage->failure.call = call;
    storage->failure.sce_result = result;
    return from_sce(result);
}

static psp_storage *owner(const psp_file *file) { return (psp_storage *)file->base.storage; }

/* "ms0:/PSP/GAME/x" -> "ms0:"; 0 for a path without a device. */
static int device_of(const char *path, char *out) {
    const char *separator = strchr(path, DEVICE_SEPARATOR);
    if (separator == NULL || separator == path || separator - path + 2 > DEVICE_MAX) {
        return 0;
    }
    const size_t length = (size_t)(separator - path) + 1;
    memcpy(out, path, length);
    out[length] = '\0';
    return 1;
}

static int open_flags(skiff_file_mode mode) {
    switch (mode) {
    case SKIFF_FILE_READ:
        return PSP_O_RDONLY;
    case SKIFF_FILE_WRITE_AT:
        return PSP_O_WRONLY;
    case SKIFF_FILE_REPLACE:
    default:
        return PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC;
    }
}

/* WRITE_AT starts at offset, which must lie within the file. */
static skiff_err position(psp_storage *storage, SceUID fd, skiff_file_mode mode, uint64_t offset) {
    if (mode != SKIFF_FILE_WRITE_AT) {
        return SKIFF_OK;
    }
    const SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
    if (size < 0) {
        return step(storage, "sceIoLseek(end)", (int)size);
    }
    if (offset > (uint64_t)size) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const SceOff placed = sceIoLseek(fd, (SceOff)offset, PSP_SEEK_SET);
    return placed == (SceOff)offset
               ? SKIFF_OK
               : step(storage, "sceIoLseek(offset)", placed < 0 ? (int)placed : -1);
}

static skiff_err psp_open(skiff_storage *base, const char *path, skiff_file_mode mode,
                          uint64_t offset, skiff_file **out) {
    psp_storage *storage = (psp_storage *)base;
    char device[DEVICE_MAX];
    if (!device_of(path, device)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const SceUID fd = sceIoOpen(path, open_flags(mode), NEW_FILE_PERMISSIONS);
    if (fd < 0) {
        return step(storage, "sceIoOpen", fd);
    }
    const skiff_err err = position(storage, fd, mode, offset);
    psp_file *file = err == SKIFF_OK ? calloc(1, sizeof *file) : NULL;
    if (file == NULL) {
        sceIoClose(fd);
        return err != SKIFF_OK ? err : SKIFF_ERR_NO_MEMORY;
    }
    file->base.storage = base;
    file->fd = fd;
    memcpy(file->device, device, sizeof device);
    *out = &file->base;
    return SKIFF_OK;
}

static skiff_err psp_read(skiff_file *base, void *buffer, size_t size, size_t *got) {
    const psp_file *file = (const psp_file *)base;
    const int result = sceIoRead(file->fd, buffer, (SceSize)size);
    if (result < 0) {
        return step(owner(file), "sceIoRead", result);
    }
    *got = (size_t)result;
    return SKIFF_OK;
}

static skiff_err psp_write(skiff_file *base, const void *data, size_t size) {
    const psp_file *file = (const psp_file *)base;
    const unsigned char *next = data;
    while (size > 0) {
        const int written = sceIoWrite(file->fd, next, (SceSize)size);
        if (written < 0) {
            return step(owner(file), "sceIoWrite", written);
        }
        if (written == 0) {
            return SKIFF_ERR_STORAGE_NO_SPACE;
        }
        next += written;
        size -= (size_t)written;
    }
    return SKIFF_OK;
}

/* What libcglue's fsync() does: the firmware flushes a whole device, not one file. */
static skiff_err psp_sync(skiff_file *base) {
    const psp_file *file = (const psp_file *)base;
    return step(owner(file), "sceIoSync", sceIoSync(file->device, SYNC_FLAGS));
}

static skiff_err psp_close(skiff_file *base) {
    psp_file *file = (psp_file *)base;
    const skiff_err err = step(owner(file), "sceIoClose", sceIoClose(file->fd));
    free(file);
    return err;
}

static skiff_err psp_size(skiff_storage *base, const char *path, uint64_t *out) {
    SceIoStat status;
    memset(&status, 0, sizeof status);
    const skiff_err err = step((psp_storage *)base, "sceIoGetstat", sceIoGetstat(path, &status));
    if (err != SKIFF_OK) {
        return err;
    }
    if (!FIO_S_ISREG(status.st_mode)) {
        return SKIFF_ERR_STORAGE_IO;
    }
    *out = (uint64_t)status.st_size;
    return SKIFF_OK;
}

static skiff_err psp_rename(skiff_storage *base, const char *from, const char *to) {
    SceIoStat status;
    if (sceIoGetstat(to, &status) >= 0) {
        return SKIFF_ERR_STORAGE_IO;
    }
    return step((psp_storage *)base, "sceIoRename", sceIoRename(from, to));
}

static skiff_err psp_remove(skiff_storage *base, const char *path) {
    return step((psp_storage *)base, "sceIoRemove", sceIoRemove(path));
}

static skiff_err psp_mkdir(skiff_storage *base, const char *path) {
    const int result = sceIoMkdir(path, NEW_FOLDER_PERMISSIONS);
    if ((unsigned)result == SCE_EEXIST_RESULT) {
        SceIoStat status;
        memset(&status, 0, sizeof status);
        return sceIoGetstat(path, &status) >= 0 && FIO_S_ISDIR(status.st_mode)
                   ? SKIFF_OK
                   : SKIFF_ERR_STORAGE_IO;
    }
    return step((psp_storage *)base, "sceIoMkdir", result);
}

/* The device's cluster counts (pspiofilemgr_devctl.h): free bytes = free clusters x sectors per
 * cluster x bytes per sector. */
static skiff_err psp_free_space(skiff_storage *base, const char *path, uint64_t *out) {
    char device[DEVICE_MAX];
    if (!device_of(path, device)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    /* The request goes to the device, so check the path itself is a folder, as the host version
     * does: a download into a missing folder (or a file by that name) must fail before it starts.
     * The device itself ("ms0:" or "ms0:/") needs no check. */
    const size_t device_length = strlen(device);
    const int device_only = path[device_length] == '\0' ||
                            (path[device_length] == '/' && path[device_length + 1] == '\0');
    if (!device_only) {
        SceIoStat status;
        memset(&status, 0, sizeof status);
        const skiff_err err =
            step((psp_storage *)base, "sceIoGetstat", sceIoGetstat(path, &status));
        if (err != SKIFF_OK) {
            return err;
        }
        if (!FIO_S_ISDIR(status.st_mode)) {
            return SKIFF_ERR_STORAGE_IO;
        }
    }
    SceDevInf info;
    memset(&info, 0, sizeof info);
    SceDevctlCmd command = {&info};
    const int result = sceIoDevctl(device, SCE_PR_GETDEV, &command, sizeof command, NULL, 0);
    const skiff_err err = step((psp_storage *)base, "sceIoDevctl(SCE_PR_GETDEV)", result);
    if ((unsigned)result == SCE_KERNEL_UNSUPPORTED) {
        return SKIFF_ERR_NOT_IMPLEMENTED;
    }
    if (err != SKIFF_OK) {
        return err;
    }
    if (info.sectorSize < 0 || info.sectorCount < 0) {
        return SKIFF_ERR_STORAGE_IO;
    }
    *out = (uint64_t)info.freeClusters * (uint64_t)info.sectorCount * (uint64_t)info.sectorSize;
    return SKIFF_OK;
}

static void psp_destroy(skiff_storage *base) { free(base); }

static const skiff_storage_ops PSP_STORAGE_OPS = {
    psp_open,   psp_read,   psp_write, psp_sync,       psp_close,   psp_size,
    psp_rename, psp_remove, psp_mkdir, psp_free_space, psp_destroy,
};

skiff_err skiff_psp_storage_create(skiff_storage **out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = NULL;
    psp_storage *storage = calloc(1, sizeof *storage);
    if (storage == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    storage->base.ops = &PSP_STORAGE_OPS;
    *out = &storage->base;
    return SKIFF_OK;
}

skiff_psp_storage_failure skiff_psp_storage_last_failure(const skiff_storage *storage) {
    const skiff_psp_storage_failure none = {NULL, 0};
    if (storage == NULL || storage->ops != &PSP_STORAGE_OPS) {
        return none;
    }
    return ((const psp_storage *)storage)->failure;
}
