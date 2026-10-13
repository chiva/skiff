#include "skiff/disc_image.h"

#include <string.h>

/* ISO 9660: the primary volume descriptor's sector, and what it starts with. */
#define ISO_PVD_SECTOR 16U
static const unsigned char ISO_PVD_START[] = {0x01, 'C', 'D', '0', '0', '1'};

/* CISO and ZISO headers, little-endian: magic, header size, disc size, block size, version,
 * index shift, two reserved bytes; the index follows. */
#define CSO_HEADER_BYTES 24U
#define CSO_HEADER_SIZE_AT 4
#define CSO_DISC_SIZE_AT 8
#define CSO_BLOCK_SIZE_AT 16
#define CSO_ALIGN_AT 21
#define CSO_INDEX_ENTRY_BYTES 4U
/* An index entry's top bit marks a block stored as it is; the rest, shifted, is its offset. */
#define CSO_INDEX_OFFSET_MASK 0x7FFFFFFFU
#define CSO_ALIGN_MAX 31U
static const char CSO_MAGIC[] = "CISO";
static const char ZSO_MAGIC[] = "ZISO";
enum { MAGIC_BYTES = 4, BITS_PER_BYTE = 8 };

static uint32_t read_u32(const unsigned char *at) {
    return (uint32_t)at[0] | (uint32_t)at[1] << BITS_PER_BYTE |
           (uint32_t)at[2] << 2 * BITS_PER_BYTE | (uint32_t)at[3] << 3 * BITS_PER_BYTE;
}

static uint64_t read_u64(const unsigned char *at) {
    return (uint64_t)read_u32(at) | (uint64_t)read_u32(at + CSO_INDEX_ENTRY_BYTES) << 32U;
}

/* Exactly length bytes of path from offset; SKIFF_ERR_ROMM_CONVERTED_DAMAGED when the file ends
 * first (the caller checked the size, so only a file changed meanwhile does). */
static skiff_err read_at(skiff_storage *storage, const char *path, uint64_t offset,
                         unsigned char *out, size_t length) {
    skiff_file *file = NULL;
    skiff_err err = skiff_storage_open(storage, path, SKIFF_FILE_READ, offset, &file);
    size_t done = 0;
    while (err == SKIFF_OK && done < length) {
        size_t got = 0;
        err = skiff_file_read(file, out + done, length - done, &got);
        if (err == SKIFF_OK && got == 0) {
            err = SKIFF_ERR_ROMM_CONVERTED_DAMAGED;
        }
        done += got;
    }
    const skiff_err closed = file != NULL ? skiff_file_close(file) : SKIFF_OK;
    return err != SKIFF_OK ? err : closed;
}

static skiff_err check_iso(skiff_storage *storage, const char *path, uint64_t size) {
    if (size % SKIFF_DISC_IMAGE_SECTOR_BYTES != 0 ||
        size < (uint64_t)(ISO_PVD_SECTOR + 1) * SKIFF_DISC_IMAGE_SECTOR_BYTES) {
        return SKIFF_ERR_ROMM_CONVERTED_DAMAGED;
    }
    unsigned char start[sizeof ISO_PVD_START];
    const skiff_err err =
        read_at(storage, path, (uint64_t)ISO_PVD_SECTOR * SKIFF_DISC_IMAGE_SECTOR_BYTES, start,
                sizeof start);
    if (err != SKIFF_OK) {
        return err;
    }
    return memcmp(start, ISO_PVD_START, sizeof start) == 0 ? SKIFF_OK
                                                           : SKIFF_ERR_ROMM_CONVERTED_DAMAGED;
}

static int is_power_of_two(uint32_t value) { return value != 0 && (value & (value - 1)) == 0; }

static skiff_err check_compressed(skiff_storage *storage, const char *path, uint64_t size,
                                  const char *magic) {
    unsigned char header[CSO_HEADER_BYTES + CSO_INDEX_ENTRY_BYTES];
    if (size < sizeof header) {
        return SKIFF_ERR_ROMM_CONVERTED_DAMAGED;
    }
    skiff_err err = read_at(storage, path, 0, header, sizeof header);
    if (err != SKIFF_OK) {
        return err;
    }
    const uint32_t header_size = read_u32(header + CSO_HEADER_SIZE_AT);
    const uint64_t disc_size = read_u64(header + CSO_DISC_SIZE_AT);
    const uint32_t block_size = read_u32(header + CSO_BLOCK_SIZE_AT);
    const unsigned align = header[CSO_ALIGN_AT];
    if (memcmp(header, magic, MAGIC_BYTES) != 0 ||
        (header_size != CSO_HEADER_BYTES && header_size != 0) || disc_size == 0 ||
        disc_size % SKIFF_DISC_IMAGE_SECTOR_BYTES != 0 || !is_power_of_two(block_size) ||
        block_size < SKIFF_DISC_IMAGE_SECTOR_BYTES || block_size > SKIFF_DISC_IMAGE_BLOCK_MAX ||
        align > CSO_ALIGN_MAX) {
        return SKIFF_ERR_ROMM_CONVERTED_DAMAGED;
    }
    /* One entry per block and one more. Rounded up without adding first, so a disc size near 2^64
     * cannot wrap to no blocks; at most 2^53 blocks, so the index's end cannot wrap either. */
    const uint64_t blocks = disc_size / block_size + (disc_size % block_size != 0 ? 1 : 0);
    const uint64_t index_end = CSO_HEADER_BYTES + (blocks + 1) * CSO_INDEX_ENTRY_BYTES;
    if (index_end > size) {
        return SKIFF_ERR_ROMM_CONVERTED_DAMAGED;
    }
    unsigned char last[CSO_INDEX_ENTRY_BYTES];
    err = read_at(storage, path, index_end - CSO_INDEX_ENTRY_BYTES, last, sizeof last);
    if (err != SKIFF_OK) {
        return err;
    }
    const uint64_t first_at =
        (uint64_t)(read_u32(header + CSO_HEADER_BYTES) & CSO_INDEX_OFFSET_MASK) << align;
    const uint64_t end_at = (uint64_t)(read_u32(last) & CSO_INDEX_OFFSET_MASK) << align;
    return first_at >= index_end && end_at >= first_at && end_at <= size
               ? SKIFF_OK
               : SKIFF_ERR_ROMM_CONVERTED_DAMAGED;
}

skiff_err skiff_disc_image_check(skiff_storage *storage, const char *path, uint64_t size,
                                 skiff_romm_format format) {
    if (storage == NULL || path == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    switch (format) {
    case SKIFF_ROMM_FORMAT_ISO:
        return check_iso(storage, path, size);
    case SKIFF_ROMM_FORMAT_CSO:
        return check_compressed(storage, path, size, CSO_MAGIC);
    case SKIFF_ROMM_FORMAT_ZSO:
        return check_compressed(storage, path, size, ZSO_MAGIC);
    default:
        return SKIFF_ERR_INVALID_ARG;
    }
}
