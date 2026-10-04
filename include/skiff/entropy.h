#ifndef SKIFF_ENTROPY_H
#define SKIFF_ENTROPY_H

/*
 * Entropy for TLS. Wraps a hardware generator that yields 32-bit words (on the PSP, the KIRK crypto
 * engine through ARK) and turns it into byte buffers for Mbed TLS's entropy hook, with a health
 * test between the two. Mbed TLS's random generator conditions what it is given, so the bytes are
 * passed through unmodified.
 *
 * Health tests: the two continuous tests of NIST SP 800-90B (4.4). Skiff credits each word with
 * full entropy (32 bits, as Mbed TLS requires), which puts both cutoffs at 2:
 *   - repetition count (4.4.1): a word equal to the previous one fails. Catches a stuck generator
 *     or one returning a constant error code at once; an ideal one trips it once in 2^32 words.
 *   - adaptive proportion (4.4.2), over windows of SKIFF_ENTROPY_WINDOW_WORDS: the window's first
 *     word appearing again within the window fails. Catches alternating values and short cycles;
 *     an ideal generator trips it about once in 8 million windows.
 * A failure, or an error from the generator, is permanent for the source: every later fill fails
 * too, so TLS stays off for the rest of the session.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"

/* Reads one word from the generator into *word. Returns SKIFF_OK or the reason it could not. */
typedef skiff_err (*skiff_entropy_read_fn)(void *ctx, uint32_t *word);

#define SKIFF_ENTROPY_WINDOW_WORDS 512U

typedef struct skiff_entropy_source {
    skiff_entropy_read_fn read;
    void *ctx;
    uint32_t previous;
    int has_previous;
    uint32_t window_first;
    unsigned window_position; /* 0 starts a new window */
    int failed;
} skiff_entropy_source;

/* Returns SKIFF_ERR_INVALID_ARG if source or read is NULL. ctx is passed to read unchanged. */
skiff_err skiff_entropy_source_init(skiff_entropy_source *source, skiff_entropy_read_fn read,
                                    void *ctx);

/*
 * Fills out with out_size bytes from the generator, in little-endian word order; the unused bytes
 * of a final partial word are discarded. Returns SKIFF_ERR_INVALID_ARG for a NULL argument,
 * SKIFF_ERR_NET_ENTROPY if the health test fails now or failed before, or the read function's
 * error. On any error out is zeroed (when it is not NULL), so a caller ignoring the result gets no
 * partial output.
 */
skiff_err skiff_entropy_fill(skiff_entropy_source *source, unsigned char *out, size_t out_size);

#endif
