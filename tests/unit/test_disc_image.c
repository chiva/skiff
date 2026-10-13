/*
 * The structure check of a disc image RomM converted (skiff/disc_image.h), on synthetic CSO, ZSO
 * and ISO files in a temporary directory: whole images pass, and an error page, a cut file, a
 * header that does not fit and an index pointing past the end are refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/disc_image.h"

#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define SECTOR ((size_t)2048)
/* A 64 KiB disc in 2 KiB blocks: 32 blocks, 33 index entries. */
#define DISC_BYTES (32 * SECTOR)
#define BLOCK_BYTES SECTOR
#define BLOCKS (DISC_BYTES / BLOCK_BYTES)
#define HEADER_BYTES ((size_t)24)
#define INDEX_BYTES ((BLOCKS + 1) * (size_t)4)
#define PLAIN_BLOCK_FLAG 0x80000000U
#define IMAGE_MAX (HEADER_BYTES + INDEX_BYTES + DISC_BYTES)
#define ISO_PVD_AT (16 * SECTOR)

static char dir[TEMP_DIR_PATH_MAX];
static char path[TEMP_DIR_PATH_MAX];
static skiff_storage *storage;
static unsigned char image[IMAGE_MAX];
static const unsigned char CSO_MAGIC[] = {'C', 'I', 'S', 'O'};

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    TEST_ASSERT_TRUE(temp_dir_path(dir, "Game.cso.part", path, sizeof path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&storage));
}

void tearDown(void) {
    skiff_storage_destroy(storage);
    temp_dir_remove(dir);
}

static void put_u32(unsigned char *at, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        at[i] = (unsigned char)(value >> (8 * i));
    }
}

static void put_u64(unsigned char *at, uint64_t value) {
    put_u32(at, (uint32_t)value);
    put_u32(at + 4, (uint32_t)(value >> 32));
}

/* A CSO or ZSO as rom-converto writes one: every block stored as it is (the check reads the
 * index, not the blocks), the data right after the index. Returns its size. */
static size_t make_compressed(const char *magic, size_t header_size, unsigned align) {
    memset(image, 0, sizeof image);
    memcpy(image, magic, 4);
    put_u32(image + 4, (uint32_t)header_size);
    put_u64(image + 8, DISC_BYTES);
    put_u32(image + 16, (uint32_t)BLOCK_BYTES);
    image[20] = 1;
    image[21] = (unsigned char)align;
    const size_t data_at = HEADER_BYTES + INDEX_BYTES;
    for (size_t block = 0; block <= BLOCKS; block++) {
        const size_t at = data_at + block * BLOCK_BYTES;
        put_u32(image + HEADER_BYTES + 4 * block,
                (uint32_t)(at >> align) | (block < BLOCKS ? PLAIN_BLOCK_FLAG : 0));
    }
    return data_at + DISC_BYTES;
}

static size_t make_iso(void) {
    memset(image, 0, sizeof image);
    const unsigned char pvd[] = {0x01, 'C', 'D', '0', '0', '1', 0x01};
    memcpy(image + ISO_PVD_AT, pvd, sizeof pvd);
    return DISC_BYTES;
}

static void write_image(size_t size) {
    skiff_file *file = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(storage, path, SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, image, size));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
}

static skiff_err check(size_t size, skiff_romm_format format) {
    write_image(size);
    const skiff_err err = skiff_disc_image_check(storage, path, size, format);
    TEST_PRINTF("%zu bytes as %s: %s", size, skiff_romm_format_name(format), skiff_err_name(err));
    return err;
}

static void test_whole_cso_and_zso_images_pass(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          check(make_compressed("CISO", HEADER_BYTES, 0), SKIFF_ROMM_FORMAT_CSO));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          check(make_compressed("ZISO", HEADER_BYTES, 0), SKIFF_ROMM_FORMAT_ZSO));
    TEST_PRINTF("a header size of 0, as old tools write, and an index shift");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, check(make_compressed("CISO", 0, 0), SKIFF_ROMM_FORMAT_CSO));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          check(make_compressed("CISO", HEADER_BYTES, 2), SKIFF_ROMM_FORMAT_CSO));
}

static void test_the_magic_must_be_the_format_asked_for(void) {
    const size_t size = make_compressed("CISO", HEADER_BYTES, 0);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED, check(size, SKIFF_ROMM_FORMAT_ZSO));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED, check(size, SKIFF_ROMM_FORMAT_ISO));
}

static void test_an_error_page_is_not_a_disc_image(void) {
    static const char PAGE[] = "<html><body>502 Bad Gateway</body></html>";
    memset(image, 0, sizeof image);
    memcpy(image, PAGE, sizeof PAGE - 1);
    for (int f = SKIFF_ROMM_FORMAT_ISO; f < SKIFF_ROMM_FORMAT_COUNT; f++) {
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED,
                              check(sizeof PAGE - 1, (skiff_romm_format)f));
    }
}

static void test_a_cut_cso_is_refused(void) {
    const size_t size = make_compressed("CISO", HEADER_BYTES, 0);
    TEST_PRINTF("the data cut short: the last entry points past the end");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED, check(size - 1, SKIFF_ROMM_FORMAT_CSO));
    TEST_PRINTF("the index cut short");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED,
                          check(HEADER_BYTES + INDEX_BYTES - 1, SKIFF_ROMM_FORMAT_CSO));
    TEST_PRINTF("the header cut short");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED,
                          check(HEADER_BYTES, SKIFF_ROMM_FORMAT_CSO));
}

typedef struct header_case {
    const char *what;
    size_t at;
    uint32_t value;
} header_case;

static void test_a_header_that_does_not_fit_is_refused(void) {
    static const header_case CASES[] = {
        {"header size 32", 4, 32},
        {"disc size 0", 8, 0},
        {"disc size not whole sectors", 8, (uint32_t)DISC_BYTES - 1},
        {"block size 3000", 16, 3000},
        {"block size 1024", 16, 1024},
        {"block size 2 MiB", 16, 2 * 1024 * 1024},
        {"first block inside the index", HEADER_BYTES, (uint32_t)HEADER_BYTES},
        {"last entry before the first", HEADER_BYTES + 4 * BLOCKS,
         (uint32_t)(HEADER_BYTES + INDEX_BYTES - 4)},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const size_t size = make_compressed("CISO", HEADER_BYTES, 0);
        put_u32(image + CASES[i].at, CASES[i].value);
        TEST_PRINTF("%s", CASES[i].what);
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED, check(size, SKIFF_ROMM_FORMAT_CSO));
    }
    TEST_PRINTF("a disc size near 2^64, which must not wrap to no blocks");
    memset(image, 0, sizeof image);
    memcpy(image, CSO_MAGIC, sizeof CSO_MAGIC);
    put_u32(image + 4, (uint32_t)HEADER_BYTES);
    put_u64(image + 8, 0xFFFFFFFFFFFFF800ULL);
    put_u32(image + 16, 4096);
    image[20] = 1;
    put_u32(image + HEADER_BYTES, (uint32_t)(HEADER_BYTES + 4));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED,
                          check(HEADER_BYTES + 4, SKIFF_ROMM_FORMAT_CSO));
    TEST_PRINTF("an index shift of 32");
    const size_t size = make_compressed("CISO", HEADER_BYTES, 0);
    image[21] = 32;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED, check(size, SKIFF_ROMM_FORMAT_CSO));
}

static void test_an_iso_needs_its_volume_descriptor(void) {
    const size_t size = make_iso();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, check(size, SKIFF_ROMM_FORMAT_ISO));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED, check(size, SKIFF_ROMM_FORMAT_CSO));
    TEST_PRINTF("not whole sectors");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED, check(size - 1, SKIFF_ROMM_FORMAT_ISO));
    TEST_PRINTF("cut before the volume descriptor's sector ends");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED,
                          check(ISO_PVD_AT, SKIFF_ROMM_FORMAT_ISO));
    TEST_PRINTF("another descriptor where the primary one should be");
    image[ISO_PVD_AT] = 0xFF;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED, check(size, SKIFF_ROMM_FORMAT_ISO));
}

static void test_a_file_shorter_than_its_stated_size_is_damaged(void) {
    const size_t size = make_iso();
    write_image(ISO_PVD_AT + 3);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CONVERTED_DAMAGED,
                          skiff_disc_image_check(storage, path, size, SKIFF_ROMM_FORMAT_ISO));
}

static void test_arguments(void) {
    const size_t size = make_iso();
    write_image(size);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_disc_image_check(NULL, path, size, SKIFF_ROMM_FORMAT_ISO));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_disc_image_check(storage, NULL, size, SKIFF_ROMM_FORMAT_ISO));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_disc_image_check(storage, path, size, SKIFF_ROMM_FORMAT_STORED));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_disc_image_check(storage, path, size, SKIFF_ROMM_FORMAT_COUNT));
    char missing[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "missing.iso", missing, sizeof missing));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND,
                          skiff_disc_image_check(storage, missing, size, SKIFF_ROMM_FORMAT_ISO));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_whole_cso_and_zso_images_pass);
    RUN_TEST(test_the_magic_must_be_the_format_asked_for);
    RUN_TEST(test_an_error_page_is_not_a_disc_image);
    RUN_TEST(test_a_cut_cso_is_refused);
    RUN_TEST(test_a_header_that_does_not_fit_is_refused);
    RUN_TEST(test_an_iso_needs_its_volume_descriptor);
    RUN_TEST(test_a_file_shorter_than_its_stated_size_is_damaged);
    RUN_TEST(test_arguments);
    return UNITY_END();
}
