/*
 * Installing games (skiff/install.h) over a temporary directory: which RomM files the PSP installer
 * takes, where a download goes without ever replacing a file Skiff did not install, and the
 * installed.json manifest that records what Skiff put there, through its round trip, damage, cap,
 * reconcile and a save cut short.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/install.h"

#include "fake_storage.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define ROM_ID 12U
#define OTHER_ROM_ID 34U
#define GAME_SIZE 1048576U
#define GAME_CRC32 0x0A1B2C3DU
#define INSTALLED_MS 1790812800123LL
#define TEXT_MAX ((size_t)4096)

static char dir[TEMP_DIR_PATH_MAX];
static char manifest_path[SKIFF_STORAGE_PATH_MAX + sizeof SKIFF_INSTALL_MANIFEST_NAME + 1];
static skiff_storage *posix;
static fake_storage storage;
static skiff_storage_roots roots;
static skiff_install_manifest *manifest;
static const skiff_installer *psp;
static skiff_romm_rom_summary rom;
static skiff_romm_file file;
static skiff_install_plan plan;

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    char app[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "PSP/GAME/Skiff", app, sizeof app));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_init(dir, app, &roots));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&posix));
    fake_storage_init(&storage, posix);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(&storage.base, roots.games));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(&storage.base, roots.app));
    snprintf(manifest_path, sizeof manifest_path, "%s/" SKIFF_INSTALL_MANIFEST_NAME, roots.app);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_create(&manifest));
    psp = skiff_install_find_installer("psp");
    TEST_ASSERT_NOT_NULL(psp);
    memset(&rom, 0, sizeof rom);
    rom.id = ROM_ID;
    rom.size = GAME_SIZE;
    rom.has_crc32 = 1;
    rom.crc32 = GAME_CRC32;
    snprintf(rom.name, sizeof rom.name, "Game");
    snprintf(rom.fs_name, sizeof rom.fs_name, "Game.iso");
    memset(&file, 0, sizeof file);
    snprintf(file.file_name, sizeof file.file_name, "Game.iso");
    file.size = GAME_SIZE;
    memset(&plan, 0xAB, sizeof plan);
}

void tearDown(void) {
    skiff_install_manifest_destroy(manifest);
    skiff_storage_destroy(&storage.base);
    skiff_storage_destroy(posix);
    temp_dir_remove(dir);
}

/* A file of size bytes at the logical path. */
static void put_file_of(const char *logical, size_t size) {
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, logical, path, sizeof path));
    unsigned char *data = calloc(1, size);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_replace_whole(posix, path, data, size));
    free(data);
    TEST_PRINTF("file on the Memory Stick: %s (%zu bytes)", logical, size);
}

/* A file of the size the records here carry, as if Skiff (or a hand copy) put it there. */
static void put_file(const char *logical) { put_file_of(logical, GAME_SIZE); }

static skiff_install_record record_of(uint64_t rom_id, const char *file_name, const char *logical) {
    skiff_install_record record;
    memset(&record, 0, sizeof record);
    record.rom_id = rom_id;
    snprintf(record.file_name, sizeof record.file_name, "%s", file_name);
    snprintf(record.path, sizeof record.path, "%s", logical);
    record.size = GAME_SIZE;
    record.has_crc32 = 1;
    record.crc32 = GAME_CRC32;
    record.installed_ms = INSTALLED_MS;
    return record;
}

static void add_record(uint64_t rom_id, const char *file_name, const char *logical) {
    const skiff_install_record record = record_of(rom_id, file_name, logical);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_record(manifest, &record));
    TEST_PRINTF("recorded: rom %llu %s at %s", (unsigned long long)rom_id, file_name, logical);
}

/* Promised to a download waiting in the queue. */
static int queued_game_iso(void *ctx, const char *logical) {
    (void)ctx;
    return strcmp(logical, "games:/Game.iso") == 0;
}

static skiff_err plan_with(skiff_install_taken_fn taken) {
    const skiff_err err = skiff_install_plan_download(psp, &rom, &file, &roots, manifest,
                                                      &storage.base, taken, NULL, &plan);
    TEST_PRINTF("plan %s -> %s: %s (renamed %d, replaces own %d)", file.file_name,
                skiff_err_name(err), plan.logical_path, plan.renamed, plan.replaces_own);
    return err;
}

static skiff_err plan_download(void) { return plan_with(NULL); }

static void assert_planned(const char *logical, int renamed, int replaces_own) {
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_OK), skiff_err_name(plan_download()));
    TEST_ASSERT_EQUAL_STRING(logical, plan.logical_path);
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, logical, path, sizeof path));
    TEST_ASSERT_EQUAL_STRING(path, plan.path);
    TEST_ASSERT_EQUAL_INT(renamed, plan.renamed);
    TEST_ASSERT_EQUAL_INT(replaces_own, plan.replaces_own);
}

static size_t read_manifest(char *out, size_t out_size) {
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_storage_read_whole(posix, manifest_path, out, out_size - 1, &length));
    out[length] = '\0';
    return length;
}

static void write_manifest(const char *text) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_replace_whole(posix, manifest_path, text, strlen(text)));
}

static skiff_install_load_result load(void) {
    skiff_install_load_result result = SKIFF_INSTALL_LOADED;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_install_manifest_load(manifest, &storage.base, manifest_path, &result));
    TEST_PRINTF("load -> result %d, %zu record(s)", (int)result, manifest->count);
    return result;
}

/* ---- Installers ---- */

static void test_the_psp_installer_is_found_by_slug(void) {
    TEST_ASSERT_EQUAL_PTR(psp, skiff_install_find_installer("PSP"));
    TEST_ASSERT_EQUAL_STRING(SKIFF_STORAGE_ROOT_GAMES, psp->target_dir);
    TEST_ASSERT_NULL(psp->post_install);
    TEST_ASSERT_NULL(skiff_install_find_installer("nes"));
    TEST_ASSERT_NULL(skiff_install_find_installer(NULL));
}

static void test_iso_cso_and_zso_are_supported_in_any_case(void) {
    const char *supported[] = {"Game.iso", "Game.CSO", "game.ZsO", "a.b.iso"};
    for (size_t i = 0; i < sizeof supported / sizeof supported[0]; i++) {
        snprintf(file.file_name, sizeof file.file_name, "%s", supported[i]);
        TEST_PRINTF("%s", supported[i]);
        TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_SUPPORTED, skiff_install_check(psp, &rom, &file));
    }
    const char *refused[] = {"Game.zip", "EBOOT.PBP", "Game", ".iso", "Game.iso.part"};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        snprintf(file.file_name, sizeof file.file_name, "%s", refused[i]);
        TEST_PRINTF("%s", refused[i]);
        TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_UNKNOWN_EXTENSION,
                              skiff_install_check(psp, &rom, &file));
    }
}

static void test_folders_and_unusable_names_are_not_installable(void) {
    rom.multiple_files = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_MULTIPLE_FILES, skiff_install_check(psp, &rom, &file));
    rom.multiple_files = 0;
    file.name_status = SKIFF_ROMM_NAME_CONTROL_CHAR;
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_UNUSABLE_NAME, skiff_install_check(psp, &rom, &file));
    file.name_status = SKIFF_ROMM_NAME_OK;
    rom.name_status = SKIFF_ROMM_NAME_TOO_LONG;
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_UNUSABLE_NAME, skiff_install_check(psp, &rom, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_UNKNOWN_EXTENSION, skiff_install_check(NULL, &rom, &file));
    TEST_PRINTF("planning refuses what the check refuses");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, plan_download());
    TEST_ASSERT_EQUAL_STRING("", plan.logical_path);
}

/* ---- Planning a download ---- */

static void test_a_new_game_goes_to_its_safe_name_in_iso(void) {
    assert_planned("games:/Game.iso", 0, 0);
    snprintf(file.file_name, sizeof file.file_name, "Disc 1: The *Best*.iso");
    assert_planned("games:/Disc 1_ The _Best_.iso", 0, 0);
}

static void test_a_file_copied_by_hand_is_never_replaced(void) {
    put_file("games:/Game.iso");
    assert_planned("games:/Game [12].iso", 1, 0);
}

static void test_another_rom_of_the_same_name_ignoring_case_gets_the_id(void) {
    TEST_PRINTF("FAT ignores case: GAME.iso of another ROM is the same file as Game.iso");
    add_record(OTHER_ROM_ID, "GAME.iso", "games:/GAME.iso");
    put_file("games:/GAME.iso");
    assert_planned("games:/Game [12].iso", 1, 0);
}

static void test_names_that_clean_to_the_same_name_do_not_collide(void) {
    add_record(OTHER_ROM_ID, "a/b.iso", "games:/a_b.iso");
    put_file("games:/a_b.iso");
    snprintf(file.file_name, sizeof file.file_name, "a:b.iso");
    assert_planned("games:/a_b [12].iso", 1, 0);
}

static void test_a_record_of_another_rom_holds_its_name_even_without_the_file(void) {
    add_record(OTHER_ROM_ID, "Game.iso", "games:/Game.iso");
    assert_planned("games:/Game [12].iso", 1, 0);
}

static void test_skiffs_own_copy_is_replaced_in_place(void) {
    add_record(ROM_ID, "Game.iso", "games:/Game.iso");
    put_file("games:/Game.iso");
    assert_planned("games:/Game.iso", 0, 1);
}

static void test_a_recorded_copy_replaced_outside_skiff_is_no_longer_its_own(void) {
    TEST_PRINTF("Skiff installed Game.iso, then someone put another file of another size there");
    add_record(ROM_ID, "Game.iso", "games:/Game.iso");
    put_file_of("games:/Game.iso", 1);
    assert_planned("games:/Game [12].iso", 1, 0);
}

static void test_a_full_manifest_refuses_a_download_it_could_not_record(void) {
    for (uint64_t i = 1; i <= SKIFF_INSTALL_RECORDS_MAX; i++) {
        char name[32];
        char logical[48];
        snprintf(name, sizeof name, "Other %llu.iso", (unsigned long long)i);
        snprintf(logical, sizeof logical, "games:/%s", name);
        const skiff_install_record record = record_of(1000 + i, name, logical);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_record(manifest, &record));
    }
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_BUFFER_TOO_SMALL),
                             skiff_err_name(plan_download()));
    TEST_ASSERT_EQUAL_STRING("", plan.logical_path);
    TEST_PRINTF("a ROM already recorded can still be downloaded again");
    rom.id = 1001;
    snprintf(file.file_name, sizeof file.file_name, "Other 1.iso");
    assert_planned("games:/Other 1.iso", 0, 0);
}

static void test_a_rom_id_the_manifest_cannot_store_is_refused(void) {
    rom.id = SKIFF_INSTALL_ROM_ID_MAX + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, plan_download());
}

static void test_skiffs_own_copy_keeps_its_renamed_place(void) {
    TEST_PRINTF(
        "installed as Game [12].iso earlier; the plain name is free now, but the copy stays");
    add_record(ROM_ID, "Game.iso", "games:/Game [12].iso");
    put_file("games:/Game [12].iso");
    assert_planned("games:/Game [12].iso", 0, 1);
}

static void test_a_recorded_copy_deleted_meanwhile_is_downloaded_again_to_its_place(void) {
    add_record(ROM_ID, "Game.iso", "games:/Game [12].iso");
    assert_planned("games:/Game [12].iso", 0, 0);
}

static void test_both_names_taken_by_foreign_files_is_refused(void) {
    put_file("games:/Game.iso");
    put_file("games:/Game [12].iso");
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_STORAGE_NAME_TAKEN),
                             skiff_err_name(plan_download()));
    TEST_ASSERT_EQUAL_STRING("", plan.logical_path);
    TEST_ASSERT_EQUAL_STRING("", plan.path);
}

static void test_a_name_promised_to_a_queued_download_is_avoided(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, plan_with(queued_game_iso));
    TEST_ASSERT_EQUAL_STRING("games:/Game [12].iso", plan.logical_path);
    TEST_ASSERT_EQUAL_INT(1, plan.renamed);
}

static void test_a_long_name_is_shortened_between_characters_to_fit_the_id(void) {
    char name[SKIFF_ROMM_FILE_NAME_MAX];
    /* 'é' is two bytes; enough of them that the id only fits once the name is shortened. */
    size_t used = 0;
    while (used + 2 < SKIFF_STORAGE_NAME_MAX - 6) {
        name[used++] = (char)0xC3;
        name[used++] = (char)0xA9;
    }
    memcpy(name + used, ".iso", 5);
    snprintf(file.file_name, sizeof file.file_name, "%s", name);
    char safe[SKIFF_STORAGE_NAME_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_safe_name(name, safe, sizeof safe));
    char logical[SKIFF_STORAGE_PATH_MAX];
    snprintf(logical, sizeof logical, "games:/%s", safe);
    put_file(logical);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, plan_download());
    const char *planned_name = plan.logical_path + strlen("games:/");
    const size_t length = strlen(planned_name);
    TEST_PRINTF("%zu bytes: ...%s", length, planned_name + length - 16);
    TEST_ASSERT_TRUE(length < SKIFF_STORAGE_NAME_MAX);
    TEST_ASSERT_EQUAL_STRING(" [12].iso", planned_name + length - strlen(" [12].iso"));
    TEST_PRINTF("the name before the id ends on a whole character");
    TEST_ASSERT_EQUAL_HEX8(0xA9, (unsigned char)planned_name[length - strlen(" [12].iso") - 1]);
    TEST_ASSERT_EQUAL_INT(1, plan.renamed);
}

static void test_an_unreadable_memory_stick_stops_the_plan(void) {
    storage.fail_suffix = "Game.iso";
    storage.size_failures = 1;
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_STORAGE_IO), skiff_err_name(plan_download()));
    TEST_ASSERT_EQUAL_STRING("", plan.logical_path);
}

static void test_planning_refuses_null_arguments(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_install_plan_download(NULL, &rom, &file, &roots, manifest,
                                                      &storage.base, NULL, NULL, &plan));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_install_plan_download(psp, &rom, &file, &roots, NULL, &storage.base,
                                                      NULL, NULL, &plan));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_install_plan_download(psp, &rom, &file, &roots, manifest, NULL, NULL, NULL, &plan));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_install_plan_download(psp, &rom, &file, &roots, manifest,
                                                      &storage.base, NULL, NULL, NULL));
}

/* ---- Library status ---- */

static void test_the_library_shows_installed_and_changed_games(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_NOT_INSTALLED, skiff_install_state_of(manifest, &rom));
    add_record(ROM_ID, "Game.iso", "games:/Game.iso");
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_INSTALLED, skiff_install_state_of(manifest, &rom));
    rom.size = GAME_SIZE + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_CHANGED, skiff_install_state_of(manifest, &rom));
    rom.size = GAME_SIZE;
    rom.crc32 = GAME_CRC32 + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_CHANGED, skiff_install_state_of(manifest, &rom));
    TEST_PRINTF("without a CRC-32 on one side, the size decides");
    rom.has_crc32 = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_INSTALLED, skiff_install_state_of(manifest, &rom));
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_NOT_INSTALLED, skiff_install_state_of(NULL, &rom));
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_NOT_INSTALLED, skiff_install_state_of(manifest, NULL));
}

/* ---- The manifest ---- */

static void test_the_manifest_round_trips_through_the_memory_stick(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_NO_MANIFEST, load());
    add_record(ROM_ID, "Caf\xC3\xA9 \"Quoted\" \\.iso", "games:/Caf\xC3\xA9 _Quoted_ _.iso");
    skiff_install_record no_crc = record_of(OTHER_ROM_ID, "Other.cso", "games:/Other.cso");
    no_crc.has_crc32 = 0;
    no_crc.crc32 = 0;
    no_crc.installed_ms = 0;
    no_crc.size = SKIFF_STORAGE_MAX_FILE_BYTES;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_record(manifest, &no_crc));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_install_manifest_save(manifest, &storage.base, manifest_path));
    char text[TEXT_MAX];
    read_manifest(text, sizeof text);
    TEST_PRINTF("installed.json: %s", text);
    TEST_ASSERT_NOT_NULL(strstr(text, "\"crc32\":\"0a1b2c3d\""));
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_LOADED, load());
    TEST_ASSERT_EQUAL_size_t(2, manifest->count);
    const skiff_install_record *first =
        skiff_install_manifest_find(manifest, ROM_ID, "Caf\xC3\xA9 \"Quoted\" \\.iso");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_STRING("games:/Caf\xC3\xA9 _Quoted_ _.iso", first->path);
    TEST_ASSERT_EQUAL_UINT64(GAME_SIZE, first->size);
    TEST_ASSERT_TRUE(first->has_crc32);
    TEST_ASSERT_EQUAL_HEX32(GAME_CRC32, first->crc32);
    TEST_ASSERT_EQUAL_INT64(INSTALLED_MS, first->installed_ms);
    const skiff_install_record *second = skiff_install_manifest_find_rom(manifest, OTHER_ROM_ID);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_FALSE(second->has_crc32);
    TEST_ASSERT_EQUAL_UINT64(SKIFF_STORAGE_MAX_FILE_BYTES, second->size);
    TEST_ASSERT_EQUAL_PTR(second, skiff_install_manifest_find_path(manifest, "GAMES:/other.CSO"));
}

static void test_unknown_fields_are_ignored(void) {
    write_manifest("{\"version\":1,\"future\":[1,2],\"installed\":[{\"rom_id\":12,"
                   "\"file_name\":\"Game.iso\",\"path\":\"games:/Game.iso\",\"size\":5,"
                   "\"cover\":\"x.png\"}]}\n");
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_LOADED, load());
    TEST_ASSERT_EQUAL_size_t(1, manifest->count);
    TEST_ASSERT_EQUAL_INT64(0, manifest->records[0].installed_ms);
}

static void test_a_damaged_manifest_loads_empty_and_is_replaced_on_save(void) {
    const char *damaged[] = {
        "{\"version\":1,\"installed\":[",
        "not json",
        "{\"version\":2,\"installed\":[]}",
        "{\"version\":1,\"installed\":[]} trailing",
        "{\"version\":1,\"installed\":[{\"rom_id\":12,\"file_name\":\"a.iso\",\"path\":"
        "\"ms0:/ISO/a.iso\",\"size\":1}]}",
        "{\"version\":1,\"installed\":[{\"rom_id\":1000000000000000,\"file_name\":\"a.iso\","
        "\"path\":\"games:/a.iso\",\"size\":1}]}",
        "{\"version\":1,\"installed\":[{\"rom_id\":1,\"file_name\":\"a.iso\",\"path\":"
        "\"games:/a.iso\",\"size\":4294967296}]}",
        "{\"version\":1,\"installed\":[{\"rom_id\":1,\"file_name\":\"a.iso\",\"path\":"
        "\"games:/a.iso\",\"size\":1,\"crc32\":\"xyz\"}]}",
        "{\"version\":1,\"installed\":[{\"rom_id\":1,\"file_name\":\"a\\u0000b.iso\",\"path\":"
        "\"games:/a.iso\",\"size\":1}]}",
        "{\"version\":1,\"installed\":[{\"rom_id\":1,\"path\":\"games:/a.iso\",\"size\":1}]}",
        "{\"version\":1,\"installed\":[{\"rom_id\":1,\"file_name\":\"a.iso\",\"path\":"
        "\"games:/a.iso\",\"size\":1},{\"rom_id\":2,\"file_name\":\"b.iso\",\"path\":"
        "\"GAMES:/A.ISO\",\"size\":1}]}",
        "{\"version\":1,\"installed\":[{\"rom_id\":1,\"file_name\":\"a.iso\",\"path\":"
        "\"games:/a.iso\",\"size\":1},{\"rom_id\":1,\"file_name\":\"a.iso\",\"path\":"
        "\"games:/b.iso\",\"size\":1}]}",
    };
    for (size_t i = 0; i < sizeof damaged / sizeof damaged[0]; i++) {
        TEST_PRINTF("damaged: %s", damaged[i]);
        write_manifest(damaged[i]);
        TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_DAMAGED, load());
        TEST_ASSERT_EQUAL_size_t(0, manifest->count);
    }
    add_record(ROM_ID, "Game.iso", "games:/Game.iso");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_install_manifest_save(manifest, &storage.base, manifest_path));
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_LOADED, load());
    TEST_ASSERT_EQUAL_size_t(1, manifest->count);
}

static void test_a_manifest_too_large_or_too_dense_is_damaged(void) {
    char *big = malloc(SKIFF_INSTALL_MANIFEST_BYTES_MAX + 2);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, ' ', SKIFF_INSTALL_MANIFEST_BYTES_MAX + 1);
    big[SKIFF_INSTALL_MANIFEST_BYTES_MAX + 1] = '\0';
    write_manifest(big);
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_DAMAGED, load());
    TEST_PRINTF("a small file made of nothing but structure is refused before cJSON parses it");
    size_t used = 0;
    used += (size_t)snprintf(big, 64, "{\"version\":1,\"installed\":[],\"x\":[");
    while (used < (size_t)SKIFF_INSTALL_RECORDS_MAX * 64) {
        big[used++] = '[';
    }
    big[used] = '\0';
    write_manifest(big);
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_DAMAGED, load());
    free(big);
}

static void test_the_manifest_holds_at_most_its_cap(void) {
    for (uint64_t i = 1; i <= SKIFF_INSTALL_RECORDS_MAX; i++) {
        char name[32];
        char logical[48];
        snprintf(name, sizeof name, "Game %llu.iso", (unsigned long long)i);
        snprintf(logical, sizeof logical, "games:/%s", name);
        const skiff_install_record record = record_of(i, name, logical);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_record(manifest, &record));
    }
    TEST_PRINTF("%zu records", manifest->count);
    skiff_install_record extra =
        record_of(SKIFF_INSTALL_RECORDS_MAX + 1, "Extra.iso", "games:/Extra.iso");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_install_manifest_record(manifest, &extra));
    TEST_PRINTF("replacing a record still works when full");
    skiff_install_record again = record_of(1, "Game 1.iso", "games:/Game 1.iso");
    again.size = 7;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_record(manifest, &again));
    TEST_ASSERT_EQUAL_UINT64(7, skiff_install_manifest_find_rom(manifest, 1)->size);
    TEST_PRINTF("a full manifest with long-ish names fits its byte cap");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_install_manifest_save(manifest, &storage.base, manifest_path));
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_LOADED, load());
    TEST_ASSERT_EQUAL_size_t(SKIFF_INSTALL_RECORDS_MAX, manifest->count);
}

static void test_a_full_manifest_of_the_longest_names_still_saves(void) {
    TEST_PRINTF("names of quotes and backslashes, each written twice in JSON");
    for (uint64_t i = 0; i < SKIFF_INSTALL_RECORDS_MAX; i++) {
        skiff_install_record record;
        memset(&record, 0, sizeof record);
        record.rom_id = SKIFF_INSTALL_ROM_ID_MAX - i;
        int used =
            snprintf(record.file_name, sizeof record.file_name, "%04llu", (unsigned long long)i);
        memset(record.file_name + used, '"', sizeof record.file_name - 1 - (size_t)used);
        used = snprintf(record.path, sizeof record.path, "games:/%04llu", (unsigned long long)i);
        memset(record.path + used, '\\', sizeof record.path - 1 - (size_t)used);
        record.size = SKIFF_STORAGE_MAX_FILE_BYTES;
        record.has_crc32 = 1;
        record.installed_ms = INSTALLED_MS;
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_record(manifest, &record));
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_install_manifest_save(manifest, &storage.base, manifest_path));
    uint64_t size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_size(posix, manifest_path, &size));
    TEST_PRINTF("installed.json: %llu bytes", (unsigned long long)size);
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_LOADED, load());
    TEST_ASSERT_EQUAL_size_t(SKIFF_INSTALL_RECORDS_MAX, manifest->count);
}

static void test_recording_replaces_the_rom_file_and_whatever_was_at_its_path(void) {
    add_record(ROM_ID, "Game.iso", "games:/Game [12].iso");
    add_record(OTHER_ROM_ID, "Old.iso", "games:/Game.iso");
    TEST_PRINTF("ROM 12 is installed again at Game.iso, replacing ROM 34's file there");
    add_record(ROM_ID, "Game.iso", "games:/Game.iso");
    TEST_ASSERT_EQUAL_size_t(1, manifest->count);
    TEST_ASSERT_EQUAL_STRING("games:/Game.iso",
                             skiff_install_manifest_find_rom(manifest, ROM_ID)->path);
    TEST_ASSERT_NULL(skiff_install_manifest_find_rom(manifest, OTHER_ROM_ID));
    add_record(OTHER_ROM_ID, "Other.iso", "games:/GAME.ISO");
    TEST_ASSERT_EQUAL_size_t(1, manifest->count);
    TEST_ASSERT_EQUAL_UINT64(OTHER_ROM_ID, manifest->records[0].rom_id);
}

static void test_records_are_checked(void) {
    skiff_install_record bad = record_of(ROM_ID, "Game.iso", "ms0:/ISO/Game.iso");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_install_manifest_record(manifest, &bad));
    bad = record_of(ROM_ID, "", "games:/Game.iso");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_install_manifest_record(manifest, &bad));
    bad = record_of(SKIFF_INSTALL_ROM_ID_MAX + 1, "Game.iso", "games:/Game.iso");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_install_manifest_record(manifest, &bad));
    bad = record_of(ROM_ID, "Game.iso", "games:/");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_install_manifest_record(manifest, &bad));
    bad = record_of(ROM_ID, "Game.iso", "games:/Game.iso");
    bad.size = SKIFF_STORAGE_MAX_FILE_BYTES + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_install_manifest_record(manifest, &bad));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_install_manifest_record(manifest, NULL));
    TEST_ASSERT_EQUAL_size_t(0, manifest->count);
}

static void test_forgetting_a_record(void) {
    add_record(ROM_ID, "Game.iso", "games:/Game.iso");
    add_record(OTHER_ROM_ID, "Other.iso", "games:/Other.iso");
    TEST_ASSERT_EQUAL_INT(1, skiff_install_manifest_forget(manifest, ROM_ID, "Game.iso"));
    TEST_ASSERT_EQUAL_INT(0, skiff_install_manifest_forget(manifest, ROM_ID, "Game.iso"));
    TEST_ASSERT_EQUAL_size_t(1, manifest->count);
    TEST_ASSERT_EQUAL_UINT64(OTHER_ROM_ID, manifest->records[0].rom_id);
}

static void test_reconcile_forgets_files_deleted_meanwhile(void) {
    add_record(ROM_ID, "Game.iso", "games:/Game.iso");
    add_record(OTHER_ROM_ID, "Gone.iso", "games:/Gone.iso");
    add_record(OTHER_ROM_ID + 1, "Kept.iso", "games:/Kept.iso");
    put_file("games:/Game.iso");
    put_file("games:/Kept.iso");
    size_t forgotten = 99;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_install_manifest_reconcile(manifest, &storage.base, &roots, &forgotten));
    TEST_ASSERT_EQUAL_size_t(1, forgotten);
    TEST_ASSERT_EQUAL_size_t(2, manifest->count);
    TEST_ASSERT_NULL(skiff_install_manifest_find_rom(manifest, OTHER_ROM_ID));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_INSTALL_NOT_INSTALLED,
        skiff_install_state_of(manifest, &(skiff_romm_rom_summary){.id = OTHER_ROM_ID}));
}

static void test_reconcile_forgets_files_replaced_outside_skiff(void) {
    add_record(ROM_ID, "Game.iso", "games:/Game.iso");
    put_file_of("games:/Game.iso", 1);
    size_t forgotten = 0;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_install_manifest_reconcile(manifest, &storage.base, &roots, &forgotten));
    TEST_ASSERT_EQUAL_size_t(1, forgotten);
    TEST_ASSERT_EQUAL_size_t(0, manifest->count);
}

static void test_reconcile_stops_at_an_unreadable_memory_stick(void) {
    add_record(ROM_ID, "Game.iso", "games:/Game.iso");
    storage.size_failures = 1;
    size_t forgotten = 99;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_install_manifest_reconcile(
                                                    manifest, &storage.base, &roots, &forgotten));
    TEST_ASSERT_EQUAL_size_t(0, forgotten);
    TEST_ASSERT_EQUAL_size_t(1, manifest->count);
}

static void test_a_failed_save_keeps_the_old_manifest(void) {
    add_record(ROM_ID, "Game.iso", "games:/Game.iso");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_install_manifest_save(manifest, &storage.base, manifest_path));
    add_record(OTHER_ROM_ID, "Other.iso", "games:/Other.iso");
    storage.fail_suffix = SKIFF_STORAGE_DRAFT_SUFFIX;
    storage.sync_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO,
                          skiff_install_manifest_save(manifest, &storage.base, manifest_path));
    storage.sync_error = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_LOADED, load());
    TEST_ASSERT_EQUAL_size_t(1, manifest->count);
}

static void test_an_unreadable_manifest_is_never_overwritten(void) {
    add_record(ROM_ID, "Game.iso", "games:/Game.iso");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_install_manifest_save(manifest, &storage.base, manifest_path));
    TEST_PRINTF("the Memory Stick fails while the manifest is read (e.g. right after a suspend)");
    storage.fail_suffix = SKIFF_INSTALL_MANIFEST_NAME;
    storage.size_failures = 1;
    skiff_install_load_result result = SKIFF_INSTALL_LOADED;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_STORAGE_IO,
        skiff_install_manifest_load(manifest, &storage.base, manifest_path, &result));
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_NO_MANIFEST, result);
    TEST_ASSERT_EQUAL_size_t(0, manifest->count);
    add_record(OTHER_ROM_ID, "Other.iso", "games:/Other.iso");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO,
                          skiff_install_manifest_save(manifest, &storage.base, manifest_path));
    TEST_PRINTF("the next good load clears the refusal and finds the old records");
    TEST_ASSERT_EQUAL_INT(SKIFF_INSTALL_LOADED, load());
    TEST_ASSERT_NOT_NULL(skiff_install_manifest_find_rom(manifest, ROM_ID));
}

static void test_format_refuses_a_buffer_too_small_and_null_arguments(void) {
    add_record(ROM_ID, "Game.iso", "games:/Game.iso");
    char text[16];
    size_t length = 99;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_install_manifest_format(manifest, text, sizeof text, &length));
    TEST_ASSERT_EQUAL_size_t(0, length);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_install_manifest_format(NULL, text, sizeof text, &length));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_install_manifest_parse(NULL, "{}", 2));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_install_manifest_parse(manifest, NULL, 2));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_install_manifest_load(NULL, &storage.base, manifest_path, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_install_manifest_save(manifest, NULL, manifest_path));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_install_manifest_create(NULL));
    TEST_ASSERT_NULL(skiff_install_manifest_find(NULL, ROM_ID, "Game.iso"));
    TEST_ASSERT_NULL(skiff_install_manifest_find_rom(NULL, ROM_ID));
    TEST_ASSERT_NULL(skiff_install_manifest_find_path(manifest, NULL));
    TEST_ASSERT_EQUAL_INT(0, skiff_install_manifest_forget(NULL, ROM_ID, "Game.iso"));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_install_manifest_reconcile(manifest, NULL, &roots, NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_psp_installer_is_found_by_slug);
    RUN_TEST(test_iso_cso_and_zso_are_supported_in_any_case);
    RUN_TEST(test_folders_and_unusable_names_are_not_installable);
    RUN_TEST(test_a_new_game_goes_to_its_safe_name_in_iso);
    RUN_TEST(test_a_file_copied_by_hand_is_never_replaced);
    RUN_TEST(test_another_rom_of_the_same_name_ignoring_case_gets_the_id);
    RUN_TEST(test_names_that_clean_to_the_same_name_do_not_collide);
    RUN_TEST(test_a_record_of_another_rom_holds_its_name_even_without_the_file);
    RUN_TEST(test_skiffs_own_copy_is_replaced_in_place);
    RUN_TEST(test_a_recorded_copy_replaced_outside_skiff_is_no_longer_its_own);
    RUN_TEST(test_a_full_manifest_refuses_a_download_it_could_not_record);
    RUN_TEST(test_a_rom_id_the_manifest_cannot_store_is_refused);
    RUN_TEST(test_skiffs_own_copy_keeps_its_renamed_place);
    RUN_TEST(test_a_recorded_copy_deleted_meanwhile_is_downloaded_again_to_its_place);
    RUN_TEST(test_both_names_taken_by_foreign_files_is_refused);
    RUN_TEST(test_a_name_promised_to_a_queued_download_is_avoided);
    RUN_TEST(test_a_long_name_is_shortened_between_characters_to_fit_the_id);
    RUN_TEST(test_an_unreadable_memory_stick_stops_the_plan);
    RUN_TEST(test_planning_refuses_null_arguments);
    RUN_TEST(test_the_library_shows_installed_and_changed_games);
    RUN_TEST(test_the_manifest_round_trips_through_the_memory_stick);
    RUN_TEST(test_unknown_fields_are_ignored);
    RUN_TEST(test_a_damaged_manifest_loads_empty_and_is_replaced_on_save);
    RUN_TEST(test_a_manifest_too_large_or_too_dense_is_damaged);
    RUN_TEST(test_the_manifest_holds_at_most_its_cap);
    RUN_TEST(test_a_full_manifest_of_the_longest_names_still_saves);
    RUN_TEST(test_recording_replaces_the_rom_file_and_whatever_was_at_its_path);
    RUN_TEST(test_records_are_checked);
    RUN_TEST(test_forgetting_a_record);
    RUN_TEST(test_reconcile_forgets_files_deleted_meanwhile);
    RUN_TEST(test_reconcile_forgets_files_replaced_outside_skiff);
    RUN_TEST(test_reconcile_stops_at_an_unreadable_memory_stick);
    RUN_TEST(test_a_failed_save_keeps_the_old_manifest);
    RUN_TEST(test_an_unreadable_manifest_is_never_overwritten);
    RUN_TEST(test_format_refuses_a_buffer_too_small_and_null_arguments);
    return UNITY_END();
}
