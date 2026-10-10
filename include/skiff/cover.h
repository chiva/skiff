#ifndef SKIFF_COVER_H
#define SKIFF_COVER_H

/*
 * A game's cover as the PSP draws it: RomM's small cover (skiff_romm_rom.cover_path), decoded and
 * scaled down once into a picture the GE can draw as it is. Decoding a 240x320 PNG takes several
 * frames' time on a PSP, so it runs on the browsing thread, never on the UI thread, and the result
 * is what the Memory Stick cache keeps.
 *
 * Only PNG is decoded (libpng): RomM saves every cover it downloads as PNG, whatever the source.
 * Artwork uploaded in RomM's web UI keeps its own format; such a cover is refused with
 * SKIFF_ERR_ROMM_COVER_FORMAT and the game shows without one.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"
#include "skiff/storage.h"
#include "skiff/storage_paths.h"

/* The box a cover is fitted into on the details screen, in pixels: the cover keeps its shape, so
 * one side may be shorter. A cover smaller than the box keeps its size. */
#define SKIFF_COVER_WIDTH 160
#define SKIFF_COVER_HEIGHT 220
/* Pixels in a cover's buffer, used or not. */
#define SKIFF_COVER_PIXELS ((size_t)SKIFF_COVER_WIDTH * SKIFF_COVER_HEIGHT)
/* The largest picture decoded, per side: RomM's small covers are a few hundred pixels, and the
 * limit bounds the memory and time one cover can take. */
#define SKIFF_COVER_SOURCE_MAX 1024
/* An interlaced PNG is decoded whole before it is scaled: at most this many bytes of it. */
#define SKIFF_COVER_INTERLACED_MAX ((size_t)1024 * 1024)
/* What transparent parts of a cover are drawn over (RGB): the details screen's placeholder grey. */
#define SKIFF_COVER_BACKGROUND_RED 0x30
#define SKIFF_COVER_BACKGROUND_GREEN 0x30
#define SKIFF_COVER_BACKGROUND_BLUE 0x30
/* The GE reads textures from 16-byte aligned addresses. */
#define SKIFF_COVER_ALIGNMENT 16

/*
 * A decoded cover: width x height pixels in RGB565 as the PSP's GU_PSM_5650 lays them out (red in
 * the low 5 bits, blue in the high 5), rows SKIFF_COVER_WIDTH pixels apart. Pixels outside
 * width x height are 0. About 70 KB: allocate it, never put it on a thread's stack.
 */
typedef struct skiff_cover {
    _Alignas(SKIFF_COVER_ALIGNMENT) uint16_t pixels[SKIFF_COVER_PIXELS];
    uint16_t width;
    uint16_t height;
} skiff_cover;

/*
 * Decodes the PNG in data[0..size) into out, scaled down with area averaging to fit
 * SKIFF_COVER_WIDTH x SKIFF_COVER_HEIGHT; transparency is drawn over the background colour above.
 * Any PNG colour type and bit depth is accepted. Returns SKIFF_ERR_ROMM_COVER_FORMAT for data that
 * is not a PNG (a JPEG, WebP or an error page), SKIFF_ERR_ROMM_COVER_DAMAGED for a damaged or cut
 * PNG, one larger than SKIFF_COVER_SOURCE_MAX on a side, or an interlaced one over
 * SKIFF_COVER_INTERLACED_MAX, SKIFF_ERR_NO_MEMORY when libpng cannot allocate, and
 * SKIFF_ERR_INVALID_ARG for a NULL argument. out is all zero on error.
 */
skiff_err skiff_cover_decode_png(const unsigned char *data, size_t size, skiff_cover *out);

/* One RGB colour as a GU_PSM_5650 pixel. */
uint16_t skiff_cover_rgb565(uint8_t red, uint8_t green, uint8_t blue);

/* ---- The Memory Stick cache ---- */

/*
 * Decoded covers are kept in SKIFF_COVER_CACHE_SLOTS files, "app:/covers/<rom_id % slots>.cov",
 * so the cache is bounded by construction (about 4.5 MB), needs no index and no folder listing, and
 * no text from RomM reaches a file name. Two ROMs sharing a slot evict each other. Each file names
 * the cover it holds (ROM, server, cover path with RomM's "?ts=", so a changed cover misses) and
 * ends a CRC-32 over all of it: a file cut by a power loss or written by another server reads as a
 * miss and is overwritten. Writes are not synced: a lost cover is fetched again.
 */
#define SKIFF_COVER_CACHE_FOLDER SKIFF_STORAGE_ROOT_APP "/covers"
#define SKIFF_COVER_CACHE_SLOTS 64
#define SKIFF_COVER_CACHE_EXTENSION ".cov"
/* Free space a new slot file leaves on top of SKIFF_STORAGE_FREE_MARGIN_BYTES: covers never take
 * the last of the Memory Stick from downloads. */
#define SKIFF_COVER_CACHE_ROOM_BYTES ((uint64_t)16 * 1024 * 1024)

/* Which cover a slot file holds. */
typedef struct skiff_cover_key {
    uint64_t rom_id;
    /* CRC-32 of the server address: ROM ids repeat across servers. */
    uint32_t server;
    /* CRC-32 of the cover path RomM gave, "?ts=" included. */
    uint32_t cover_path;
} skiff_cover_key;

/* The key of rom_id's cover at cover_path on the server at base_url. */
skiff_cover_key skiff_cover_key_of(const char *base_url, uint64_t rom_id, const char *cover_path);

/*
 * Reads key's cover from its slot into out. SKIFF_ERR_STORAGE_NOT_FOUND when the slot is empty or
 * holds another cover; SKIFF_ERR_ROMM_COVER_DAMAGED when the file is not a whole cover (cut,
 * changed, from another Skiff version); otherwise the storage's error. out is all zero unless
 * SKIFF_OK.
 */
skiff_err skiff_cover_cache_load(skiff_storage *storage, const skiff_storage_roots *roots,
                                 const skiff_cover_key *key, skiff_cover *out);

/*
 * Writes cover into key's slot, replacing whatever it held, and creates the folder first. Whatever
 * the slot file grows by (all of it for a new one, the difference over a shorter cover or a cut
 * file) needs SKIFF_COVER_CACHE_ROOM_BYTES free beyond the margin (SKIFF_ERR_STORAGE_NO_SPACE
 * otherwise); a replacement no larger than the file there is not checked. A device that cannot
 * report its free space is written to anyway. SKIFF_ERR_INVALID_ARG for a NULL argument or a
 * cover larger than the box; otherwise the storage's error, and the slot may then hold a cut file,
 * which loads as damaged.
 */
skiff_err skiff_cover_cache_store(skiff_storage *storage, const skiff_storage_roots *roots,
                                  const skiff_cover_key *key, const skiff_cover *cover);

#endif
