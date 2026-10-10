/*
 * Cover decoding (skiff/cover.h): PNGs of every colour type and bit depth, interlaced or not,
 * scaled down into the cover box with area averaging and their transparency drawn over the
 * background, and everything that is not a whole PNG refused with a code instead of a crash. The
 * PNGs are written in memory with libpng's writer, so every picture here is synthetic.
 */
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "skiff/cover.h"

#include "unity.h"

#define PNG_BYTES_MAX ((size_t)8 * 1024 * 1024)
#define CHANNEL_MAX 255
#define HALF_ALPHA 128
/* A small RomM cover: a 600x800 upload scaled to 40%. */
#define SMALL_COVER_WIDTH 240
#define SMALL_COVER_HEIGHT 320
/* A PNG chunk: length (4 bytes), type (4), data, CRC-32 of type and data (4). */
#define CHUNK_LENGTH_BYTES 4
#define CHUNK_TYPE_BYTES 4
#define CHUNK_CRC_BYTES 4
#define PNG_SIGNATURE_BYTES 8
/* Corruption rounds over one PNG, each with a different byte changed. */
#define CORRUPTION_ROUNDS 400
#define LCG_MULTIPLIER 1103515245U
#define LCG_INCREMENT 12345U

/* What a synthetic picture holds at (x, y), in 8-bit RGBA. */
typedef void (*sample_fn)(uint32_t x, uint32_t y, png_byte rgba[4]);

typedef struct picture {
    uint32_t width;
    uint32_t height;
    int color_type;
    int bit_depth;
    int interlace;
    sample_fn sample;
} picture;

typedef struct png_buffer {
    unsigned char *data;
    size_t size;
} png_buffer;

static skiff_cover *cover;
static png_buffer encoded;

void setUp(void) {
    cover = malloc(sizeof *cover);
    TEST_ASSERT_NOT_NULL(cover);
    encoded.data = malloc(PNG_BYTES_MAX);
    TEST_ASSERT_NOT_NULL(encoded.data);
    encoded.size = 0;
}

void tearDown(void) {
    free(encoded.data);
    free(cover);
}

/* ---- Synthetic pictures ---- */

static const png_byte TEAL[4] = {0x20, 0xA0, 0x90, 0xFF};
static const png_byte GREY[4] = {0x80, 0x80, 0x80, 0xFF};

static void teal(uint32_t x, uint32_t y, png_byte rgba[4]) {
    (void)x;
    (void)y;
    memcpy(rgba, TEAL, 4);
}

static void grey(uint32_t x, uint32_t y, png_byte rgba[4]) {
    (void)x;
    (void)y;
    memcpy(rgba, GREY, 4);
}

static void clear(uint32_t x, uint32_t y, png_byte rgba[4]) {
    teal(x, y, rgba);
    rgba[3] = 0;
}

static void half_clear(uint32_t x, uint32_t y, png_byte rgba[4]) {
    teal(x, y, rgba);
    rgba[3] = HALF_ALPHA;
}

/* Black and white columns, one pixel wide. */
static void stripes(uint32_t x, uint32_t y, png_byte rgba[4]) {
    (void)y;
    const png_byte level = x % 2 == 0 ? 0 : CHANNEL_MAX;
    rgba[0] = level;
    rgba[1] = level;
    rgba[2] = level;
    rgba[3] = CHANNEL_MAX;
}

/* Black and white rows, one pixel high. */
static void row_stripes(uint32_t x, uint32_t y, png_byte rgba[4]) { stripes(y, x, rgba); }

/* Noise, so the PNG compresses about as badly as a photograph. */
static void noise(uint32_t x, uint32_t y, png_byte rgba[4]) {
    const uint32_t mixed = (x * 2654435761U) ^ (y * 40503U);
    rgba[0] = (png_byte)mixed;
    rgba[1] = (png_byte)(mixed >> 8);
    rgba[2] = (png_byte)(mixed >> 16);
    rgba[3] = CHANNEL_MAX;
}

static void append(png_structp png, png_bytep data, size_t size) {
    png_buffer *out = png_get_io_ptr(png);
    if (size > PNG_BYTES_MAX - out->size) {
        png_error(png, "test buffer full");
    }
    memcpy(out->data + out->size, data, size);
    out->size += size;
}

static void flush(png_structp png) { (void)png; }

/* The palette for palette pictures: the sample's colour at (0, 0), black and white. */
static void write_palette(png_structp png, png_infop info, const picture *spec) {
    png_byte first[4];
    spec->sample(0, 0, first);
    png_color colors[3] = {{first[0], first[1], first[2]}, {0, 0, 0}, {255, 255, 255}};
    png_set_PLTE(png, info, colors, 3);
    if (first[3] != CHANNEL_MAX) {
        png_byte alpha[1] = {first[3]};
        png_set_tRNS(png, info, alpha, 1, NULL);
    }
}

/* One row of spec in its colour type and bit depth. */
static void encode_row(const picture *spec, uint32_t y, png_bytep row) {
    size_t used = 0;
    for (uint32_t x = 0; x < spec->width; x++) {
        png_byte rgba[4];
        spec->sample(x, y, rgba);
        switch (spec->color_type) {
        case PNG_COLOR_TYPE_PALETTE:
            row[used++] = 0;
            break;
        case PNG_COLOR_TYPE_GRAY:
            if (spec->bit_depth == 1) {
                if (x % 8 == 0) {
                    row[used++] = 0;
                }
                row[used - 1] |= (png_byte)((rgba[0] > CHANNEL_MAX / 2) << (7 - x % 8));
            } else {
                row[used++] = rgba[0];
            }
            break;
        case PNG_COLOR_TYPE_GRAY_ALPHA:
            row[used++] = rgba[0];
            row[used++] = rgba[3];
            break;
        case PNG_COLOR_TYPE_RGB:
            for (int c = 0; c < 3; c++) {
                row[used++] = rgba[c];
                if (spec->bit_depth == 16) {
                    row[used++] = rgba[c];
                }
            }
            break;
        default:
            for (int c = 0; c < 4; c++) {
                row[used++] = rgba[c];
            }
            break;
        }
    }
}

/* Writes spec as a PNG into encoded. */
static void make_png(const picture *spec) {
    encoded.size = 0;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    TEST_ASSERT_NOT_NULL(info);
    png_bytep row = calloc((size_t)spec->width * 8 + 8, 1);
    TEST_ASSERT_NOT_NULL(row);
    if (setjmp(png_jmpbuf(png)) != 0) {
        TEST_FAIL_MESSAGE("libpng could not write the test picture");
    }
    png_set_write_fn(png, &encoded, append, flush);
    png_set_IHDR(png, info, spec->width, spec->height, spec->bit_depth, spec->color_type,
                 spec->interlace, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    if (spec->color_type == PNG_COLOR_TYPE_PALETTE) {
        write_palette(png, info, spec);
    }
    png_write_info(png, info);
    const int passes = png_set_interlace_handling(png);
    for (int pass = 0; pass < passes; pass++) {
        for (uint32_t y = 0; y < spec->height; y++) {
            encode_row(spec, y, row);
            png_write_row(png, row);
        }
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    free(row);
}

static picture rgb(uint32_t width, uint32_t height, sample_fn sample) {
    picture spec = {width, height, PNG_COLOR_TYPE_RGB, 8, PNG_INTERLACE_NONE, sample};
    return spec;
}

static skiff_err decode(void) {
    const skiff_err err = skiff_cover_decode_png(encoded.data, encoded.size, cover);
    TEST_PRINTF("%zu bytes -> %s, %ux%u", encoded.size, skiff_err_name(err), cover->width,
                cover->height);
    return err;
}

static uint16_t pixel_at(uint32_t x, uint32_t y) {
    return cover->pixels[(size_t)y * SKIFF_COVER_WIDTH + x];
}

/* Every pixel of the cover is expected; every one outside it 0. */
static void assert_filled_with(uint16_t expected) {
    for (uint32_t y = 0; y < SKIFF_COVER_HEIGHT; y++) {
        for (uint32_t x = 0; x < SKIFF_COVER_WIDTH; x++) {
            const int inside = x < cover->width && y < cover->height;
            if (pixel_at(x, y) != (inside ? expected : 0)) {
                char message[96];
                snprintf(message, sizeof message, "pixel (%u, %u) is %04x, expected %04x", x, y,
                         pixel_at(x, y), inside ? expected : 0);
                TEST_FAIL_MESSAGE(message);
            }
        }
    }
}

static void assert_all_zero(void) {
    TEST_ASSERT_EQUAL_UINT16(0, cover->width);
    TEST_ASSERT_EQUAL_UINT16(0, cover->height);
    for (size_t i = 0; i < SKIFF_COVER_PIXELS; i++) {
        TEST_ASSERT_EQUAL_UINT16(0, cover->pixels[i]);
    }
}

/* ---- Tests ---- */

static void test_rgb565_puts_red_low_and_blue_high(void) {
    TEST_ASSERT_EQUAL_HEX16(0x001F, skiff_cover_rgb565(0xFF, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(0x07E0, skiff_cover_rgb565(0, 0xFF, 0));
    TEST_ASSERT_EQUAL_HEX16(0xF800, skiff_cover_rgb565(0, 0, 0xFF));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, skiff_cover_rgb565(0xFF, 0xFF, 0xFF));
    TEST_ASSERT_EQUAL_HEX16(0x0000, skiff_cover_rgb565(0x07, 0x03, 0x07));
}

typedef struct fit_case {
    uint32_t width;
    uint32_t height;
    uint16_t fitted_width;
    uint16_t fitted_height;
} fit_case;

static void test_a_cover_is_fitted_into_the_box_keeping_its_shape(void) {
    static const fit_case CASES[] = {
        {SMALL_COVER_WIDTH, SMALL_COVER_HEIGHT, 160, 213},
        {600, 800, 160, 213},
        {320, 240, 160, 120},
        {SKIFF_COVER_WIDTH, SKIFF_COVER_HEIGHT, SKIFF_COVER_WIDTH, SKIFF_COVER_HEIGHT},
        {161, 220, 160, 219},
        {100, 100, 100, 100},
        {1, 1, 1, 1},
        {SKIFF_COVER_SOURCE_MAX, 1, SKIFF_COVER_WIDTH, 1},
        {1, SKIFF_COVER_SOURCE_MAX, 1, SKIFF_COVER_HEIGHT},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const picture spec = rgb(CASES[i].width, CASES[i].height, teal);
        make_png(&spec);
        TEST_PRINTF("%ux%u", CASES[i].width, CASES[i].height);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, decode());
        TEST_ASSERT_EQUAL_UINT16(CASES[i].fitted_width, cover->width);
        TEST_ASSERT_EQUAL_UINT16(CASES[i].fitted_height, cover->height);
        assert_filled_with(skiff_cover_rgb565(TEAL[0], TEAL[1], TEAL[2]));
    }
}

static void test_every_colour_type_and_depth_decodes_to_the_same_pixels(void) {
    static const picture SPECS[] = {
        {SMALL_COVER_WIDTH, SMALL_COVER_HEIGHT, PNG_COLOR_TYPE_RGB, 8, PNG_INTERLACE_NONE, teal},
        {SMALL_COVER_WIDTH, SMALL_COVER_HEIGHT, PNG_COLOR_TYPE_RGB, 16, PNG_INTERLACE_NONE, teal},
        {SMALL_COVER_WIDTH, SMALL_COVER_HEIGHT, PNG_COLOR_TYPE_RGBA, 8, PNG_INTERLACE_NONE, teal},
        {SMALL_COVER_WIDTH, SMALL_COVER_HEIGHT, PNG_COLOR_TYPE_PALETTE, 8, PNG_INTERLACE_NONE,
         teal},
        {SMALL_COVER_WIDTH, SMALL_COVER_HEIGHT, PNG_COLOR_TYPE_RGB, 8, PNG_INTERLACE_ADAM7, teal},
    };
    for (size_t i = 0; i < sizeof SPECS / sizeof SPECS[0]; i++) {
        make_png(&SPECS[i]);
        TEST_PRINTF("colour type %d, %d bits, interlace %d", SPECS[i].color_type,
                    SPECS[i].bit_depth, SPECS[i].interlace);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, decode());
        TEST_ASSERT_EQUAL_UINT16(160, cover->width);
        TEST_ASSERT_EQUAL_UINT16(213, cover->height);
        assert_filled_with(skiff_cover_rgb565(TEAL[0], TEAL[1], TEAL[2]));
    }
}

static void test_grey_pictures_become_grey_pixels(void) {
    static const picture SPECS[] = {
        {SMALL_COVER_WIDTH, SMALL_COVER_HEIGHT, PNG_COLOR_TYPE_GRAY, 8, PNG_INTERLACE_NONE, grey},
        {SMALL_COVER_WIDTH, SMALL_COVER_HEIGHT, PNG_COLOR_TYPE_GRAY_ALPHA, 8, PNG_INTERLACE_NONE,
         grey},
    };
    for (size_t i = 0; i < sizeof SPECS / sizeof SPECS[0]; i++) {
        make_png(&SPECS[i]);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, decode());
        assert_filled_with(skiff_cover_rgb565(GREY[0], GREY[1], GREY[2]));
    }
    TEST_PRINTF("1-bit grey: black and white columns");
    const picture white = {16, 16, PNG_COLOR_TYPE_GRAY, 1, PNG_INTERLACE_NONE, stripes};
    make_png(&white);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, decode());
    TEST_ASSERT_EQUAL_HEX16(0x0000, pixel_at(0, 0));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, pixel_at(1, 0));
}

static void test_transparency_is_drawn_over_the_background(void) {
    const uint16_t background = skiff_cover_rgb565(
        SKIFF_COVER_BACKGROUND_RED, SKIFF_COVER_BACKGROUND_GREEN, SKIFF_COVER_BACKGROUND_BLUE);
    picture spec = {
        SMALL_COVER_WIDTH, SMALL_COVER_HEIGHT, PNG_COLOR_TYPE_RGBA, 8, PNG_INTERLACE_NONE, clear};
    make_png(&spec);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, decode());
    assert_filled_with(background);
    TEST_PRINTF("half transparent: halfway between");
    spec.sample = half_clear;
    make_png(&spec);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, decode());
    const uint32_t a = HALF_ALPHA;
    assert_filled_with(skiff_cover_rgb565(
        (uint8_t)((TEAL[0] * a + SKIFF_COVER_BACKGROUND_RED * (255 - a) + 127) / 255),
        (uint8_t)((TEAL[1] * a + SKIFF_COVER_BACKGROUND_GREEN * (255 - a) + 127) / 255),
        (uint8_t)((TEAL[2] * a + SKIFF_COVER_BACKGROUND_BLUE * (255 - a) + 127) / 255)));
    TEST_PRINTF("a palette with a transparent entry (tRNS)");
    spec.color_type = PNG_COLOR_TYPE_PALETTE;
    spec.sample = clear;
    make_png(&spec);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, decode());
    assert_filled_with(background);
}

static void test_scaling_averages_the_pixels_it_folds_together(void) {
    const picture spec = rgb(SKIFF_COVER_WIDTH * 2, SKIFF_COVER_HEIGHT * 2, stripes);
    make_png(&spec);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, decode());
    TEST_ASSERT_EQUAL_UINT16(SKIFF_COVER_WIDTH, cover->width);
    TEST_ASSERT_EQUAL_UINT16(SKIFF_COVER_HEIGHT, cover->height);
    /* Two black and two white pixels each: (2 * 255 + 2) / 4 rounds to 128. */
    assert_filled_with(skiff_cover_rgb565(128, 128, 128));
    TEST_PRINTF("a picture that fits is not resampled");
    const picture fits = rgb(2, 2, stripes);
    make_png(&fits);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, decode());
    TEST_ASSERT_EQUAL_HEX16(0x0000, pixel_at(0, 0));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, pixel_at(1, 1));
}

/* The mean of the cover's green channel (6 bits, the most precise one), in hundredths. */
static uint32_t mean_green_x100(void) {
    uint32_t total = 0;
    for (uint32_t y = 0; y < cover->height; y++) {
        for (uint32_t x = 0; x < cover->width; x++) {
            total += (pixel_at(x, y) >> 5) & 0x3F;
        }
    }
    const uint32_t count = (uint32_t)cover->width * cover->height;
    return count == 0 ? 0 : total * 100U / count;
}

static void test_scaling_by_a_fraction_keeps_the_average_brightness(void) {
    /* 240 -> 160 is 1.5 source pixels per output pixel: each output column takes one whole stripe
     * and half of the next, so the columns run 85, 85, 170, 170 and average mid grey. */
    const picture columns = rgb(SMALL_COVER_WIDTH, SMALL_COVER_HEIGHT, stripes);
    make_png(&columns);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, decode());
    const uint16_t dark = skiff_cover_rgb565(85, 85, 85);
    const uint16_t light = skiff_cover_rgb565(170, 170, 170);
    const uint16_t expected[] = {dark, dark, light, light, dark, dark, light, light};
    for (uint32_t y = 0; y < cover->height; y += cover->height - 1) {
        for (uint32_t x = 0; x < sizeof expected / sizeof expected[0]; x++) {
            TEST_ASSERT_EQUAL_HEX16(expected[x], pixel_at(x, y));
        }
    }
    uint32_t mean = mean_green_x100();
    TEST_PRINTF("columns: mean green %u/100 of 63", mean);
    TEST_ASSERT_UINT32_WITHIN(100, 3150, mean);
    TEST_PRINTF("rows, 320 -> 213: an uneven ratio");
    const picture rows = rgb(SMALL_COVER_WIDTH, SMALL_COVER_HEIGHT, row_stripes);
    make_png(&rows);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, decode());
    mean = mean_green_x100();
    TEST_PRINTF("rows: mean green %u/100 of 63", mean);
    TEST_ASSERT_UINT32_WITHIN(100, 3150, mean);
}

static void test_a_picture_too_large_is_refused(void) {
    picture spec = rgb(SKIFF_COVER_SOURCE_MAX + 1, 8, teal);
    make_png(&spec);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_COVER_DAMAGED, decode());
    assert_all_zero();
    spec = rgb(8, SKIFF_COVER_SOURCE_MAX + 1, teal);
    make_png(&spec);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_COVER_DAMAGED, decode());
    TEST_PRINTF("interlaced, it must fit SKIFF_COVER_INTERLACED_MAX whole");
    spec = rgb(512, 513, teal);
    spec.interlace = PNG_INTERLACE_ADAM7;
    make_png(&spec);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_COVER_DAMAGED, decode());
    assert_all_zero();
    spec.height = 512;
    make_png(&spec);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, decode());
}

static void test_what_is_not_a_png_is_another_format(void) {
    static const unsigned char JPEG[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10, 'J', 'F', 'I', 'F', 0};
    static const unsigned char WEBP[] = "RIFF\x24\x00\x00\x00WEBPVP8 ";
    static const unsigned char HTML[] = "<html><body>404 Not Found</body></html>";
    static const unsigned char SHORT[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A};
    const struct {
        const unsigned char *data;
        size_t size;
    } CASES[] = {{JPEG, sizeof JPEG},
                 {WEBP, sizeof WEBP - 1},
                 {HTML, sizeof HTML - 1},
                 {SHORT, sizeof SHORT},
                 {JPEG, 0}};
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        memset(cover, 0x5A, sizeof *cover);
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_COVER_FORMAT,
                              skiff_cover_decode_png(CASES[i].data, CASES[i].size, cover));
        assert_all_zero();
    }
}

static void test_a_cut_png_is_damaged(void) {
    const picture spec = rgb(SMALL_COVER_WIDTH, SMALL_COVER_HEIGHT, noise);
    make_png(&spec);
    const size_t whole = encoded.size;
    TEST_PRINTF("whole: %zu bytes", whole);
    /* Cut inside the header, inside the picture data, and just before IEND. */
    const size_t cuts[] = {8, 20, 33, whole / 2, whole - 13, whole - 1};
    for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
        memset(cover, 0x5A, sizeof *cover);
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_COVER_DAMAGED,
                              skiff_cover_decode_png(encoded.data, cuts[i], cover));
        assert_all_zero();
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_cover_decode_png(encoded.data, whole, cover));
}

static uint32_t read_be32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

/* Gives every whole chunk the CRC-32 of what it now holds, so a changed byte reaches libpng's
 * parsing and the decoder instead of stopping at the CRC check. */
static void repair_crcs(unsigned char *data, size_t size) {
    size_t at = PNG_SIGNATURE_BYTES;
    while (size - at >= CHUNK_LENGTH_BYTES + CHUNK_TYPE_BYTES + CHUNK_CRC_BYTES) {
        const size_t length = read_be32(data + at);
        /* The loop condition keeps this from wrapping. */
        const size_t room = size - at - CHUNK_LENGTH_BYTES - CHUNK_TYPE_BYTES - CHUNK_CRC_BYTES;
        if (length > room) {
            return;
        }
        const size_t covered = CHUNK_TYPE_BYTES + length;
        const uint32_t crc = (uint32_t)crc32(0, data + at + CHUNK_LENGTH_BYTES, (uInt)covered);
        unsigned char *field = data + at + CHUNK_LENGTH_BYTES + covered;
        field[0] = (unsigned char)(crc >> 24);
        field[1] = (unsigned char)(crc >> 16);
        field[2] = (unsigned char)(crc >> 8);
        field[3] = (unsigned char)crc;
        at += CHUNK_LENGTH_BYTES + covered + CHUNK_CRC_BYTES;
    }
}

/* Changes one byte past the signature per round (a changed signature is simply another format);
 * with repair, the chunk CRCs are fixed up after it. libpng only warns about a bad CRC in an
 * ancillary chunk (tRNS), so not every round is refused. Every result is a cover or a refusal,
 * never a crash (ASan runs this too). Returns how many were refused. */
static int corrupt_rounds(const unsigned char *original, int repair) {
    uint32_t state = repair ? 7U : 1U;
    int refused = 0;
    for (int round = 0; round < CORRUPTION_ROUNDS; round++) {
        memcpy(encoded.data, original, encoded.size);
        state = state * LCG_MULTIPLIER + LCG_INCREMENT;
        const size_t at = PNG_SIGNATURE_BYTES + (state >> 8) % (encoded.size - PNG_SIGNATURE_BYTES);
        encoded.data[at] ^= (unsigned char)(1U + (state >> 24) % 255U);
        if (repair) {
            repair_crcs(encoded.data, encoded.size);
        }
        const skiff_err err = skiff_cover_decode_png(encoded.data, encoded.size, cover);
        TEST_ASSERT_TRUE(err == SKIFF_OK || err == SKIFF_ERR_ROMM_COVER_DAMAGED);
        refused += err != SKIFF_OK;
    }
    return refused;
}

static void test_a_changed_byte_never_crashes_the_decoder(void) {
    const picture specs[] = {
        rgb(64, 64, noise),
        {48, 40, PNG_COLOR_TYPE_PALETTE, 8, PNG_INTERLACE_NONE, clear},
        {40, 48, PNG_COLOR_TYPE_RGBA, 8, PNG_INTERLACE_ADAM7, noise},
    };
    for (size_t i = 0; i < sizeof specs / sizeof specs[0]; i++) {
        make_png(&specs[i]);
        unsigned char *original = malloc(encoded.size);
        TEST_ASSERT_NOT_NULL(original);
        memcpy(original, encoded.data, encoded.size);
        const int refused = corrupt_rounds(original, 0);
        const int refused_parsed = corrupt_rounds(original, 1);
        TEST_PRINTF(
            "colour type %d: %d of %d refused at the CRC check, %d of %d with CRCs repaired",
            specs[i].color_type, refused, CORRUPTION_ROUNDS, refused_parsed, CORRUPTION_ROUNDS);
        TEST_ASSERT_GREATER_THAN_INT(0, refused_parsed);
        free(original);
    }
}

static void test_bad_arguments_are_refused(void) {
    const picture spec = rgb(4, 4, teal);
    make_png(&spec);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_cover_decode_png(NULL, 4, cover));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_cover_decode_png(encoded.data, encoded.size, NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rgb565_puts_red_low_and_blue_high);
    RUN_TEST(test_a_cover_is_fitted_into_the_box_keeping_its_shape);
    RUN_TEST(test_every_colour_type_and_depth_decodes_to_the_same_pixels);
    RUN_TEST(test_grey_pictures_become_grey_pixels);
    RUN_TEST(test_transparency_is_drawn_over_the_background);
    RUN_TEST(test_scaling_averages_the_pixels_it_folds_together);
    RUN_TEST(test_scaling_by_a_fraction_keeps_the_average_brightness);
    RUN_TEST(test_a_picture_too_large_is_refused);
    RUN_TEST(test_what_is_not_a_png_is_another_format);
    RUN_TEST(test_a_cut_png_is_damaged);
    RUN_TEST(test_a_changed_byte_never_crashes_the_decoder);
    RUN_TEST(test_bad_arguments_are_refused);
    return UNITY_END();
}
