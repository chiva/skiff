#include "skiff/entropy.h"

#include <string.h>

enum { ENTROPY_WORD_BYTES = 4, ENTROPY_BITS_PER_BYTE = 8 };

skiff_err skiff_entropy_source_init(skiff_entropy_source *source, skiff_entropy_read_fn read,
                                    void *ctx) {
    if (source == NULL || read == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(source, 0, sizeof *source);
    source->read = read;
    source->ctx = ctx;
    return SKIFF_OK;
}

/* Repetition count test, cutoff 2 (see entropy.h). */
static int passes_repetition_count(skiff_entropy_source *source, uint32_t word) {
    if (source->has_previous && word == source->previous) {
        return 0;
    }
    source->previous = word;
    source->has_previous = 1;
    return 1;
}

/* Adaptive proportion test, cutoff 2 (see entropy.h). */
static int passes_adaptive_proportion(skiff_entropy_source *source, uint32_t word) {
    if (source->window_position == 0) {
        source->window_first = word;
    } else if (word == source->window_first) {
        return 0;
    }
    source->window_position = (source->window_position + 1) % SKIFF_ENTROPY_WINDOW_WORDS;
    return 1;
}

/* Both tests see every word; a failure latches the source. */
static int passes_health_tests(skiff_entropy_source *source, uint32_t word) {
    if (!passes_repetition_count(source, word) || !passes_adaptive_proportion(source, word)) {
        source->failed = 1;
        return 0;
    }
    return 1;
}

static skiff_err fail_fill(unsigned char *out, size_t out_size, skiff_err err) {
    if (out != NULL) {
        memset(out, 0, out_size);
    }
    return err;
}

skiff_err skiff_entropy_fill(skiff_entropy_source *source, unsigned char *out, size_t out_size) {
    if (source == NULL || source->read == NULL || out == NULL) {
        return fail_fill(out, out_size, SKIFF_ERR_INVALID_ARG);
    }
    if (source->failed) {
        return fail_fill(out, out_size, SKIFF_ERR_NET_ENTROPY);
    }
    for (size_t offset = 0; offset < out_size; offset += ENTROPY_WORD_BYTES) {
        uint32_t word = 0;
        const skiff_err err = source->read(source->ctx, &word);
        if (err != SKIFF_OK) {
            source->failed = 1;
            return fail_fill(out, out_size, err);
        }
        if (!passes_health_tests(source, word)) {
            return fail_fill(out, out_size, SKIFF_ERR_NET_ENTROPY);
        }
        for (size_t byte = 0; byte < ENTROPY_WORD_BYTES && offset + byte < out_size; byte++) {
            out[offset + byte] = (unsigned char)(word >> (byte * ENTROPY_BITS_PER_BYTE));
        }
    }
    return SKIFF_OK;
}
