/*
 * The cover cache (skiff/cover.h) over a temporary directory through the fake storage: a stored
 * cover comes back pixel for pixel, a slot holding another ROM, server or cover version is a miss,
 * a cut or changed file is damaged rather than drawn, the cache stays at 64 files whatever is
 * stored, and a nearly full Memory Stick gets no new slot files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/cover.h"

#include "fake_storage.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define BASE_URL "https://romm.test"
#define OTHER_BASE_URL "https://other.test"
#define ROM_ID 70U
#define COVER_PATH "/assets/romm/resources/roms/1/70/cover/small.png?ts=2026-10-10 04:26:25"
#define NEWER_COVER_PATH "/assets/romm/resources/roms/1/70/cover/small.png?ts=2026-10-11 09:00:00"
#define COVER_WIDTH 160
#define COVER_HEIGHT 213
/* The slot file's header (src/cover/cache.c). */
#define HEADER_BYTES 36
#define FILE_BYTES (HEADER_BYTES + COVER_HEIGHT * SKIFF_COVER_WIDTH * 2)
#define ROOM_NEEDED (SKIFF_STORAGE_FREE_MARGIN_BYTES + SKIFF_COVER_CACHE_ROOM_BYTES + FILE_BYTES)

static char dir[TEMP_DIR_PATH_MAX];
static skiff_storage *posix;
static fake_storage storage;
static skiff_storage_roots roots;
static skiff_cover *cover;
static skiff_cover *loaded;
static skiff_cover_key key;

/* A cover whose every pixel differs from its neighbours, so a misplaced row or byte shows. */
static void fill_cover(skiff_cover *out, uint16_t width, uint16_t height, uint16_t seed) {
    memset(out, 0, sizeof *out);
    out->width = width;
    out->height = height;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < SKIFF_COVER_WIDTH; x++) {
            out->pixels[y * SKIFF_COVER_WIDTH + x] =
                x < width ? (uint16_t)(seed + y * 997U + x * 31U) : 0;
        }
    }
}

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    char app[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "PSP/GAME/Skiff", app, sizeof app));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_init(dir, app, &roots));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&posix));
    fake_storage_init(&storage, posix);
    cover = malloc(sizeof *cover);
    loaded = malloc(sizeof *loaded);
    TEST_ASSERT_NOT_NULL(cover);
    TEST_ASSERT_NOT_NULL(loaded);
    fill_cover(cover, COVER_WIDTH, COVER_HEIGHT, 1);
    key = skiff_cover_key_of(BASE_URL, ROM_ID, COVER_PATH);
}

void tearDown(void) {
    free(loaded);
    free(cover);
    skiff_storage_destroy(&storage.base);
    skiff_storage_destroy(posix);
    temp_dir_remove(dir);
}

/* The real path of slot file number slot. */
static void slot_file(unsigned slot, char *out, size_t size) {
    char logical[64];
    snprintf(logical, sizeof logical, SKIFF_COVER_CACHE_FOLDER "/%u.cov", slot);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, logical, out, size));
}

static void slot_path(uint64_t rom_id, char *out, size_t size) {
    slot_file((unsigned)(rom_id % SKIFF_COVER_CACHE_SLOTS), out, size);
}

static skiff_err load(const skiff_cover_key *wanted) {
    memset(loaded, 0x5A, sizeof *loaded);
    const skiff_err err = skiff_cover_cache_load(&storage.base, &roots, wanted, loaded);
    TEST_PRINTF("load ROM %llu -> %s, %ux%u", (unsigned long long)wanted->rom_id,
                skiff_err_name(err), loaded->width, loaded->height);
    return err;
}

static void assert_loaded_is(const skiff_cover *expected) {
    TEST_ASSERT_EQUAL_UINT16(expected->width, loaded->width);
    TEST_ASSERT_EQUAL_UINT16(expected->height, loaded->height);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(expected->pixels, loaded->pixels, SKIFF_COVER_PIXELS);
}

static void assert_loaded_empty(void) {
    TEST_ASSERT_EQUAL_UINT16(0, loaded->width);
    TEST_ASSERT_EQUAL_UINT16(0, loaded->height);
    TEST_ASSERT_EACH_EQUAL_HEX16(0, loaded->pixels, SKIFF_COVER_PIXELS);
}

/* Rewrites a slot file through the real storage, as a power cut or another program would. */
static void rewrite_slot(uint64_t rom_id, size_t keep, long flip_at) {
    char path[SKIFF_STORAGE_PATH_MAX];
    slot_path(rom_id, path, sizeof path);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    static unsigned char bytes[FILE_BYTES + 1];
    const size_t size = fread(bytes, 1, sizeof bytes, file);
    fclose(file);
    TEST_ASSERT_EQUAL_size_t(FILE_BYTES, size);
    if (flip_at >= 0) {
        bytes[flip_at] ^= 0x01;
    }
    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(keep, fwrite(bytes, 1, keep, file));
    fclose(file);
}

/* ---- Tests ---- */

static void test_a_stored_cover_comes_back_pixel_for_pixel(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    TEST_PRINTF("%d write(s), %llu bytes, %d sync(s)", storage.writes,
                (unsigned long long)storage.bytes_written, storage.syncs);
    TEST_ASSERT_EQUAL_INT(1, storage.writes);
    TEST_ASSERT_EQUAL_UINT64(FILE_BYTES, storage.bytes_written);
    TEST_ASSERT_EQUAL_INT(0, storage.syncs);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load(&key));
    assert_loaded_is(cover);
    TEST_PRINTF("a narrow, short cover too");
    fill_cover(cover, 1, 1, 0xBEEF);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load(&key));
    assert_loaded_is(cover);
}

static void test_an_empty_slot_or_another_cover_is_a_miss(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, load(&key));
    assert_loaded_empty();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    const skiff_cover_key others[] = {
        skiff_cover_key_of(BASE_URL, ROM_ID + SKIFF_COVER_CACHE_SLOTS, COVER_PATH),
        skiff_cover_key_of(OTHER_BASE_URL, ROM_ID, COVER_PATH),
        skiff_cover_key_of(BASE_URL, ROM_ID, NEWER_COVER_PATH),
    };
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
        TEST_PRINTF("another ROM in the slot, another server, a newer cover: miss %zu", i);
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, load(&others[i]));
        assert_loaded_empty();
    }
    TEST_PRINTF("the ROM sharing the slot evicts it");
    fill_cover(cover, COVER_WIDTH, COVER_HEIGHT, 7);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_cover_cache_store(&storage.base, &roots, &others[0], cover));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load(&others[0]));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, load(&key));
}

static void test_a_cut_or_changed_file_is_damaged_not_drawn(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    const struct {
        size_t keep;
        long flip_at;
        const char *what;
    } CASES[] = {
        {0, -1, "empty"},
        {HEADER_BYTES - 1, -1, "cut in the header"},
        {HEADER_BYTES + 100, -1, "cut in the pixels"},
        {FILE_BYTES - 1, -1, "one byte short"},
        {FILE_BYTES, 0, "magic changed"},
        {FILE_BYTES, 4, "version changed"},
        {FILE_BYTES, 10, "box size changed"},
        {FILE_BYTES, 6, "width changed"},
        {FILE_BYTES, FILE_BYTES - 1, "last pixel changed"},
        {FILE_BYTES, 33, "CRC changed"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                              skiff_cover_cache_store(&storage.base, &roots, &key, cover));
        rewrite_slot(ROM_ID, CASES[i].keep, CASES[i].flip_at);
        TEST_PRINTF("%s", CASES[i].what);
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_COVER_DAMAGED, load(&key));
        assert_loaded_empty();
    }
    TEST_PRINTF("a byte after the pixels");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    char path[SKIFF_STORAGE_PATH_MAX];
    slot_path(ROM_ID, path, sizeof path);
    FILE *file = fopen(path, "ab");
    TEST_ASSERT_NOT_NULL(file);
    fputc(0, file);
    fclose(file);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_COVER_DAMAGED, load(&key));
    TEST_PRINTF("storing again repairs the slot");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load(&key));
    assert_loaded_is(cover);
}

static void test_the_cache_never_holds_more_than_its_slots(void) {
    for (uint64_t rom_id = 1; rom_id <= (uint64_t)SKIFF_COVER_CACHE_SLOTS * 3; rom_id++) {
        const skiff_cover_key each = skiff_cover_key_of(BASE_URL, rom_id, COVER_PATH);
        fill_cover(cover, COVER_WIDTH, COVER_HEIGHT, (uint16_t)rom_id);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                              skiff_cover_cache_store(&storage.base, &roots, &each, cover));
    }
    int files = 0;
    for (unsigned slot = 0; slot < SKIFF_COVER_CACHE_SLOTS + 8; slot++) {
        char path[SKIFF_STORAGE_PATH_MAX];
        uint64_t size = 0;
        slot_file(slot, path, sizeof path);
        files += skiff_storage_size(&storage.base, path, &size) == SKIFF_OK;
    }
    TEST_PRINTF("%d ROMs stored, %d slot files", SKIFF_COVER_CACHE_SLOTS * 3, files);
    TEST_ASSERT_EQUAL_INT(SKIFF_COVER_CACHE_SLOTS, files);
    TEST_PRINTF("the last ROM of each slot is the one kept");
    const skiff_cover_key last =
        skiff_cover_key_of(BASE_URL, (uint64_t)SKIFF_COVER_CACHE_SLOTS * 3, COVER_PATH);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load(&last));
    assert_loaded_is(cover);
}

static void test_a_nearly_full_stick_gets_no_new_slot_file(void) {
    storage.has_free_bytes = 1;
    storage.free_bytes = ROOM_NEEDED - 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE,
                          skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    TEST_ASSERT_EQUAL_INT(0, storage.writes);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, load(&key));
    TEST_PRINTF("with just enough room it is written");
    storage.free_bytes = ROOM_NEEDED;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    TEST_PRINTF("replacing an existing slot takes no more room, so it is not checked");
    storage.free_bytes = 0;
    const int queries = storage.free_space_queries;
    fill_cover(cover, COVER_WIDTH, COVER_HEIGHT, 9);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    TEST_ASSERT_EQUAL_INT(queries, storage.free_space_queries);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load(&key));
    assert_loaded_is(cover);
}

static void test_a_device_that_cannot_tell_its_space_still_caches(void) {
    storage.free_space_error = SKIFF_ERR_NOT_IMPLEMENTED;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load(&key));
}

static void test_storage_failures_come_back_as_their_codes(void) {
    storage.mkdir_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO,
                          skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    storage.mkdir_error = SKIFF_OK;
    TEST_PRINTF("the Memory Stick fills up mid-write: a cut file, read as damaged");
    storage.write_budget = 1000;
    storage.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE,
                          skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    storage.write_error = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_COVER_DAMAGED, load(&key));
    TEST_PRINTF("a size query that fails");
    storage.size_failures = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO,
                          skiff_cover_cache_store(&storage.base, &roots, &key, cover));
}

static void test_bad_arguments_are_refused(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_cover_cache_store(NULL, &roots, &key, cover));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_cover_cache_store(&storage.base, NULL, &key, cover));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_cover_cache_store(&storage.base, &roots, NULL, cover));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_cover_cache_store(&storage.base, &roots, &key, NULL));
    const uint16_t bad[][2] = {
        {0, 1}, {1, 0}, {SKIFF_COVER_WIDTH + 1, 1}, {1, SKIFF_COVER_HEIGHT + 1}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cover->width = bad[i][0];
        cover->height = bad[i][1];
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                              skiff_cover_cache_store(&storage.base, &roots, &key, cover));
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_cover_cache_load(NULL, &roots, &key, loaded));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_cover_cache_load(&storage.base, NULL, &key, loaded));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_cover_cache_load(&storage.base, &roots, NULL, loaded));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_cover_cache_load(&storage.base, &roots, &key, NULL));
}

static void test_keys_follow_the_server_rom_and_cover_path(void) {
    const skiff_cover_key a = skiff_cover_key_of(BASE_URL, ROM_ID, COVER_PATH);
    const skiff_cover_key b = skiff_cover_key_of(BASE_URL, ROM_ID, COVER_PATH);
    TEST_ASSERT_EQUAL_UINT64(a.rom_id, b.rom_id);
    TEST_ASSERT_EQUAL_HEX32(a.server, b.server);
    TEST_ASSERT_EQUAL_HEX32(a.cover_path, b.cover_path);
    TEST_ASSERT_NOT_EQUAL(a.server, skiff_cover_key_of(OTHER_BASE_URL, ROM_ID, COVER_PATH).server);
    TEST_ASSERT_NOT_EQUAL(a.cover_path,
                          skiff_cover_key_of(BASE_URL, ROM_ID, NEWER_COVER_PATH).cover_path);
    const skiff_cover_key none = skiff_cover_key_of(NULL, ROM_ID, NULL);
    TEST_ASSERT_EQUAL_HEX32(0, none.server);
    TEST_ASSERT_EQUAL_HEX32(0, none.cover_path);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_stored_cover_comes_back_pixel_for_pixel);
    RUN_TEST(test_an_empty_slot_or_another_cover_is_a_miss);
    RUN_TEST(test_a_cut_or_changed_file_is_damaged_not_drawn);
    RUN_TEST(test_the_cache_never_holds_more_than_its_slots);
    RUN_TEST(test_a_nearly_full_stick_gets_no_new_slot_file);
    RUN_TEST(test_a_device_that_cannot_tell_its_space_still_caches);
    RUN_TEST(test_storage_failures_come_back_as_their_codes);
    RUN_TEST(test_bad_arguments_are_refused);
    RUN_TEST(test_keys_follow_the_server_rom_and_cover_path);
    return UNITY_END();
}
