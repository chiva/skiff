#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "skiff/cover.h"

/*
 * A slot file: a header, then height rows of SKIFF_COVER_WIDTH pixels, little-endian. The header's
 * last field is the CRC-32 of the header before it and of the pixels.
 */
#define FILE_MAGIC_BYTES 4
static const unsigned char FILE_MAGIC[FILE_MAGIC_BYTES] = {'S', 'K', 'C', 'V'};
/* Bumped when the pixel format or the layout changes; the box size is in the header too. */
#define FILE_VERSION 1
#define AT_VERSION 4
#define AT_WIDTH 6
#define AT_HEIGHT 8
#define AT_BOX_WIDTH 10
#define AT_BOX_HEIGHT 12
#define AT_ROM_ID 16
#define AT_SERVER 24
#define AT_COVER_PATH 28
#define AT_CRC 32
#define HEADER_BYTES 36
#define PIXEL_BYTES 2
#define BYTE_BITS 8
#define BYTE_MASK 0xFFU
/* "app:/covers/" and a slot number. */
#define SLOT_NAME_MAX 32

static void put16(unsigned char *at, uint32_t value) {
    for (int i = 0; i < 2; i++) {
        at[i] = (unsigned char)((value >> (BYTE_BITS * i)) & BYTE_MASK);
    }
}

static void put32(unsigned char *at, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        at[i] = (unsigned char)((value >> (BYTE_BITS * i)) & BYTE_MASK);
    }
}

static void put64(unsigned char *at, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        at[i] = (unsigned char)((value >> (BYTE_BITS * i)) & BYTE_MASK);
    }
}

static uint32_t get16(const unsigned char *at) { return (uint32_t)at[0] | (uint32_t)at[1] << 8; }

static uint32_t get32(const unsigned char *at) {
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static uint64_t get64(const unsigned char *at) {
    return (uint64_t)get32(at) | (uint64_t)get32(at + 4) << 32;
}

static uint32_t text_crc(const char *text) {
    return (uint32_t)crc32(0, (const Bytef *)text, (uInt)strlen(text));
}

skiff_cover_key skiff_cover_key_of(const char *base_url, uint64_t rom_id, const char *cover_path) {
    skiff_cover_key key = {rom_id, 0, 0};
    key.server = base_url == NULL ? 0 : text_crc(base_url);
    key.cover_path = cover_path == NULL ? 0 : text_crc(cover_path);
    return key;
}

/* The real path of rom_id's slot file, and of the folder that holds it. */
static skiff_err slot_paths(const skiff_storage_roots *roots, uint64_t rom_id, char *slot,
                            char *folder) {
    char logical[SLOT_NAME_MAX];
    snprintf(logical, sizeof logical, SKIFF_COVER_CACHE_FOLDER "/%u" SKIFF_COVER_CACHE_EXTENSION,
             (unsigned)(rom_id % SKIFF_COVER_CACHE_SLOTS));
    skiff_err err = skiff_storage_resolve(roots, logical, slot, SKIFF_STORAGE_PATH_MAX);
    if (err == SKIFF_OK && folder != NULL) {
        err =
            skiff_storage_resolve(roots, SKIFF_COVER_CACHE_FOLDER, folder, SKIFF_STORAGE_PATH_MAX);
    }
    return err;
}

static size_t pixel_bytes(uint32_t height) {
    return (size_t)height * SKIFF_COVER_WIDTH * PIXEL_BYTES;
}

/* The CRC-32 a file must end its header with: of the header before it, then of the pixels. */
static uint32_t file_crc(const unsigned char *header, const unsigned char *pixels, size_t size) {
    const uLong crc = crc32(0, header, AT_CRC);
    return (uint32_t)crc32(crc, pixels, (uInt)size);
}

/* ---- Loading ---- */

/* Reads exactly size bytes, or reports how the file fell short. */
static skiff_err read_exactly(skiff_file *file, unsigned char *out, size_t size) {
    size_t done = 0;
    while (done < size) {
        size_t got = 0;
        const skiff_err err = skiff_file_read(file, out + done, size - done, &got);
        if (err != SKIFF_OK) {
            return err;
        }
        if (got == 0) {
            return SKIFF_ERR_ROMM_COVER_DAMAGED;
        }
        done += got;
    }
    return SKIFF_OK;
}

/* A header this Skiff wrote for the current box: SKIFF_ERR_ROMM_COVER_DAMAGED otherwise. Then
 * whether it names key's cover: SKIFF_ERR_STORAGE_NOT_FOUND otherwise. */
static skiff_err check_header(const unsigned char *header, const skiff_cover_key *key) {
    const uint32_t width = get16(header + AT_WIDTH);
    const uint32_t height = get16(header + AT_HEIGHT);
    if (memcmp(header, FILE_MAGIC, FILE_MAGIC_BYTES) != 0 ||
        get16(header + AT_VERSION) != FILE_VERSION ||
        get16(header + AT_BOX_WIDTH) != SKIFF_COVER_WIDTH ||
        get16(header + AT_BOX_HEIGHT) != SKIFF_COVER_HEIGHT || width == 0 ||
        width > SKIFF_COVER_WIDTH || height == 0 || height > SKIFF_COVER_HEIGHT) {
        return SKIFF_ERR_ROMM_COVER_DAMAGED;
    }
    return get64(header + AT_ROM_ID) == key->rom_id && get32(header + AT_SERVER) == key->server &&
                   get32(header + AT_COVER_PATH) == key->cover_path
               ? SKIFF_OK
               : SKIFF_ERR_STORAGE_NOT_FOUND;
}

/* Reads the pixels after a checked header into out, straight into its buffer, and checks the CRC
 * and that nothing follows them. */
static skiff_err read_pixels(skiff_file *file, const unsigned char *header, skiff_cover *out) {
    const uint32_t height = get16(header + AT_HEIGHT);
    unsigned char *bytes = (unsigned char *)out->pixels;
    const size_t size = pixel_bytes(height);
    skiff_err err = read_exactly(file, bytes, size);
    if (err != SKIFF_OK) {
        return err;
    }
    unsigned char extra = 0;
    size_t got = 0;
    err = skiff_file_read(file, &extra, 1, &got);
    if (err != SKIFF_OK) {
        return err;
    }
    if (got != 0 || file_crc(header, bytes, size) != get32(header + AT_CRC)) {
        return SKIFF_ERR_ROMM_COVER_DAMAGED;
    }
    /* Little-endian bytes into pixels, in place: pixel i is read from the two bytes it occupies. */
    for (size_t i = 0; i < size / PIXEL_BYTES; i++) {
        out->pixels[i] = (uint16_t)get16(bytes + i * PIXEL_BYTES);
    }
    out->width = (uint16_t)get16(header + AT_WIDTH);
    out->height = (uint16_t)height;
    return SKIFF_OK;
}

skiff_err skiff_cover_cache_load(skiff_storage *storage, const skiff_storage_roots *roots,
                                 const skiff_cover_key *key, skiff_cover *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (storage == NULL || roots == NULL || key == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    char slot[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = slot_paths(roots, key->rom_id, slot, NULL);
    skiff_file *file = NULL;
    if (err == SKIFF_OK) {
        err = skiff_storage_open(storage, slot, SKIFF_FILE_READ, 0, &file);
    }
    unsigned char header[HEADER_BYTES];
    if (err == SKIFF_OK) {
        err = read_exactly(file, header, sizeof header);
    }
    if (err == SKIFF_OK) {
        err = check_header(header, key);
    }
    if (err == SKIFF_OK) {
        err = read_pixels(file, header, out);
    }
    const skiff_err closed = skiff_file_close(file);
    if (err == SKIFF_OK) {
        err = closed;
    }
    if (err != SKIFF_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}

/* ---- Storing ---- */

/* Room for a slot file that does not exist yet; replacing an existing one never needs more. */
static skiff_err check_room(skiff_storage *storage, const char *slot, const char *folder,
                            size_t file_size) {
    uint64_t existing = 0;
    skiff_err err = skiff_storage_size(storage, slot, &existing);
    if (err != SKIFF_ERR_STORAGE_NOT_FOUND) {
        return err;
    }
    err = skiff_storage_check_room(storage, folder, file_size + SKIFF_COVER_CACHE_ROOM_BYTES);
    /* A device that cannot tell is not refused: a full one fails the write instead. */
    return err == SKIFF_ERR_NOT_IMPLEMENTED ? SKIFF_OK : err;
}

/* The whole slot file for cover into a new buffer of *size bytes; NULL without memory. */
static unsigned char *encode(const skiff_cover_key *key, const skiff_cover *cover, size_t *size) {
    const size_t pixels = pixel_bytes(cover->height);
    *size = HEADER_BYTES + pixels;
    unsigned char *file = calloc(1, *size);
    if (file == NULL) {
        return NULL;
    }
    memcpy(file, FILE_MAGIC, FILE_MAGIC_BYTES);
    put16(file + AT_VERSION, FILE_VERSION);
    put16(file + AT_WIDTH, cover->width);
    put16(file + AT_HEIGHT, cover->height);
    put16(file + AT_BOX_WIDTH, SKIFF_COVER_WIDTH);
    put16(file + AT_BOX_HEIGHT, SKIFF_COVER_HEIGHT);
    put64(file + AT_ROM_ID, key->rom_id);
    put32(file + AT_SERVER, key->server);
    put32(file + AT_COVER_PATH, key->cover_path);
    for (size_t i = 0; i < pixels / PIXEL_BYTES; i++) {
        put16(file + HEADER_BYTES + i * PIXEL_BYTES, cover->pixels[i]);
    }
    put32(file + AT_CRC, file_crc(file, file + HEADER_BYTES, pixels));
    return file;
}

skiff_err skiff_cover_cache_store(skiff_storage *storage, const skiff_storage_roots *roots,
                                  const skiff_cover_key *key, const skiff_cover *cover) {
    if (storage == NULL || roots == NULL || key == NULL || cover == NULL || cover->width == 0 ||
        cover->width > SKIFF_COVER_WIDTH || cover->height == 0 ||
        cover->height > SKIFF_COVER_HEIGHT) {
        return SKIFF_ERR_INVALID_ARG;
    }
    char slot[SKIFF_STORAGE_PATH_MAX];
    char folder[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = slot_paths(roots, key->rom_id, slot, folder);
    if (err == SKIFF_OK) {
        err = skiff_storage_mkdirs(storage, folder);
    }
    size_t size = 0;
    unsigned char *file = err == SKIFF_OK ? encode(key, cover, &size) : NULL;
    if (err == SKIFF_OK && file == NULL) {
        err = SKIFF_ERR_NO_MEMORY;
    }
    if (err == SKIFF_OK) {
        err = check_room(storage, slot, folder, size);
    }
    skiff_file *handle = NULL;
    if (err == SKIFF_OK) {
        err = skiff_storage_open(storage, slot, SKIFF_FILE_REPLACE, 0, &handle);
    }
    if (err == SKIFF_OK) {
        err = skiff_file_write(handle, file, size);
    }
    const skiff_err closed = skiff_file_close(handle);
    if (err == SKIFF_OK) {
        err = closed;
    }
    free(file);
    return err;
}
