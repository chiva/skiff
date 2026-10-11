/*
 * Reading a unit: any zip of "<FOLDER>/<FILE>" entries, stored or deflated, with or without folder
 * entries and data descriptors, as Skiff, argosy-sigil, Argosy and grout write them. A unit comes
 * from RomM and is untrusted, so it is read in three steps and refused whole at the first problem:
 *
 *   1. the end record and the central directory: names, flags, methods, counts and sizes, so a
 *      unit that could hold too much is refused before anything is inflated;
 *   2. every local header against its central directory entry, and the layout: entries back to
 *      back from the start of the file, in central directory order, then the directory, then the
 *      end record, with nothing between, after or overlapping;
 *   3. the data: copied or inflated (inflate stops at the declared size), checked against its
 *      CRC-32, hashed, and written into the staging folder when there is one.
 *
 * Nothing is written before steps 1 and 2 pass; a refusal in step 3 removes what it extracted.
 */
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "saves_internal.h"

/* Folder entries come on top of the files: one per folder at most. */
#define ENTRIES_MAX (SKIFF_SAVES_FILES_MAX + SKIFF_SAVES_FOLDERS_MAX)
#define PRINTABLE_FIRST 0x20
#define PRINTABLE_LAST 0x7E
#define DOS_DEVICE_SEPARATOR ':'
#define WINDOWS_SEPARATOR '\\'
#define CURRENT_FOLDER "."
#define PARENT_FOLDER ".."

typedef struct zip_entry {
    char name[SKIFF_SAVES_ENTRY_SIZE];
    size_t name_length;
    /* Where the folder part ends; the whole name minus its '/' for a folder entry. */
    size_t folder_length;
    int is_folder;
    uint16_t flags;
    uint16_t method;
    uint32_t crc;
    uint32_t compressed;
    uint32_t size;
    uint32_t local_offset;
    /* Where the data starts, from the local header (step 2). */
    uint32_t data_offset;
} zip_entry;

typedef struct reader {
    skiff_storage *storage;
    const skiff_storage_roots *roots;
    skiff_saves_unit *unit;
    const char *staging;
    char unit_path[SKIFF_STORAGE_PATH_MAX];
    char out_path[SKIFF_STORAGE_PATH_MAX];
    uint64_t unit_bytes;
    uint32_t directory_offset;
    uint32_t directory_bytes;
    size_t entry_count;
    zip_entry entries[ENTRIES_MAX];
    /* Whether each of unit->folders had a folder entry, to refuse a second one. */
    unsigned char folder_entry_seen[SKIFF_SAVES_FOLDERS_MAX];
    /* Files extracted so far (unit->files[0..extracted)) and whether the folders were created,
     * for undoing a refused extraction. */
    size_t extracted;
    int folders_created;
    z_stream inflater;
    int inflater_ready;
    unsigned char header[ZIP_CENTRAL_BYTES];
    unsigned char in[SAVES_BUFFER_BYTES];
    unsigned char out[SAVES_BUFFER_BYTES];
} reader;

/* A refusal: the problem is recorded, and every step stops on it. */
static skiff_err refuse(reader *r, skiff_saves_problem problem, const zip_entry *entry) {
    if (entry != NULL) {
        saves_refuse(r->unit, problem, entry->name, entry->name_length);
    } else {
        saves_refuse(r->unit, problem, "", 0);
    }
    return SKIFF_ERR_STORAGE_SAVE_DAMAGED;
}

static skiff_err open_at(reader *r, uint64_t offset, skiff_file **out) {
    return skiff_storage_open(r->storage, r->unit_path, SKIFF_FILE_READ, offset, out);
}

/* Reads up to size bytes, fewer only at the end of the file (*got says how many). */
static skiff_err read_fully(skiff_file *file, void *buffer, size_t size, size_t *got) {
    unsigned char *bytes = buffer;
    *got = 0;
    size_t step = 1;
    skiff_err err = SKIFF_OK;
    while (err == SKIFF_OK && step > 0 && *got < size) {
        err = skiff_file_read(file, bytes + *got, size - *got, &step);
        *got += err == SKIFF_OK ? step : 0;
    }
    return err;
}

/* Reads exactly size bytes, or refuses the unit as cut short. */
static skiff_err read_exact(reader *r, skiff_file *file, void *buffer, size_t size) {
    size_t got = 0;
    const skiff_err err = read_fully(file, buffer, size, &got);
    if (err != SKIFF_OK) {
        return err;
    }
    return got == size ? SKIFF_OK : refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, NULL);
}

static skiff_err skip(reader *r, skiff_file *file, size_t size) {
    skiff_err err = SKIFF_OK;
    while (err == SKIFF_OK && size > 0) {
        const size_t step = size < sizeof r->in ? size : sizeof r->in;
        err = read_exact(r, file, r->in, step);
        size -= step;
    }
    return err;
}

static skiff_err close_after(skiff_file *file, skiff_err err) {
    const skiff_err close_err = skiff_file_close(file);
    return err != SKIFF_OK ? err : close_err;
}

/* ---- Step 1: the end record and the central directory ---- */

static skiff_err read_end(reader *r) {
    if (r->unit_bytes < ZIP_END_BYTES) {
        return refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, NULL);
    }
    if (r->unit_bytes > SKIFF_SAVES_UNIT_FILE_BYTES_MAX) {
        return refuse(r, SKIFF_SAVES_PROBLEM_TOO_LARGE, NULL);
    }
    skiff_file *file = NULL;
    skiff_err err = open_at(r, r->unit_bytes - ZIP_END_BYTES, &file);
    if (err != SKIFF_OK) {
        return err;
    }
    unsigned char *end = r->header;
    err = close_after(file, read_exact(r, file, end, ZIP_END_BYTES));
    if (err != SKIFF_OK) {
        return err;
    }
    /* No comment: the end record must be the last 22 bytes. */
    if (saves_read_le32(end) != ZIP_END_SIGNATURE ||
        saves_read_le16(end + ZIP_END_COMMENT_LENGTH) != 0) {
        return refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, NULL);
    }
    const uint16_t disk_entries = saves_read_le16(end + ZIP_END_DISK_ENTRIES);
    const uint16_t entries = saves_read_le16(end + ZIP_END_ENTRIES);
    r->directory_bytes = saves_read_le32(end + ZIP_END_DIRECTORY_SIZE);
    r->directory_offset = saves_read_le32(end + ZIP_END_DIRECTORY_OFFSET);
    if (entries == ZIP_ZIP64_U16 || r->directory_bytes == ZIP_ZIP64_U32 ||
        r->directory_offset == ZIP_ZIP64_U32) {
        return refuse(r, SKIFF_SAVES_PROBLEM_ZIP64, NULL);
    }
    if (saves_read_le16(end + ZIP_END_DISK) != 0 ||
        saves_read_le16(end + ZIP_END_DIRECTORY_DISK) != 0 || disk_entries != entries) {
        return refuse(r, SKIFF_SAVES_PROBLEM_MULTI_DISK, NULL);
    }
    if ((uint64_t)r->directory_offset + r->directory_bytes != r->unit_bytes - ZIP_END_BYTES ||
        (uint64_t)entries * ZIP_CENTRAL_BYTES > r->directory_bytes) {
        return refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, NULL);
    }
    if (entries > ENTRIES_MAX) {
        return refuse(r, SKIFF_SAVES_PROBLEM_TOO_MANY_FILES, NULL);
    }
    r->entry_count = entries;
    return SKIFF_OK;
}

static int is_part(const char *part, size_t length, const char *word) {
    return length == strlen(word) && memcmp(part, word, length) == 0;
}

/* Checks an entry's name (§7 of the design: what a unit may hold) and finds its folder part. */
static skiff_saves_problem check_name(const reader *r, zip_entry *entry) {
    const char *name = entry->name;
    const size_t length = entry->name_length;
    for (size_t i = 0; i < length; i++) {
        const unsigned char c = (unsigned char)name[i];
        if (c < PRINTABLE_FIRST || c > PRINTABLE_LAST) {
            return SKIFF_SAVES_PROBLEM_NAME;
        }
    }
    if (name[0] == SAVES_PATH_SEPARATOR || memchr(name, WINDOWS_SEPARATOR, length) != NULL ||
        memchr(name, DOS_DEVICE_SEPARATOR, length) != NULL) {
        return SKIFF_SAVES_PROBLEM_PATH;
    }
    entry->is_folder = name[length - 1] == SAVES_PATH_SEPARATOR;
    const size_t body = entry->is_folder ? length - 1 : length;
    size_t parts = 0;
    size_t start = 0;
    size_t first_end = body;
    for (size_t i = 0; i <= body; i++) {
        if (i < body && name[i] != SAVES_PATH_SEPARATOR) {
            continue;
        }
        const size_t part_length = i - start;
        if (part_length == 0 || is_part(name + start, part_length, CURRENT_FOLDER) ||
            is_part(name + start, part_length, PARENT_FOLDER)) {
            return SKIFF_SAVES_PROBLEM_PATH;
        }
        if (parts == 0) {
            first_end = i;
        }
        parts++;
        start = i + 1;
    }
    if (parts > (entry->is_folder ? 1U : 2U)) {
        return SKIFF_SAVES_PROBLEM_NESTED_FOLDER;
    }
    if (!entry->is_folder && parts < 2) {
        return SKIFF_SAVES_PROBLEM_PATH;
    }
    entry->folder_length = first_end;
    if (!saves_name_valid(name, first_end) ||
        (!entry->is_folder && !saves_name_valid(name + first_end + 1, body - first_end - 1))) {
        return SKIFF_SAVES_PROBLEM_NAME;
    }
    char folder[SKIFF_SAVES_NAME_SIZE];
    memcpy(folder, name, first_end);
    folder[first_end] = '\0';
    return saves_has_prefix(folder, r->unit->save_target) ? SKIFF_SAVES_PROBLEM_NONE
                                                          : SKIFF_SAVES_PROBLEM_OTHER_GAME;
}

/* Skips an extra field, refusing one that carries ZIP64 sizes. */
static skiff_err skip_extra(reader *r, skiff_file *file, size_t length, const zip_entry *entry) {
    while (length >= ZIP_EXTRA_HEADER_BYTES) {
        unsigned char header[ZIP_EXTRA_HEADER_BYTES];
        skiff_err err = read_exact(r, file, header, sizeof header);
        if (err != SKIFF_OK) {
            return err;
        }
        length -= sizeof header;
        const uint16_t data_length = saves_read_le16(header + ZIP_EXTRA_LENGTH);
        if (saves_read_le16(header + ZIP_EXTRA_ID) == ZIP_EXTRA_ZIP64) {
            return refuse(r, SKIFF_SAVES_PROBLEM_ZIP64, entry);
        }
        if (data_length > length) {
            return refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, entry);
        }
        err = skip(r, file, data_length);
        if (err != SKIFF_OK) {
            return err;
        }
        length -= data_length;
    }
    /* A few bytes too short for a field: padding some writers leave. */
    return skip(r, file, length);
}

/* The folder part of entry in unit->folders, added if new; refuses one spelled two ways. */
static skiff_err note_folder(reader *r, const zip_entry *entry) {
    skiff_saves_unit *unit = r->unit;
    size_t i = 0;
    while (i < unit->folder_count && !saves_same_name(unit->folders[i], strlen(unit->folders[i]),
                                                      entry->name, entry->folder_length)) {
        i++;
    }
    if (i < unit->folder_count) {
        if (memcmp(unit->folders[i], entry->name, entry->folder_length) != 0 ||
            (entry->is_folder && r->folder_entry_seen[i])) {
            return refuse(r, SKIFF_SAVES_PROBLEM_DUPLICATE, entry);
        }
    } else if (unit->folder_count == SKIFF_SAVES_FOLDERS_MAX) {
        return refuse(r, SKIFF_SAVES_PROBLEM_TOO_MANY_FOLDERS, entry);
    } else {
        memcpy(unit->folders[i], entry->name, entry->folder_length);
        unit->folders[i][entry->folder_length] = '\0';
        unit->folder_count++;
    }
    r->folder_entry_seen[i] = (unsigned char)(r->folder_entry_seen[i] || entry->is_folder);
    return SKIFF_OK;
}

/* A file entry against the files before it (FAT folds case), the file cap and the size cap. */
static skiff_err note_file(reader *r, const zip_entry *entry, size_t index, size_t *files,
                           uint64_t *total) {
    for (size_t i = 0; i < index; i++) {
        const zip_entry *other = &r->entries[i];
        if (!other->is_folder &&
            saves_same_name(other->name, other->name_length, entry->name, entry->name_length)) {
            return refuse(r, SKIFF_SAVES_PROBLEM_DUPLICATE, entry);
        }
    }
    if (++*files > SKIFF_SAVES_FILES_MAX) {
        return refuse(r, SKIFF_SAVES_PROBLEM_TOO_MANY_FILES, entry);
    }
    *total += entry->size;
    return *total > SKIFF_SAVES_UNIT_BYTES_MAX ? refuse(r, SKIFF_SAVES_PROBLEM_TOO_LARGE, entry)
                                               : SKIFF_OK;
}

/* The fixed part of a central directory record, checked; the name follows it. */
static skiff_err check_central(reader *r, zip_entry *entry) {
    const unsigned char *h = r->header;
    entry->flags = saves_read_le16(h + ZIP_CENTRAL_FLAGS);
    entry->method = saves_read_le16(h + ZIP_CENTRAL_METHOD);
    entry->crc = saves_read_le32(h + ZIP_CENTRAL_CRC);
    entry->compressed = saves_read_le32(h + ZIP_CENTRAL_COMPRESSED);
    entry->size = saves_read_le32(h + ZIP_CENTRAL_SIZE);
    entry->local_offset = saves_read_le32(h + ZIP_CENTRAL_OFFSET);
    if (saves_read_le16(h + ZIP_CENTRAL_DISK) != 0) {
        return refuse(r, SKIFF_SAVES_PROBLEM_MULTI_DISK, entry);
    }
    if (entry->flags &
        (ZIP_FLAG_ENCRYPTED | ZIP_FLAG_STRONG_ENCRYPTION | ZIP_FLAG_ENCRYPTED_DIRECTORY)) {
        return refuse(r, SKIFF_SAVES_PROBLEM_ENCRYPTED, entry);
    }
    if (entry->compressed == ZIP_ZIP64_U32 || entry->size == ZIP_ZIP64_U32 ||
        entry->local_offset == ZIP_ZIP64_U32) {
        return refuse(r, SKIFF_SAVES_PROBLEM_ZIP64, entry);
    }
    if (entry->method != ZIP_METHOD_STORED && entry->method != ZIP_METHOD_DEFLATE) {
        return refuse(r, SKIFF_SAVES_PROBLEM_METHOD, entry);
    }
    return SKIFF_OK;
}

static skiff_err read_entry(reader *r, skiff_file *file, size_t index, size_t *files,
                            uint64_t *total, uint64_t *consumed) {
    zip_entry *entry = &r->entries[index];
    skiff_err err = read_exact(r, file, r->header, ZIP_CENTRAL_BYTES);
    if (err == SKIFF_OK && saves_read_le32(r->header) != ZIP_CENTRAL_SIGNATURE) {
        err = refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, NULL);
    }
    if (err != SKIFF_OK) {
        return err;
    }
    const size_t name_length = saves_read_le16(r->header + ZIP_CENTRAL_NAME_LENGTH);
    const size_t extra_length = saves_read_le16(r->header + ZIP_CENTRAL_EXTRA_LENGTH);
    const size_t comment_length = saves_read_le16(r->header + ZIP_CENTRAL_COMMENT_LENGTH);
    *consumed += ZIP_CENTRAL_BYTES + name_length + extra_length + comment_length;
    entry->name_length = name_length < sizeof entry->name ? name_length : sizeof entry->name - 1;
    err = read_exact(r, file, entry->name, entry->name_length);
    entry->name[entry->name_length] = '\0';
    if (err == SKIFF_OK) {
        err = check_central(r, entry);
    }
    if (err == SKIFF_OK && name_length == 0) {
        err = refuse(r, SKIFF_SAVES_PROBLEM_PATH, entry);
    }
    if (err == SKIFF_OK && name_length >= sizeof entry->name) {
        err = refuse(r, SKIFF_SAVES_PROBLEM_NAME, entry);
    }
    if (err == SKIFF_OK) {
        err = skip_extra(r, file, extra_length, entry);
    }
    if (err == SKIFF_OK) {
        err = skip(r, file, comment_length);
    }
    const skiff_saves_problem problem =
        err == SKIFF_OK ? check_name(r, entry) : SKIFF_SAVES_PROBLEM_NONE;
    if (problem != SKIFF_SAVES_PROBLEM_NONE) {
        return refuse(r, problem, entry);
    }
    if (err == SKIFF_OK && entry->method == ZIP_METHOD_STORED && entry->compressed != entry->size) {
        err = refuse(r, SKIFF_SAVES_PROBLEM_SIZE, entry);
    }
    if (err == SKIFF_OK && entry->is_folder && entry->size != 0) {
        err = refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, entry);
    }
    if (err == SKIFF_OK) {
        err = note_folder(r, entry);
    }
    if (err == SKIFF_OK && !entry->is_folder) {
        err = note_file(r, entry, index, files, total);
    }
    return err;
}

static skiff_err read_directory(reader *r) {
    skiff_file *file = NULL;
    skiff_err err = open_at(r, r->directory_offset, &file);
    if (err != SKIFF_OK) {
        return err;
    }
    size_t files = 0;
    uint64_t total = 0;
    uint64_t consumed = 0;
    for (size_t i = 0; err == SKIFF_OK && i < r->entry_count; i++) {
        err = read_entry(r, file, i, &files, &total, &consumed);
        if (err == SKIFF_OK && consumed > r->directory_bytes) {
            err = refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, NULL);
        }
    }
    if (err == SKIFF_OK && consumed != r->directory_bytes) {
        err = refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, NULL);
    }
    return close_after(file, err);
}

/* ---- Step 2: local headers and the layout ---- */

/* A local header field matches the directory's; with a data descriptor it may also be 0. */
static int agrees(uint32_t local, uint32_t central, int descriptor) {
    return local == central || (descriptor && local == 0);
}

static int descriptor_matches(const unsigned char *fields, const zip_entry *entry) {
    return saves_read_le32(fields + ZIP_DESCRIPTOR_CRC) == entry->crc &&
           saves_read_le32(fields + ZIP_DESCRIPTOR_COMPRESSED) == entry->compressed &&
           saves_read_le32(fields + ZIP_DESCRIPTOR_SIZE) == entry->size;
}

/* The data descriptor after entry's data, matching the directory: its length into *length. */
static skiff_err check_descriptor(reader *r, const zip_entry *entry, uint64_t at, size_t *length) {
    skiff_file *file = NULL;
    skiff_err err = open_at(r, at, &file);
    if (err != SKIFF_OK) {
        return err;
    }
    unsigned char d[ZIP_DESCRIPTOR_SIGNED_BYTES];
    size_t got = 0;
    err = close_after(file, read_fully(file, d, sizeof d, &got));
    if (err != SKIFF_OK) {
        return err;
    }
    const int signed_form = got == sizeof d && saves_read_le32(d) == ZIP_DESCRIPTOR_SIGNATURE &&
                            descriptor_matches(d + ZIP_SIGNATURE_BYTES, entry);
    const int plain_form = got >= ZIP_DESCRIPTOR_BYTES && descriptor_matches(d, entry);
    if (!signed_form && !plain_form) {
        return refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, entry);
    }
    *length = signed_form ? ZIP_DESCRIPTOR_SIGNED_BYTES : ZIP_DESCRIPTOR_BYTES;
    return SKIFF_OK;
}

static skiff_err check_local(reader *r, zip_entry *entry, uint64_t *next) {
    if (entry->local_offset != *next) {
        return refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, entry);
    }
    skiff_file *file = NULL;
    skiff_err err = open_at(r, entry->local_offset, &file);
    if (err != SKIFF_OK) {
        return err;
    }
    const unsigned char *h = r->header;
    err = read_exact(r, file, r->header, ZIP_LOCAL_BYTES);
    char name[SKIFF_SAVES_ENTRY_SIZE] = {0};
    const size_t name_length = err == SKIFF_OK ? saves_read_le16(h + ZIP_LOCAL_NAME_LENGTH) : 0;
    const size_t extra_length = err == SKIFF_OK ? saves_read_le16(h + ZIP_LOCAL_EXTRA_LENGTH) : 0;
    if (err == SKIFF_OK && name_length == entry->name_length) {
        err = read_exact(r, file, name, name_length);
        /* Held to the directory's rules too: no ZIP64 field, no field running past its end. */
        if (err == SKIFF_OK) {
            err = skip_extra(r, file, extra_length, entry);
        }
    }
    err = close_after(file, err);
    if (err != SKIFF_OK) {
        return err;
    }
    const int descriptor = (entry->flags & ZIP_FLAG_DESCRIPTOR) != 0;
    const uint32_t compressed = saves_read_le32(h + ZIP_LOCAL_COMPRESSED);
    const uint32_t size = saves_read_le32(h + ZIP_LOCAL_SIZE);
    if (compressed == ZIP_ZIP64_U32 || size == ZIP_ZIP64_U32) {
        return refuse(r, SKIFF_SAVES_PROBLEM_ZIP64, entry);
    }
    if (saves_read_le32(h) != ZIP_LOCAL_SIGNATURE ||
        saves_read_le16(h + ZIP_LOCAL_FLAGS) != entry->flags ||
        saves_read_le16(h + ZIP_LOCAL_METHOD) != entry->method ||
        !agrees(saves_read_le32(h + ZIP_LOCAL_CRC), entry->crc, descriptor) ||
        !agrees(compressed, entry->compressed, descriptor) ||
        !agrees(size, entry->size, descriptor) || name_length != entry->name_length ||
        memcmp(name, entry->name, name_length) != 0) {
        return refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, entry);
    }
    const uint64_t data =
        (uint64_t)entry->local_offset + ZIP_LOCAL_BYTES + name_length + extra_length;
    uint64_t end = data + entry->compressed;
    if (end > r->directory_offset) {
        return refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, entry);
    }
    if (descriptor) {
        size_t length = 0;
        err = check_descriptor(r, entry, end, &length);
        if (err != SKIFF_OK) {
            return err;
        }
        end += length;
    }
    if (end > r->directory_offset) {
        return refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, entry);
    }
    entry->data_offset = (uint32_t)data;
    *next = end;
    return SKIFF_OK;
}

static skiff_err check_layout(reader *r) {
    uint64_t next = 0;
    skiff_err err = SKIFF_OK;
    for (size_t i = 0; err == SKIFF_OK && i < r->entry_count; i++) {
        err = check_local(r, &r->entries[i], &next);
    }
    if (err == SKIFF_OK && next != r->directory_offset) {
        err = refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, NULL);
    }
    return err;
}

/* ---- Step 3: the data ---- */

/* Where a file's bytes go while it is read: its CRC-32, its MD5, and the staged copy. */
typedef struct sink {
    uLong crc;
    skiff_md5 md5;
    skiff_file *out;
    uint64_t produced;
} sink;

static skiff_err take(sink *s, const unsigned char *bytes, size_t length) {
    s->crc = crc32(s->crc, bytes, (uInt)length);
    skiff_md5_update(&s->md5, bytes, length);
    s->produced += length;
    return s->out != NULL ? skiff_file_write(s->out, bytes, length) : SKIFF_OK;
}

static skiff_err copy_stored(reader *r, const zip_entry *entry, skiff_file *in, sink *s) {
    uint32_t remaining = entry->compressed;
    skiff_err err = SKIFF_OK;
    while (err == SKIFF_OK && remaining > 0) {
        const size_t step = remaining < sizeof r->in ? remaining : sizeof r->in;
        err = read_exact(r, in, r->in, step);
        if (err == SKIFF_OK) {
            err = take(s, r->in, step);
        }
        remaining -= (uint32_t)step;
    }
    return err;
}

static skiff_err start_inflater(reader *r) {
    if (r->inflater_ready) {
        return inflateReset(&r->inflater) == Z_OK ? SKIFF_OK : SKIFF_ERR_NO_MEMORY;
    }
    memset(&r->inflater, 0, sizeof r->inflater);
    /* Negative window bits: raw deflate, as zip entries hold it (no zlib header). */
    if (inflateInit2(&r->inflater, -MAX_WBITS) != Z_OK) {
        return SKIFF_ERR_NO_MEMORY;
    }
    r->inflater_ready = 1;
    return SKIFF_OK;
}

/* One inflate call's outcome: keep going, the stream ended, or a refusal or error in *err. */
typedef enum inflate_step { INFLATE_MORE, INFLATE_END, INFLATE_FAILED } inflate_step;

static inflate_step inflate_once(reader *r, const zip_entry *entry, sink *s, skiff_err *err) {
    z_stream *z = &r->inflater;
    /* One byte past the declared size is allowed out, so a stream that holds more is caught. */
    const uint64_t left = entry->size - s->produced + 1;
    const uInt room = (uInt)(left < sizeof r->out ? left : sizeof r->out);
    z->next_out = r->out;
    z->avail_out = room;
    const int status = inflate(z, Z_NO_FLUSH);
    const size_t made = room - z->avail_out;
    if (s->produced + made > entry->size) {
        *err = refuse(r, SKIFF_SAVES_PROBLEM_SIZE, entry);
        return INFLATE_FAILED;
    }
    *err = take(s, r->out, made);
    if (*err != SKIFF_OK) {
        return INFLATE_FAILED;
    }
    if (status == Z_STREAM_END) {
        return INFLATE_END;
    }
    if (status == Z_OK) {
        return INFLATE_MORE;
    }
    *err = status == Z_MEM_ERROR ? SKIFF_ERR_NO_MEMORY
                                 : refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, entry);
    return INFLATE_FAILED;
}

static skiff_err copy_deflated(reader *r, const zip_entry *entry, skiff_file *in, sink *s) {
    skiff_err err = start_inflater(r);
    if (err != SKIFF_OK) {
        return err;
    }
    z_stream *z = &r->inflater;
    z->avail_in = 0;
    uint32_t remaining = entry->compressed;
    inflate_step step = INFLATE_MORE;
    while (step == INFLATE_MORE) {
        if (z->avail_in == 0 && remaining > 0) {
            const size_t chunk = remaining < sizeof r->in ? remaining : sizeof r->in;
            err = read_exact(r, in, r->in, chunk);
            if (err != SKIFF_OK) {
                return err;
            }
            z->next_in = r->in;
            z->avail_in = (uInt)chunk;
            remaining -= (uint32_t)chunk;
        }
        step = inflate_once(r, entry, s, &err);
    }
    if (step == INFLATE_FAILED) {
        return err;
    }
    if (s->produced != entry->size) {
        return refuse(r, SKIFF_SAVES_PROBLEM_SIZE, entry);
    }
    return z->avail_in != 0 || remaining != 0 ? refuse(r, SKIFF_SAVES_PROBLEM_STRUCTURE, entry)
                                              : SKIFF_OK;
}

/* "<staging>/<FOLDER>[/<FILE>]" for entry, resolved. */
static skiff_err staged_path(reader *r, const char *folder, const char *file) {
    return saves_resolve(r->roots, r->staging, folder, file, r->out_path, sizeof r->out_path);
}

static skiff_err create_folders(reader *r) {
    char root[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = skiff_storage_resolve(r->roots, r->staging, root, sizeof root);
    if (err == SKIFF_OK) {
        err = skiff_storage_mkdirs(r->storage, root);
    }
    r->folders_created = 1;
    for (size_t i = 0; err == SKIFF_OK && i < r->unit->folder_count; i++) {
        err = staged_path(r, r->unit->folders[i], NULL);
        if (err == SKIFF_OK) {
            err = skiff_storage_mkdir(r->storage, r->out_path);
        }
    }
    return err;
}

static skiff_err open_staged(reader *r, const zip_entry *entry, skiff_file **out) {
    char folder[SKIFF_SAVES_NAME_SIZE];
    memcpy(folder, entry->name, entry->folder_length);
    folder[entry->folder_length] = '\0';
    skiff_err err = staged_path(r, folder, entry->name + entry->folder_length + 1);
    if (err == SKIFF_OK) {
        err = skiff_storage_open(r->storage, r->out_path, SKIFF_FILE_REPLACE, 0, out);
    }
    return err;
}

static skiff_err read_data(reader *r, const zip_entry *entry) {
    skiff_saves_unit *unit = r->unit;
    sink s;
    memset(&s, 0, sizeof s);
    s.crc = crc32(0L, Z_NULL, 0);
    skiff_md5_init(&s.md5);
    skiff_file *in = NULL;
    skiff_err err = open_at(r, entry->data_offset, &in);
    if (err == SKIFF_OK && r->staging != NULL) {
        err = open_staged(r, entry, &s.out);
        /* Counted from the moment it exists, so a refusal removes it too. */
        r->extracted += err == SKIFF_OK ? 1 : 0;
    }
    if (err == SKIFF_OK) {
        err = entry->method == ZIP_METHOD_STORED ? copy_stored(r, entry, in, &s)
                                                 : copy_deflated(r, entry, in, &s);
    }
    if (err == SKIFF_OK && (uint32_t)s.crc != entry->crc) {
        err = refuse(r, SKIFF_SAVES_PROBLEM_CRC, entry);
    }
    if (err == SKIFF_OK && s.out != NULL) {
        err = skiff_file_sync(s.out);
    }
    err = close_after(s.out, err);
    err = close_after(in, err);
    if (err != SKIFF_OK) {
        return err;
    }
    skiff_saves_file *file = &unit->files[unit->file_count++];
    memcpy(file->name, entry->name, entry->name_length + 1);
    file->size = entry->size;
    file->mtime = SKIFF_STORAGE_MTIME_UNKNOWN;
    unsigned char digest[SKIFF_MD5_DIGEST_BYTES];
    skiff_md5_final(&s.md5, digest);
    skiff_md5_hex(digest, file->md5);
    unit->total_bytes += entry->size;
    return SKIFF_OK;
}

static skiff_err read_all_data(reader *r) {
    skiff_err err = r->staging != NULL ? create_folders(r) : SKIFF_OK;
    for (size_t i = 0; err == SKIFF_OK && i < r->entry_count; i++) {
        if (!r->entries[i].is_folder) {
            err = read_data(r, &r->entries[i]);
        }
    }
    return err;
}

/* Removes what a refused or failed extraction wrote: its files, then its emptied folders. */
static void undo_extraction(reader *r) {
    size_t removed = 0;
    for (size_t i = 0; i < r->entry_count && removed < r->extracted; i++) {
        const zip_entry *entry = &r->entries[i];
        if (entry->is_folder) {
            continue;
        }
        char folder[SKIFF_SAVES_NAME_SIZE];
        memcpy(folder, entry->name, entry->folder_length);
        folder[entry->folder_length] = '\0';
        if (staged_path(r, folder, entry->name + entry->folder_length + 1) == SKIFF_OK) {
            (void)skiff_storage_remove(r->storage, r->out_path);
        }
        removed++;
    }
    for (size_t i = 0; r->folders_created && i < r->unit->folder_count; i++) {
        if (staged_path(r, r->unit->folders[i], NULL) == SKIFF_OK) {
            (void)skiff_storage_rmdir(r->storage, r->out_path);
        }
    }
}

static skiff_err read_unit(reader *r) {
    skiff_err err = skiff_storage_size(r->storage, r->unit_path, &r->unit_bytes);
    if (err == SKIFF_OK) {
        err = read_end(r);
    }
    if (err == SKIFF_OK) {
        err = read_directory(r);
    }
    if (err == SKIFF_OK) {
        err = check_layout(r);
    }
    if (err == SKIFF_OK) {
        err = read_all_data(r);
    }
    if (err != SKIFF_OK && r->staging != NULL) {
        undo_extraction(r);
    }
    if (r->inflater_ready) {
        inflateEnd(&r->inflater);
    }
    return err;
}

static void reset_unit(skiff_saves_unit *unit, const char *save_target) {
    const skiff_saves_problem problem = unit->problem;
    char name[SKIFF_SAVES_ENTRY_SIZE];
    memcpy(name, unit->problem_name, sizeof name);
    memset(unit, 0, sizeof *unit);
    memcpy(unit->save_target, save_target, strlen(save_target) + 1);
    unit->problem = problem;
    memcpy(unit->problem_name, name, sizeof name);
}

skiff_err skiff_saves_read_unit(skiff_storage *storage, const skiff_storage_roots *roots,
                                const char *unit_path, const char *save_target, const char *staging,
                                skiff_saves_unit *unit) {
    if (storage == NULL || roots == NULL || unit_path == NULL || unit == NULL ||
        !skiff_saves_target_valid(save_target) ||
        (staging != NULL && strlen(staging) >= sizeof unit->root)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    unit->problem = SKIFF_SAVES_PROBLEM_NONE;
    unit->problem_name[0] = '\0';
    reset_unit(unit, save_target);
    reader *r = calloc(1, sizeof *r);
    if (r == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    r->storage = storage;
    r->roots = roots;
    r->unit = unit;
    r->staging = staging;
    skiff_err err = skiff_storage_resolve(roots, unit_path, r->unit_path, sizeof r->unit_path);
    if (err == SKIFF_OK) {
        err = read_unit(r);
    }
    free(r);
    if (err != SKIFF_OK) {
        reset_unit(unit, save_target);
        return err;
    }
    saves_sort(unit);
    saves_content_hash(unit);
    if (staging != NULL) {
        memcpy(unit->root, staging, strlen(staging) + 1);
    }
    return SKIFF_OK;
}
