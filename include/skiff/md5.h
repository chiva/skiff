#ifndef SKIFF_MD5_H
#define SKIFF_MD5_H

/*
 * MD5 (RFC 1321), the hash RomM identifies saves by (skiff/saves.h). Skiff carries its own rather
 * than calling Mbed TLS's PSA hashes: those need psa_crypto_init() first, which on the PSP needs
 * the TLS entropy hook and so ARK, and save units must work without either (the emulator has no
 * ARK). MD5 only names content here, as RomM uses it; nothing relies on it for security.
 */

#include <stddef.h>
#include <stdint.h>

#define SKIFF_MD5_DIGEST_BYTES 16
#define SKIFF_MD5_BLOCK_BYTES 64
#define SKIFF_MD5_STATE_WORDS 4
/* A digest as lowercase hex, as RomM writes it, terminator included. */
#define SKIFF_MD5_HEX_SIZE (SKIFF_MD5_DIGEST_BYTES * 2 + 1)

typedef struct skiff_md5 {
    uint32_t state[SKIFF_MD5_STATE_WORDS];
    /* Bytes hashed so far; the part of a block not yet hashed is its remainder. */
    uint64_t length;
    unsigned char block[SKIFF_MD5_BLOCK_BYTES];
} skiff_md5;

void skiff_md5_init(skiff_md5 *md5);

/* Adds size bytes of data (NULL only with size 0). */
void skiff_md5_update(skiff_md5 *md5, const void *data, size_t size);

/* Writes the digest of everything added; md5 must be initialised again before reuse. */
void skiff_md5_final(skiff_md5 *md5, unsigned char digest[SKIFF_MD5_DIGEST_BYTES]);

/* The digest as lowercase hex, NUL-terminated. */
void skiff_md5_hex(const unsigned char digest[SKIFF_MD5_DIGEST_BYTES],
                   char out[SKIFF_MD5_HEX_SIZE]);

#endif
