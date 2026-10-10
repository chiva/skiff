#include <png.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/cover.h"

/* A PNG file starts with these eight bytes; anything else is another format or an error page. */
#define PNG_SIGNATURE_BYTES 8
/* Every row is turned into 8-bit RGBA before it is scaled. */
#define RGBA_CHANNELS 4
#define RGB_CHANNELS 3
#define CHANNEL_MAX 255U
#define OPAQUE 0xFF
/* The largest ancillary chunk (an ICC profile, compressed text) libpng may allocate for. A cover
 * needs none of them; the limit keeps a crafted one from costing megabytes. */
#define CHUNK_MALLOC_MAX ((png_alloc_size_t)256 * 1024)
/* RGB565: 5 bits of red at bit 0, 6 of green at bit 5, 5 of blue at bit 11 (GU_PSM_5650). */
#define RED_DROP_BITS 3
#define GREEN_DROP_BITS 2
#define BLUE_DROP_BITS 3
#define GREEN_SHIFT 5
#define BLUE_SHIFT 11

/* Area averaging into one output row at a time: each source pixel adds to the output pixel it
 * falls in, and the row is written once the source moves past it. */
typedef struct scaler {
    uint32_t source_width;
    uint32_t source_height;
    uint32_t width;
    uint32_t height;
    /* The output row being gathered, and how many source rows have been added. */
    uint32_t row;
    uint32_t rows_added;
    uint32_t sums[SKIFF_COVER_WIDTH][RGB_CHANNELS];
    uint32_t counts[SKIFF_COVER_WIDTH];
} scaler;

/* Everything one decode owns, on the heap: the browsing thread's stack is 32 KB, and nothing the
 * error path needs lives in a local a longjmp could leave stale. */
typedef struct decoder {
    const unsigned char *data;
    size_t size;
    size_t offset;
    int out_of_memory;
    png_bytep row;
    png_bytep image;
    png_bytepp image_rows;
    scaler scale;
    skiff_cover *out;
} decoder;

uint16_t skiff_cover_rgb565(uint8_t red, uint8_t green, uint8_t blue) {
    return (uint16_t)(((unsigned)red >> RED_DROP_BITS) |
                      (((unsigned)green >> GREEN_DROP_BITS) << GREEN_SHIFT) |
                      (((unsigned)blue >> BLUE_DROP_BITS) << BLUE_SHIFT));
}

/* ---- Scaling ---- */

/* The size a source_width x source_height picture takes in the cover box: its own when it fits,
 * otherwise scaled down to touch the box on one side, keeping its shape. */
static void fit(scaler *scale) {
    const uint64_t source_width = scale->source_width;
    const uint64_t source_height = scale->source_height;
    if (source_width <= SKIFF_COVER_WIDTH && source_height <= SKIFF_COVER_HEIGHT) {
        scale->width = (uint32_t)source_width;
        scale->height = (uint32_t)source_height;
    } else if (source_width * SKIFF_COVER_HEIGHT >= source_height * SKIFF_COVER_WIDTH) {
        scale->width = SKIFF_COVER_WIDTH;
        scale->height =
            (uint32_t)((source_height * SKIFF_COVER_WIDTH + source_width / 2) / source_width);
    } else {
        scale->height = SKIFF_COVER_HEIGHT;
        scale->width =
            (uint32_t)((source_width * SKIFF_COVER_HEIGHT + source_height / 2) / source_height);
    }
    scale->width = scale->width == 0 ? 1 : scale->width;
    scale->height = scale->height == 0 ? 1 : scale->height;
}

/* One colour channel drawn over the background with its alpha. */
static uint32_t over_background(uint32_t channel, uint32_t alpha, uint32_t background) {
    return (channel * alpha + background * (CHANNEL_MAX - alpha) + CHANNEL_MAX / 2) / CHANNEL_MAX;
}

static void write_row(scaler *scale, skiff_cover *out) {
    uint16_t *pixels = out->pixels + (size_t)scale->row * SKIFF_COVER_WIDTH;
    for (uint32_t x = 0; x < scale->width; x++) {
        const uint32_t count = scale->counts[x];
        uint8_t channels[RGB_CHANNELS] = {0, 0, 0};
        for (int c = 0; c < RGB_CHANNELS && count > 0; c++) {
            channels[c] = (uint8_t)((scale->sums[x][c] + count / 2) / count);
        }
        pixels[x] = skiff_cover_rgb565(channels[0], channels[1], channels[2]);
    }
    memset(scale->sums, 0, sizeof scale->sums);
    memset(scale->counts, 0, sizeof scale->counts);
    scale->rows_added = 0;
}

/* Adds source row y (8-bit RGBA) to the output row it falls in, writing out the one before. */
static void add_row(scaler *scale, skiff_cover *out, uint32_t y, png_const_bytep rgba) {
    static const uint32_t BACKGROUND[RGB_CHANNELS] = {
        SKIFF_COVER_BACKGROUND_RED, SKIFF_COVER_BACKGROUND_GREEN, SKIFF_COVER_BACKGROUND_BLUE};
    const uint32_t row = (uint32_t)((uint64_t)y * scale->height / scale->source_height);
    if (row != scale->row && scale->rows_added > 0) {
        write_row(scale, out);
    }
    scale->row = row;
    for (uint32_t x = 0; x < scale->source_width; x++) {
        const uint32_t column = (uint32_t)((uint64_t)x * scale->width / scale->source_width);
        png_const_bytep pixel = rgba + (size_t)x * RGBA_CHANNELS;
        const uint32_t alpha = pixel[RGB_CHANNELS];
        for (int c = 0; c < RGB_CHANNELS; c++) {
            scale->sums[column][c] += over_background(pixel[c], alpha, BACKGROUND[c]);
        }
        scale->counts[column]++;
    }
    scale->rows_added++;
}

/* ---- libpng ---- */

static void read_bytes(png_structp png, png_bytep out, size_t count) {
    decoder *decode = png_get_io_ptr(png);
    if (count > decode->size - decode->offset) {
        png_error(png, "cut short");
    }
    memcpy(out, decode->data + decode->offset, count);
    decode->offset += count;
}

/* libpng's default handlers print to stderr; Skiff reports one code instead. */
static void on_error(png_structp png, png_const_charp message) {
    (void)message;
    png_longjmp(png, 1);
}

static void on_warning(png_structp png, png_const_charp message) {
    (void)png;
    (void)message;
}

static png_voidp allocate(png_structp png, png_alloc_size_t size) {
    void *block = malloc(size);
    if (block == NULL) {
        decoder *decode = png_get_mem_ptr(png);
        decode->out_of_memory = 1;
    }
    return block;
}

static void release(png_structp png, png_voidp block) {
    (void)png;
    free(block);
}

/* Asks libpng for 8-bit RGBA rows whatever the file holds. */
static void request_rgba(png_structp png, png_infop info) {
    const int color_type = png_get_color_type(png, info);
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    if ((color_type & PNG_COLOR_MASK_ALPHA) == 0 && !png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_filler(png, OPAQUE, PNG_FILLER_AFTER);
    }
}

/* Reads the rows, which run after setjmp: libpng errors land back there. */
static void read_rows(png_structp png, png_infop info, decoder *decode) {
    scaler *scale = &decode->scale;
    const int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    const size_t row_bytes = png_get_rowbytes(png, info);
    if (png_get_channels(png, info) != RGBA_CHANNELS ||
        row_bytes != (size_t)scale->source_width * RGBA_CHANNELS) {
        png_error(png, "unexpected row layout");
    }
    if (passes == 1) {
        decode->row = png_malloc(png, row_bytes);
        for (uint32_t y = 0; y < scale->source_height; y++) {
            png_read_row(png, decode->row, NULL);
            add_row(scale, decode->out, y, decode->row);
        }
    } else {
        /* Every pass touches every row, so an interlaced picture is read whole first. */
        if (row_bytes * scale->source_height > SKIFF_COVER_INTERLACED_MAX) {
            png_error(png, "interlaced picture too large");
        }
        decode->image = png_malloc(png, row_bytes * scale->source_height);
        decode->image_rows = (png_bytepp)png_malloc(png, sizeof(png_bytep) * scale->source_height);
        for (uint32_t y = 0; y < scale->source_height; y++) {
            decode->image_rows[y] = decode->image + (size_t)y * row_bytes;
        }
        png_read_image(png, decode->image_rows);
        for (uint32_t y = 0; y < scale->source_height; y++) {
            add_row(scale, decode->out, y, decode->image_rows[y]);
        }
    }
    write_row(scale, decode->out);
    /* The chunks after the picture too: a body cut before its end is not a whole cover. */
    png_read_end(png, NULL);
}

skiff_err skiff_cover_decode_png(const unsigned char *data, size_t size, skiff_cover *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (data == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (size < PNG_SIGNATURE_BYTES || png_sig_cmp(data, 0, PNG_SIGNATURE_BYTES) != 0) {
        return SKIFF_ERR_ROMM_COVER_FORMAT;
    }
    decoder *decode = calloc(1, sizeof *decode);
    if (decode == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    decode->data = data;
    decode->size = size;
    decode->out = out;
    png_structp png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, decode, on_error, on_warning,
                                               decode, allocate, release);
    png_infop info = png == NULL ? NULL : png_create_info_struct(png);
    if (info == NULL) {
        png_destroy_read_struct(png == NULL ? NULL : &png, NULL, NULL);
        free(decode);
        return SKIFF_ERR_NO_MEMORY;
    }
    skiff_err err = SKIFF_OK;
    if (setjmp(png_jmpbuf(png)) == 0) {
        png_set_read_fn(png, decode, read_bytes);
        png_set_user_limits(png, SKIFF_COVER_SOURCE_MAX, SKIFF_COVER_SOURCE_MAX);
        png_set_chunk_malloc_max(png, CHUNK_MALLOC_MAX);
        png_read_info(png, info);
        decode->scale.source_width = png_get_image_width(png, info);
        decode->scale.source_height = png_get_image_height(png, info);
        fit(&decode->scale);
        request_rgba(png, info);
        read_rows(png, info, decode);
        out->width = (uint16_t)decode->scale.width;
        out->height = (uint16_t)decode->scale.height;
    } else {
        err = decode->out_of_memory ? SKIFF_ERR_NO_MEMORY : SKIFF_ERR_ROMM_COVER_DAMAGED;
        memset(out, 0, sizeof *out);
    }
    png_free(png, decode->row);
    png_free(png, (png_voidp)decode->image_rows);
    png_free(png, decode->image);
    png_destroy_read_struct(&png, &info, NULL);
    free(decode);
    return err;
}
