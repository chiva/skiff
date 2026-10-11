/*
 * MD5 as RFC 1321 specifies it (§3: padding, length, the four rounds of 16 steps), written from the
 * RFC rather than its reference code. Words are read and written little-endian byte by byte, so it
 * runs the same on any byte order and alignment.
 */
#include "skiff/md5.h"

#include <string.h>

#define WORDS_PER_BLOCK 16
#define STEPS 64
#define STEPS_PER_ROUND 16
#define BITS_PER_BYTE 8
#define WORD_BITS 32
#define BYTE_MASK 0xFFU
/* §3.1: a 1 bit, zeros, then the length in bits as 8 bytes, ending a block. */
#define PADDING_FIRST_BYTE 0x80U
#define LENGTH_BYTES 8
#define LENGTH_OFFSET (SKIFF_MD5_BLOCK_BYTES - LENGTH_BYTES)
#define HEX_DIGITS "0123456789abcdef"
#define NIBBLE_BITS 4
#define NIBBLE_MASK 0x0FU

/* §3.3: the initial buffer A, B, C, D. */
static const uint32_t INITIAL_STATE[SKIFF_MD5_STATE_WORDS] = {0x67452301U, 0xefcdab89U, 0x98badcfeU,
                                                              0x10325476U};

/* §3.4: T[i], the integer part of 4294967296 * abs(sin(i)) for i = 1..64. */
static const uint32_t SINE_TABLE[STEPS] = {
    0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U,
    0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U,
    0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU,
    0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U, 0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
    0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U,
    0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU,
    0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U, 0xf4292244U,
    0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
    0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU,
    0xeb86d391U};

/* §3.4: the left rotation of each step. */
static const unsigned SHIFTS[STEPS] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

/* §3.4: which word of the block each step reads. Round 1 takes them in order, rounds 2, 3 and 4
 * step through them by 5, 3 and 7 from 1, 5 and 0. */
static const unsigned char WORD_ORDER[STEPS] = {
    0,  1,  2,  3,  4, 5, 6,  7, 8,  9,  10, 11, 12, 13, 14, 15, 1, 6,  11, 0, 5, 10,
    15, 4,  9,  14, 3, 8, 13, 2, 7,  12, 5,  8,  11, 14, 1,  4,  7, 10, 13, 0, 3, 6,
    9,  12, 15, 2,  0, 7, 14, 5, 12, 3,  10, 1,  8,  15, 6,  13, 4, 11, 2,  9};

enum { ROUND_F, ROUND_G, ROUND_H, ROUND_I };

static uint32_t rotate_left(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (WORD_BITS - bits));
}

static uint32_t load_le32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << BITS_PER_BYTE |
           (uint32_t)bytes[2] << (2 * BITS_PER_BYTE) | (uint32_t)bytes[3] << (3 * BITS_PER_BYTE);
}

static void store_le32(unsigned char *bytes, uint32_t value) {
    for (size_t i = 0; i < sizeof value; i++) {
        bytes[i] = (unsigned char)((value >> (i * BITS_PER_BYTE)) & BYTE_MASK);
    }
}

/* §3.4: F, G, H and I, one per round. */
static uint32_t round_function(int round, uint32_t b, uint32_t c, uint32_t d) {
    switch (round) {
    case ROUND_F:
        return (b & c) | (~b & d);
    case ROUND_G:
        return (b & d) | (c & ~d);
    case ROUND_H:
        return b ^ c ^ d;
    default:
        return c ^ (b | ~d);
    }
}

static void hash_block(uint32_t state[SKIFF_MD5_STATE_WORDS], const unsigned char *block) {
    uint32_t words[WORDS_PER_BLOCK];
    for (size_t i = 0; i < WORDS_PER_BLOCK; i++) {
        words[i] = load_le32(block + i * sizeof words[0]);
    }
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    for (size_t step = 0; step < STEPS; step++) {
        const uint32_t mixed = a + round_function((int)(step / STEPS_PER_ROUND), b, c, d) +
                               SINE_TABLE[step] + words[WORD_ORDER[step]];
        a = d;
        d = c;
        c = b;
        b += rotate_left(mixed, SHIFTS[step]);
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void skiff_md5_init(skiff_md5 *md5) {
    memcpy(md5->state, INITIAL_STATE, sizeof md5->state);
    md5->length = 0;
}

void skiff_md5_update(skiff_md5 *md5, const void *data, size_t size) {
    const unsigned char *bytes = data;
    size_t used = (size_t)(md5->length % SKIFF_MD5_BLOCK_BYTES);
    md5->length += size;
    while (size > 0) {
        const size_t room = SKIFF_MD5_BLOCK_BYTES - used;
        const size_t take = size < room ? size : room;
        if (used == 0 && take == SKIFF_MD5_BLOCK_BYTES) {
            hash_block(md5->state, bytes);
        } else {
            memcpy(md5->block + used, bytes, take);
            if (used + take == SKIFF_MD5_BLOCK_BYTES) {
                hash_block(md5->state, md5->block);
            }
        }
        used = (used + take) % SKIFF_MD5_BLOCK_BYTES;
        bytes += take;
        size -= take;
    }
}

void skiff_md5_final(skiff_md5 *md5, unsigned char digest[SKIFF_MD5_DIGEST_BYTES]) {
    const uint64_t bits = md5->length * BITS_PER_BYTE;
    size_t used = (size_t)(md5->length % SKIFF_MD5_BLOCK_BYTES);
    md5->block[used++] = PADDING_FIRST_BYTE;
    if (used > LENGTH_OFFSET) {
        memset(md5->block + used, 0, SKIFF_MD5_BLOCK_BYTES - used);
        hash_block(md5->state, md5->block);
        used = 0;
    }
    memset(md5->block + used, 0, LENGTH_OFFSET - used);
    for (size_t i = 0; i < LENGTH_BYTES; i++) {
        md5->block[LENGTH_OFFSET + i] = (unsigned char)((bits >> (i * BITS_PER_BYTE)) & BYTE_MASK);
    }
    hash_block(md5->state, md5->block);
    for (size_t i = 0; i < SKIFF_MD5_STATE_WORDS; i++) {
        store_le32(digest + i * sizeof md5->state[0], md5->state[i]);
    }
}

void skiff_md5_hex(const unsigned char digest[SKIFF_MD5_DIGEST_BYTES],
                   char out[SKIFF_MD5_HEX_SIZE]) {
    for (size_t i = 0; i < SKIFF_MD5_DIGEST_BYTES; i++) {
        out[2 * i] = HEX_DIGITS[digest[i] >> NIBBLE_BITS];
        out[2 * i + 1] = HEX_DIGITS[digest[i] & NIBBLE_MASK];
    }
    out[SKIFF_MD5_HEX_SIZE - 1] = '\0';
}
