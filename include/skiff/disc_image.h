#ifndef SKIFF_DISC_IMAGE_H
#define SKIFF_DISC_IMAGE_H

/*
 * Whether a file is a disc image in the format Skiff asked RomM to convert a game into. RomM sends
 * no checksum for a converted file, so the download engine runs this before the file takes its
 * final name (skiff_download_spec.check): enough to catch an error page, a cut file or a broken
 * conversion, not proof of the contents.
 *
 *   - ISO: whole 2048-byte sectors, at least up to the primary volume descriptor, which is at
 *     sector 16 and starts with "\1CD001" (ISO 9660).
 *   - CSO and ZSO (CISO and ZISO, as rom-converto and maxcso write them): the magic ("CISO",
 *     "ZISO"), a 24-byte header (or 0, as old tools write), a disc of whole sectors, a block size
 *     that is a power of two from 2048 to SKIFF_DISC_IMAGE_BLOCK_MAX, and an index of one entry
 *     per block plus one, inside the file: the first block starts at or after the index's end,
 *     and the last entry (where the data ends) is no earlier than that and within the file.
 */

#include <stdint.h>

#include "skiff/error.h"
#include "skiff/romm.h"
#include "skiff/storage.h"

#define SKIFF_DISC_IMAGE_SECTOR_BYTES 2048U
/* The largest CSO/ZSO block (maxcso's largest is 16 KiB; rom-converto's 16 KiB for 2 GiB discs). */
#define SKIFF_DISC_IMAGE_BLOCK_MAX ((uint32_t)1024 * 1024)

/*
 * Checks the file at path, size bytes long (as downloaded), against format. SKIFF_OK when it
 * passes; SKIFF_ERR_ROMM_CONVERTED_DAMAGED when it does not; the storage's error when it cannot be
 * read; SKIFF_ERR_INVALID_ARG for a NULL argument or a format that is not a disc image Skiff asks
 * for (SKIFF_ROMM_FORMAT_STORED, or out of range).
 */
skiff_err skiff_disc_image_check(skiff_storage *storage, const char *path, uint64_t size,
                                 skiff_romm_format format);

#endif
