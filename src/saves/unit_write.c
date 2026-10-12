/*
 * Writing a unit as argosy-sigil's PSP unit, byte for byte (its src/zip_mem.c): for each file in
 * name order a local header (version 2.0, no flags, STORED, time 0, date 1980-01-01, CRC-32 and
 * sizes, the name, no extra field) and the bytes; then the central directory (made by and needing
 * version 2.0, the same fields, no extra field, comment, disk or attributes, and the local
 * header's offset) and the end record (one disk, the entry counts, the directory's size and
 * offset, no comment). With no data descriptor the CRC-32 and size go before the bytes, so each
 * file is read once to measure it and again to copy it.
 */
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "saves_internal.h"

typedef struct writer {
    skiff_storage *storage;
    const skiff_storage_roots *roots;
    skiff_saves_unit *unit;
    char unit_path[SKIFF_STORAGE_PATH_MAX];
    char file_path[SKIFF_STORAGE_PATH_MAX];
    /* Bytes of the unit written so far: where the next record starts. */
    uint32_t written;
    uint32_t crc[SKIFF_SAVES_FILES_MAX];
    uint32_t offset[SKIFF_SAVES_FILES_MAX];
    unsigned char header[ZIP_CENTRAL_BYTES + SKIFF_SAVES_ENTRY_SIZE];
    unsigned char buffer[SAVES_BUFFER_BYTES];
} writer;

/* What one read of a file found. */
typedef struct file_pass {
    uint64_t size;
    uint32_t crc;
} file_pass;

/* Reads the unit's file i from start to end, feeding each chunk to the unit when out is open, to
 * md5 when given, and to the CRC-32 when with_crc is set (the zip needs it; a hash does not). */
static skiff_err read_file(writer *w, size_t i, skiff_file *out, skiff_md5 *md5, int with_crc,
                           file_pass *pass) {
    const char *name = w->unit->files[i].name;
    const char *slash = strchr(name, SAVES_PATH_SEPARATOR);
    char folder[SKIFF_SAVES_NAME_SIZE];
    const size_t folder_length = slash != NULL ? (size_t)(slash - name) : 0;
    if (slash == NULL || folder_length >= sizeof folder) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memcpy(folder, name, folder_length);
    folder[folder_length] = '\0';
    skiff_err err = saves_resolve(w->roots, w->unit->root, folder, slash + 1, w->file_path,
                                  sizeof w->file_path);
    skiff_file *file = NULL;
    if (err == SKIFF_OK) {
        err = skiff_storage_open(w->storage, w->file_path, SKIFF_FILE_READ, 0, &file);
    }
    uLong crc = crc32(0L, Z_NULL, 0);
    pass->size = 0;
    size_t got = 1;
    while (err == SKIFF_OK && got > 0) {
        err = skiff_file_read(file, w->buffer, sizeof w->buffer, &got);
        if (err == SKIFF_OK && got > 0) {
            crc = with_crc ? crc32(crc, w->buffer, (uInt)got) : crc;
            pass->size += got;
            if (md5 != NULL) {
                skiff_md5_update(md5, w->buffer, got);
            }
            if (out != NULL) {
                err = skiff_file_write(out, w->buffer, got);
            }
        }
        if (err == SKIFF_OK && pass->size > w->unit->files[i].size) {
            err = SKIFF_ERR_STORAGE_IO;
        }
    }
    const skiff_err close_err = skiff_file_close(file);
    pass->crc = (uint32_t)crc;
    return err != SKIFF_OK ? err : close_err;
}

/* Opens the unit where the next record goes: reopened for every step, since a suspend
 * invalidates open files on the PSP. */
static skiff_err open_unit(writer *w, skiff_file **out) {
    return skiff_storage_open(w->storage, w->unit_path, SKIFF_FILE_WRITE_AT, w->written, out);
}

static skiff_err finish_step(skiff_file *out, skiff_err err, int sync) {
    if (err == SKIFF_OK && sync) {
        err = skiff_file_sync(out);
    }
    const skiff_err close_err = skiff_file_close(out);
    return err != SKIFF_OK ? err : close_err;
}

static uint16_t name_length(const skiff_saves_file *file) { return (uint16_t)strlen(file->name); }

static size_t local_header(writer *w, size_t i) {
    const skiff_saves_file *file = &w->unit->files[i];
    const uint16_t length = name_length(file);
    unsigned char *h = w->header;
    memset(h, 0, ZIP_LOCAL_BYTES);
    saves_write_le32(h, ZIP_LOCAL_SIGNATURE);
    saves_write_le16(h + ZIP_LOCAL_VERSION, ZIP_VERSION_20);
    saves_write_le16(h + ZIP_LOCAL_DATE, ZIP_DOS_DATE_1980_01_01);
    saves_write_le32(h + ZIP_LOCAL_CRC, w->crc[i]);
    saves_write_le32(h + ZIP_LOCAL_COMPRESSED, (uint32_t)file->size);
    saves_write_le32(h + ZIP_LOCAL_SIZE, (uint32_t)file->size);
    saves_write_le16(h + ZIP_LOCAL_NAME_LENGTH, length);
    memcpy(h + ZIP_LOCAL_BYTES, file->name, length);
    return ZIP_LOCAL_BYTES + (size_t)length;
}

static size_t central_header(writer *w, size_t i) {
    const skiff_saves_file *file = &w->unit->files[i];
    const uint16_t length = name_length(file);
    unsigned char *h = w->header;
    memset(h, 0, ZIP_CENTRAL_BYTES);
    saves_write_le32(h, ZIP_CENTRAL_SIGNATURE);
    saves_write_le16(h + ZIP_CENTRAL_MADE_BY, ZIP_VERSION_20);
    saves_write_le16(h + ZIP_CENTRAL_VERSION, ZIP_VERSION_20);
    saves_write_le16(h + ZIP_CENTRAL_DATE, ZIP_DOS_DATE_1980_01_01);
    saves_write_le32(h + ZIP_CENTRAL_CRC, w->crc[i]);
    saves_write_le32(h + ZIP_CENTRAL_COMPRESSED, (uint32_t)file->size);
    saves_write_le32(h + ZIP_CENTRAL_SIZE, (uint32_t)file->size);
    saves_write_le16(h + ZIP_CENTRAL_NAME_LENGTH, length);
    saves_write_le32(h + ZIP_CENTRAL_OFFSET, w->offset[i]);
    memcpy(h + ZIP_CENTRAL_BYTES, file->name, length);
    return ZIP_CENTRAL_BYTES + (size_t)length;
}

/* Measures file i (CRC-32, MD5), then writes its local header and copies it after. */
static skiff_err write_entry(writer *w, size_t i) {
    skiff_saves_file *file = &w->unit->files[i];
    skiff_md5 md5;
    skiff_md5_init(&md5);
    file_pass measured;
    skiff_err err = read_file(w, i, NULL, &md5, 1, &measured);
    if (err == SKIFF_OK && measured.size != file->size) {
        err = SKIFF_ERR_STORAGE_IO;
    }
    if (err != SKIFF_OK) {
        return err;
    }
    unsigned char digest[SKIFF_MD5_DIGEST_BYTES];
    skiff_md5_final(&md5, digest);
    skiff_md5_hex(digest, file->md5);
    w->crc[i] = measured.crc;
    w->offset[i] = w->written;
    skiff_file *out = NULL;
    err = open_unit(w, &out);
    if (err != SKIFF_OK) {
        return err;
    }
    const size_t header_bytes = local_header(w, i);
    err = skiff_file_write(out, w->header, header_bytes);
    file_pass copied = {0, 0};
    if (err == SKIFF_OK) {
        err = read_file(w, i, out, NULL, 1, &copied);
    }
    if (err == SKIFF_OK && (copied.size != measured.size || copied.crc != measured.crc)) {
        err = SKIFF_ERR_STORAGE_IO;
    }
    err = finish_step(out, err, 0);
    if (err == SKIFF_OK) {
        w->written += (uint32_t)(header_bytes + file->size);
    }
    return err;
}

static skiff_err write_directory(writer *w) {
    skiff_file *out = NULL;
    skiff_err err = open_unit(w, &out);
    if (err != SKIFF_OK) {
        return err;
    }
    const uint32_t directory_offset = w->written;
    uint32_t directory_bytes = 0;
    for (size_t i = 0; err == SKIFF_OK && i < w->unit->file_count; i++) {
        const size_t bytes = central_header(w, i);
        err = skiff_file_write(out, w->header, bytes);
        directory_bytes += (uint32_t)bytes;
    }
    unsigned char *end = w->header;
    memset(end, 0, ZIP_END_BYTES);
    saves_write_le32(end, ZIP_END_SIGNATURE);
    saves_write_le16(end + ZIP_END_DISK_ENTRIES, (uint16_t)w->unit->file_count);
    saves_write_le16(end + ZIP_END_ENTRIES, (uint16_t)w->unit->file_count);
    saves_write_le32(end + ZIP_END_DIRECTORY_SIZE, directory_bytes);
    saves_write_le32(end + ZIP_END_DIRECTORY_OFFSET, directory_offset);
    if (err == SKIFF_OK) {
        err = skiff_file_write(out, end, ZIP_END_BYTES);
    }
    return finish_step(out, err, 1);
}

/* Each file named "<FOLDER>/<FILE>" with both parts valid, within the caps. */
static int writable(const skiff_saves_unit *unit) {
    if (unit->root[0] == '\0' || unit->file_count > SKIFF_SAVES_FILES_MAX ||
        unit->total_bytes > SKIFF_SAVES_UNIT_BYTES_MAX) {
        return 0;
    }
    uint64_t total = 0;
    for (size_t i = 0; i < unit->file_count; i++) {
        const char *name = unit->files[i].name;
        const char *slash = strchr(name, SAVES_PATH_SEPARATOR);
        if (slash == NULL || !saves_name_valid(name, (size_t)(slash - name)) ||
            !saves_name_valid(slash + 1, strlen(slash + 1))) {
            return 0;
        }
        total += unit->files[i].size;
    }
    return total <= SKIFF_SAVES_UNIT_BYTES_MAX;
}

/* The unit's files' MD5s, empty again after a failure. */
static void forget_md5s(skiff_saves_unit *unit) {
    for (size_t i = 0; i < unit->file_count; i++) {
        unit->files[i].md5[0] = '\0';
    }
}

skiff_err skiff_saves_hash_unit(skiff_storage *storage, const skiff_storage_roots *roots,
                                skiff_saves_unit *unit) {
    if (unit != NULL) {
        unit->content_hash[0] = '\0';
    }
    if (storage == NULL || roots == NULL || unit == NULL || !writable(unit)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    writer *w = calloc(1, sizeof *w);
    if (w == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    w->storage = storage;
    w->roots = roots;
    w->unit = unit;
    skiff_err err = SKIFF_OK;
    for (size_t i = 0; err == SKIFF_OK && i < unit->file_count; i++) {
        skiff_md5 md5;
        skiff_md5_init(&md5);
        file_pass pass;
        err = read_file(w, i, NULL, &md5, 0, &pass);
        if (err == SKIFF_OK && pass.size != unit->files[i].size) {
            err = SKIFF_ERR_STORAGE_IO;
        }
        if (err == SKIFF_OK) {
            unsigned char digest[SKIFF_MD5_DIGEST_BYTES];
            skiff_md5_final(&md5, digest);
            skiff_md5_hex(digest, unit->files[i].md5);
        }
    }
    free(w);
    if (err == SKIFF_OK) {
        saves_content_hash(unit);
    } else {
        forget_md5s(unit);
    }
    return err;
}

skiff_err skiff_saves_write_unit(skiff_storage *storage, const skiff_storage_roots *roots,
                                 skiff_saves_unit *unit, const char *unit_path) {
    if (unit != NULL) {
        unit->content_hash[0] = '\0';
    }
    if (storage == NULL || roots == NULL || unit == NULL || unit_path == NULL || !writable(unit)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    writer *w = calloc(1, sizeof *w);
    if (w == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    w->storage = storage;
    w->roots = roots;
    w->unit = unit;
    skiff_err err = skiff_storage_resolve(roots, unit_path, w->unit_path, sizeof w->unit_path);
    if (err == SKIFF_OK) {
        skiff_file *out = NULL;
        err = skiff_storage_open(storage, w->unit_path, SKIFF_FILE_REPLACE, 0, &out);
        err = err == SKIFF_OK ? skiff_file_close(out) : err;
        for (size_t i = 0; err == SKIFF_OK && i < unit->file_count; i++) {
            err = write_entry(w, i);
        }
        if (err == SKIFF_OK) {
            err = write_directory(w);
        }
        if (err != SKIFF_OK) {
            (void)skiff_storage_remove(storage, w->unit_path);
        }
    }
    free(w);
    if (err == SKIFF_OK) {
        saves_content_hash(unit);
    } else {
        forget_md5s(unit);
    }
    return err;
}
