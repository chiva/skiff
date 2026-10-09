/*
 * QR codes (skiff/ui.h): the pairing address becomes a code a phone reads back exactly. Each code
 * is drawn as a phone would see it (light quiet zone, a few pixels a module) into a PGM image,
 * which zbarimg (zbar-tools, in the host image) decodes; the text it reads must be the text
 * encoded.
 */
#include <stdio.h>
#include <string.h>

#include "skiff/ui.h"

#include "temp_dir.h"
#include "unity.h"

/* An address as Skiff shows it on the pairing screen. */
#define PAIRING_URL "https://192.168.5.112:8443/pair/device?user_code=SKIFF234"
/* Pixels a module in the test images, as on the PSP's pairing screen. */
#define TEST_SCALE 4
#define PGM_LIGHT 255
#define PGM_DARK 0
/* The finder pattern in each corner but the bottom right: 7 modules a side. */
#define FINDER_SIZE 7
#define DECODED_MAX 512
#define COMMAND_MAX (TEMP_DIR_PATH_MAX + 64)
/* Sizes a side per version: 17 + 4 a version. */
#define VERSION_4_SIZE 33
#define VERSION_10_SIZE 57
/* The pairing screen's corner for the code (SKIFF_APP_QR_WIDTH). */
#define PAIR_AREA_PIXELS 176

static char dir[TEMP_DIR_PATH_MAX];

void setUp(void) { TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir)); }

void tearDown(void) { temp_dir_remove(dir); }

static void print_code(const skiff_ui_qr *qr) {
    char row[SKIFF_UI_QR_SIZE_MAX * 2 + 1];
    for (int y = 0; y < qr->size; y++) {
        for (int x = 0; x < qr->size; x++) {
            const char *cell = skiff_ui_qr_dark(qr, x, y) ? "##" : "  ";
            memcpy(&row[(size_t)x * 2], cell, 2);
        }
        row[(size_t)qr->size * 2] = '\0';
        TEST_PRINTF("|%s|", row);
    }
}

/* Writes qr with its quiet zone into a PGM file and returns what zbarimg reads from it. */
static void decode(const skiff_ui_qr *qr, char *text, size_t text_size) {
    char image[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "code.pgm", image, sizeof image));
    FILE *file = fopen(image, "wb");
    TEST_ASSERT_NOT_NULL(file);
    const int side = (qr->size + 2 * SKIFF_UI_QR_QUIET_ZONE) * TEST_SCALE;
    fprintf(file, "P5\n%d %d\n255\n", side, side);
    for (int py = 0; py < side; py++) {
        for (int px = 0; px < side; px++) {
            const int x = px / TEST_SCALE - SKIFF_UI_QR_QUIET_ZONE;
            const int y = py / TEST_SCALE - SKIFF_UI_QR_QUIET_ZONE;
            fputc(skiff_ui_qr_dark(qr, x, y) ? PGM_DARK : PGM_LIGHT, file);
        }
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));

    char command[COMMAND_MAX];
    snprintf(command, sizeof command, "zbarimg --raw -q '%s'", image);
    /* The command is fixed but for the image's path, a directory this test created. */
    // NOLINTNEXTLINE(cert-env33-c)
    FILE *reader = popen(command, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(reader, "zbarimg (zbar-tools) runs in the host image");
    const size_t length = fread(text, 1, text_size - 1, reader);
    const int status = pclose(reader);
    text[length] = '\0';
    text[strcspn(text, "\n")] = '\0';
    TEST_PRINTF("zbarimg (status %d) read: \"%s\"", status, text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, status, "zbarimg found no QR code in the image");
}

static void test_a_pairing_address_reads_back_exactly(void) {
    skiff_ui_qr qr;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_qr_encode(PAIRING_URL, &qr));
    TEST_PRINTF("\"%s\" (%zu bytes): %d modules a side", PAIRING_URL, strlen(PAIRING_URL), qr.size);
    print_code(&qr);
    TEST_ASSERT_EQUAL_INT(VERSION_4_SIZE, qr.size);
    char text[DECODED_MAX];
    decode(&qr, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING(PAIRING_URL, text);
}

static void test_the_largest_code_reads_back_exactly(void) {
    /* 213 bytes: the most version 10 holds at level M, the largest code Skiff makes. */
    char longest[214];
    for (size_t i = 0; i < sizeof longest - 1; i++) {
        longest[i] = (char)('a' + i % 26);
    }
    longest[sizeof longest - 1] = '\0';
    skiff_ui_qr qr;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_qr_encode(longest, &qr));
    TEST_ASSERT_EQUAL_INT(VERSION_10_SIZE, qr.size);
    char text[DECODED_MAX];
    decode(&qr, text, sizeof text);
    TEST_ASSERT_EQUAL_STRING(longest, text);
}

static void test_text_beyond_the_largest_code_is_refused(void) {
    char too_long[215];
    memset(too_long, 'a', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = '\0';
    skiff_ui_qr qr;
    qr.size = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL, skiff_ui_qr_encode(too_long, &qr));
    TEST_ASSERT_EQUAL_INT(0, qr.size);
    TEST_ASSERT_EQUAL_INT(0, skiff_ui_qr_scale(&qr, PAIR_AREA_PIXELS));
}

static void test_null_is_refused(void) {
    skiff_ui_qr qr;
    qr.size = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_ui_qr_encode(NULL, &qr));
    TEST_ASSERT_EQUAL_INT(0, qr.size);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_ui_qr_encode(PAIRING_URL, NULL));
    TEST_ASSERT_EQUAL_INT(0, skiff_ui_qr_dark(NULL, 0, 0));
    TEST_ASSERT_EQUAL_INT(0, skiff_ui_qr_scale(NULL, PAIR_AREA_PIXELS));
}

static void test_the_corners_carry_finder_patterns_and_the_border_is_light(void) {
    skiff_ui_qr qr;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_qr_encode(PAIRING_URL, &qr));
    const int last = qr.size - FINDER_SIZE;
    const int corners[3][2] = {{0, 0}, {last, 0}, {0, last}};
    for (size_t c = 0; c < 3; c++) {
        const int cx = corners[c][0];
        const int cy = corners[c][1];
        /* A dark ring, a light ring, then a dark 3 by 3 centre. */
        TEST_ASSERT_EQUAL_INT(1, skiff_ui_qr_dark(&qr, cx, cy));
        TEST_ASSERT_EQUAL_INT(1, skiff_ui_qr_dark(&qr, cx + FINDER_SIZE - 1, cy + FINDER_SIZE - 1));
        TEST_ASSERT_EQUAL_INT(0, skiff_ui_qr_dark(&qr, cx + 1, cy + 1));
        TEST_ASSERT_EQUAL_INT(1, skiff_ui_qr_dark(&qr, cx + 3, cy + 3));
    }
    TEST_ASSERT_EQUAL_INT(0, skiff_ui_qr_dark(&qr, -1, 0));
    TEST_ASSERT_EQUAL_INT(0, skiff_ui_qr_dark(&qr, 0, qr.size));
}

static void test_scale_fits_the_code_and_its_quiet_zone(void) {
    skiff_ui_qr qr;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_ui_qr_encode(PAIRING_URL, &qr));
    /* 33 modules and 4 a side of quiet zone: 41 modules, 4 pixels each in 176. */
    TEST_ASSERT_EQUAL_INT(4, skiff_ui_qr_scale(&qr, PAIR_AREA_PIXELS));
    TEST_ASSERT_EQUAL_INT(1, skiff_ui_qr_scale(&qr, 41));
    TEST_ASSERT_EQUAL_INT(0, skiff_ui_qr_scale(&qr, 40));
    TEST_ASSERT_EQUAL_INT(0, skiff_ui_qr_scale(&qr, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_pairing_address_reads_back_exactly);
    RUN_TEST(test_the_largest_code_reads_back_exactly);
    RUN_TEST(test_text_beyond_the_largest_code_is_refused);
    RUN_TEST(test_null_is_refused);
    RUN_TEST(test_the_corners_carry_finder_patterns_and_the_border_is_light);
    RUN_TEST(test_scale_fits_the_code_and_its_quiet_zone);
    return UNITY_END();
}
