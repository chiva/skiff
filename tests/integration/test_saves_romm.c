/*
 * Save sync (skiff/romm_saves.h) against the integration RomM behind Caddy, over the curl transport
 * and the TLS stack the PSP links, with two PSPs paired through the device-code flow with every
 * scope Skiff asks for: A negotiates (upload) and uploads a synthetic unit, B negotiates
 * (download), downloads it byte for byte, confirms it and finds nothing more to do, then uploads
 * its own version; A's upload without overwrite is then refused with SKIFF_ERR_ROMM_SAVE_CONFLICT.
 * A token that only browses is left to the recorded fixtures and test_pairing_romm.c: RomM limits
 * how many pairings an address may start, and each one counts.
 *
 * Then a save folder makes the whole trip on the seed's synthetic PSP disc, whose save ID RomM read
 * when it scanned it (SKIF00002): A collects and uploads it as the saves layer writes a unit, RomM
 * lists the disc among the ROMs with saves and names A as the save's device, and B downloads,
 * checks and restores it byte for byte; negotiate's restore_unlisted brings back a save B synced
 * and no longer lists.
 *
 * Every run uses a slot of its own, so it can run again against the same server: negotiate reports
 * every slot, and the test keeps its own, as the app does. The units are the recorder's synthetic
 * zips (tests/fixtures/romm/save-unit-*.zip). Run by `scripts/dev.sh romm-test`
 * (tests/integration/transport-test.sh), with the server's details in SKIFF_IT_* environment
 * variables. Not a ctest test: it needs the server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "skiff/curl_transport.h"
#include "skiff/romm.h"
#include "skiff/romm_pairing.h"
#include "skiff/romm_saves.h"
#include "skiff/saves.h"
#include "skiff/saves_restore.h"

#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

enum {
    PATH_MAX_LENGTH = 256,
    BODY_MAX = 512,
    DECIMAL_BASE = 10,
    HTTP_OK = 200,
    UNIT_MAX = 4096,
};

#define TLS_SITE "https://proxy:8443"
#define TEST_CONNECT_TIMEOUT_S 5L
#define TEST_STALL_TIMEOUT_S 10L
/* Fixed, so every run pairs the same two devices: RomM keeps one record per identifier. */
#define IDENTIFIER_A "0000000000000000000000005a7e000a"
#define IDENTIFIER_B "0000000000000000000000005a7e000b"
#define ALL_SCOPES                                                                                 \
    "[\"platforms.read\",\"roms.read\",\"assets.read\",\"assets.write\",\"devices.read\","         \
    "\"devices.write\",\"collections.read\"]"
#define POLLS_MAX 6
#define UNIT_1 SKIFF_FIXTURES_DIR "/romm/save-unit-1.zip"
#define UNIT_2 SKIFF_FIXTURES_DIR "/romm/save-unit-2.zip"
/* RomM's content hashes of the units (record-saves-fixtures.sh prints them). */
#define HASH_1 "969f6b134c73db736eb427afde88aee4"
#define HASH_2 "3a54b3d66a1c01e3fdcfa5a2cfb9afb2"
#define FILE_NAME "SKIF00001.zip"
#define EMULATOR "psp_console"
#define KEEP_VERSIONS 10U
#define SLOT_MAX 32
#define MS_PER_SECOND 1000LL

/* The save disc's game: a save folder of it, and what the folder holds. */
#define DISC_FOLDER "PSP/SAVEDATA/SKIF00002DATA00"
#define DISC_DATA "synthetic save of the Skiff save disc"
#define DISC_PARAM "\x00PSF synthetic SKIF00002"
#define DISC_UNIT_NAME "SKIF00002.zip"
#define PLATFORM_SLUG "psp"

static char ca_file[PATH_MAX_LENGTH];
static uint64_t disc_rom_id;
static const char *disc_target;
static const char *admin_authorization;
static uint64_t rom_id;
static char slot[SLOT_MAX];
static char dir[TEMP_DIR_PATH_MAX];
static skiff_transport *transport;
static skiff_storage *storage;
static skiff_romm_client open_client;
static skiff_romm_sync_plan plan;

typedef struct paired_psp {
    skiff_romm_client client;
    char device_id[SKIFF_CONFIG_DEVICE_ID_MAX];
} paired_psp;

static paired_psp psp_a;
static paired_psp psp_b;

static const char *required_env(const char *name) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        printf("missing environment variable %s (run through scripts/dev.sh romm-test)\n", name);
        exit(EXIT_FAILURE);
    }
    return value;
}

void setUp(void) {}

/* The disc test's unit, freed here too when an assertion ends the test early. */
static skiff_saves_unit *disc_unit;

void tearDown(void) {
    skiff_saves_unit_destroy(disc_unit);
    disc_unit = NULL;
}

static void sleep_seconds(uint32_t seconds) {
    const struct timespec wait = {.tv_sec = (time_t)seconds, .tv_nsec = 0};
    nanosleep(&wait, NULL);
}

/* What the web UI sends when the admin approves user_code with scopes. */
static void admin_approve(const char *user_code, const char *scopes) {
    char body[BODY_MAX];
    snprintf(body, sizeof body, "{\"user_code\":\"%s\",\"approved_scopes\":%s}", user_code, scopes);
    const skiff_http_header authorization = {"Authorization", admin_authorization};
    skiff_http_request request;
    memset(&request, 0, sizeof request);
    request.method = SKIFF_HTTP_POST;
    request.url = TLS_SITE "/api/auth/device/approve";
    request.headers = &authorization;
    request.header_count = 1;
    request.body = body;
    request.body_size = strlen(body);
    request.content_type = "application/json";
    skiff_http_response response;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(transport, &request, &response));
    TEST_ASSERT_EQUAL_INT64(HTTP_OK, response.status);
}

/* Pairs a PSP as the app does, the admin approving scopes, and opens a client with its token. */
static void pair(const char *identifier, const char *scopes, paired_psp *out) {
    skiff_romm_pairing pairing;
    skiff_romm_pairing_result result;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_pairing_start(&open_client, identifier, &pairing));
    admin_approve(pairing.user_code, scopes);
    for (int i = 0; i < POLLS_MAX; i++) {
        sleep_seconds(pairing.interval_s);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_pairing_poll(&open_client, &pairing, &result));
        if (result.state == SKIFF_ROMM_PAIRING_APPROVED) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_PAIRING_APPROVED, result.state);
    TEST_PRINTF("paired %s as device %s with %s", identifier, result.device_id, result.scopes);
    snprintf(out->device_id, sizeof out->device_id, "%s", result.device_id);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_client_init(&out->client, transport, TLS_SITE, result.token));
    skiff_romm_pairing_clear(&pairing, &result);
}

static skiff_romm_save_state unit_state(const char *hash, uint64_t size) {
    const skiff_romm_save_state state = {
        rom_id, FILE_NAME, slot, EMULATOR, hash, (int64_t)time(NULL) * MS_PER_SECOND, size};
    return state;
}

/* Negotiates for psp and returns its operation for this run's slot, failing if there is none. */
static const skiff_romm_sync_operation *negotiate(paired_psp *psp,
                                                  const skiff_romm_save_state *state) {
    const uint64_t roms[] = {rom_id};
    const skiff_romm_sync_query query = {psp->device_id, roms, 1, state, state != NULL ? 1 : 0, 0};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_negotiate(&psp->client, &query, &plan));
    const skiff_romm_sync_operation *mine = NULL;
    for (size_t i = 0; i < plan.count; i++) {
        const skiff_romm_sync_operation *op = &plan.operations[i];
        TEST_PRINTF("session %llu: action %d, slot '%s', save %llu, hash '%s'",
                    (unsigned long long)plan.session_id, (int)op->action, op->slot,
                    (unsigned long long)op->save_id, op->server_content_hash);
        if (op->has_slot && strcmp(op->slot, slot) == 0) {
            TEST_ASSERT_NULL_MESSAGE(mine, "one operation per slot");
            mine = op;
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(mine, "an operation for this run's slot");
    return mine;
}

static skiff_err upload(paired_psp *psp, const char *unit, const char *hash, int overwrite,
                        skiff_romm_save *out) {
    const skiff_romm_upload spec = {rom_id,    psp->device_id, EMULATOR, slot, hash,
                                    overwrite, KEEP_VERSIONS,  storage,  unit, FILE_NAME};
    const skiff_err err = skiff_romm_saves_upload(&psp->client, &spec, out);
    TEST_PRINTF("upload from %s -> %s: save %llu '%s', hash %s, from '%s'", psp->device_id,
                skiff_err_name(err), (unsigned long long)out->id, out->file_name, out->content_hash,
                out->origin_device_name);
    return err;
}

static size_t read_file(const char *path, unsigned char *out, size_t out_size) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    const size_t size = fread(out, 1, out_size, file);
    fclose(file);
    return size;
}

/* The other tests run as these two PSPs. */
static void test_two_psps_pair_with_the_save_scopes(void) {
    pair(IDENTIFIER_A, ALL_SCOPES, &psp_a);
    pair(IDENTIFIER_B, ALL_SCOPES, &psp_b);
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp(psp_a.device_id, psp_b.device_id));
}

static void test_a_save_goes_from_one_psp_to_another_and_back(void) {
    static unsigned char unit[UNIT_MAX];
    static unsigned char got[UNIT_MAX];
    const size_t unit_size = read_file(UNIT_1, unit, sizeof unit);

    TEST_PRINTF("A holds a save RomM has never seen: upload");
    skiff_romm_save_state state = unit_state(HASH_1, unit_size);
    const skiff_romm_sync_operation *op = negotiate(&psp_a, &state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SYNC_UPLOAD, op->action);
    const uint64_t session_a = plan.session_id;
    skiff_romm_save first;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, upload(&psp_a, UNIT_1, HASH_1, 0, &first));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(HASH_1, first.content_hash, "RomM hashes the unit as Skiff");
    TEST_ASSERT_EQUAL_UINT64(unit_size, first.file_size_bytes);
    TEST_ASSERT_EQUAL_STRING(psp_a.device_id, first.origin_device_id);
    TEST_ASSERT_EQUAL_STRING(SKIFF_ROMM_PAIRING_NAME, first.origin_device_name);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_complete(&psp_a.client, session_a, 1, 0));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_ROMM_BAD_RESPONSE,
                                  skiff_romm_saves_complete(&psp_a.client, session_a, 1, 0),
                                  "a session closes once");

    TEST_PRINTF("B holds nothing: download A's save");
    op = negotiate(&psp_b, NULL);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SYNC_DOWNLOAD, op->action);
    TEST_ASSERT_EQUAL_UINT64(first.id, op->save_id);
    TEST_ASSERT_EQUAL_STRING(HASH_1, op->server_content_hash);
    const uint64_t session_b = plan.session_id;
    char target[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, FILE_NAME, target, sizeof target));
    const skiff_romm_download download = {op->save_id, psp_b.device_id, storage, target, UNIT_MAX};
    uint64_t size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_download(&psp_b.client, &download, &size));
    TEST_ASSERT_EQUAL_UINT64(unit_size, size);
    TEST_ASSERT_EQUAL_size_t(unit_size, read_file(target, got, sizeof got));
    TEST_ASSERT_EQUAL_MEMORY(unit, got, unit_size);
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_romm_saves_downloaded(&psp_b.client, first.id, psp_b.device_id, HASH_1));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_complete(&psp_b.client, session_b, 1, 0));

    TEST_PRINTF("B now holds what RomM holds: nothing to do");
    state = unit_state(HASH_1, unit_size);
    op = negotiate(&psp_b, &state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SYNC_NO_OP, op->action);

    TEST_PRINTF("B plays and uploads: RomM knows B synced the newest version, so no 409");
    /* RomM keeps times to the second and takes the newest save of a slot as its head: two saves in
     * the same second tie, and either may come back. */
    sleep_seconds(1);
    skiff_romm_save second;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, upload(&psp_b, UNIT_2, HASH_2, 0, &second));
    TEST_ASSERT_EQUAL_STRING(HASH_2, second.content_hash);
    TEST_ASSERT_EQUAL_STRING(psp_b.device_id, second.origin_device_id);

    TEST_PRINTF("A uploads without overwrite: RomM has a version A never synced");
    skiff_romm_save refused;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_SAVE_CONFLICT,
                          upload(&psp_a, UNIT_1, HASH_1, 0, &refused));
    TEST_ASSERT_EQUAL_UINT64(0, refused.id);
    TEST_PRINTF("and A's negotiate offers B's version");
    state = unit_state(HASH_1, unit_size);
    op = negotiate(&psp_a, &state);
    TEST_ASSERT_TRUE(op->has_save_id);
    TEST_ASSERT_EQUAL_UINT64(second.id, op->save_id);
    TEST_ASSERT_EQUAL_STRING(HASH_2, op->server_content_hash);
}

/* A PSP's Memory Stick under dir/<name>: its saves and Skiff's folder. */
static void psp_roots(const char *name, skiff_storage_roots *out) {
    char root[TEMP_DIR_PATH_MAX];
    char app[TEMP_DIR_PATH_MAX];
    char sub[PATH_MAX_LENGTH];
    snprintf(sub, sizeof sub, "%s", name);
    TEST_ASSERT_TRUE(temp_dir_path(dir, sub, root, sizeof root));
    snprintf(sub, sizeof sub, "%s/PSP/GAME/Skiff", name);
    TEST_ASSERT_TRUE(temp_dir_path(dir, sub, app, sizeof app));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_init(root, app, out));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(storage, out->app));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(storage, out->saves));
}

static void write_bytes(const char *path, const char *data, size_t size) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    TEST_ASSERT_EQUAL_size_t(size, fwrite(data, 1, size, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

/* The operation for this run's slot in the last plan, or NULL. */
static const skiff_romm_sync_operation *slot_operation(uint64_t id) {
    for (size_t i = 0; i < plan.count; i++) {
        const skiff_romm_sync_operation *op = &plan.operations[i];
        if (op->rom_id == id && op->has_slot && strcmp(op->slot, slot) == 0) {
            return op;
        }
    }
    return NULL;
}

static void test_a_save_folder_travels_through_the_save_disc(void) {
    skiff_storage_roots a_roots;
    skiff_storage_roots b_roots;
    psp_roots("psp-a", &a_roots);
    psp_roots("psp-b", &b_roots);
    char path[TEMP_DIR_PATH_MAX];
    char folder[PATH_MAX_LENGTH];
    snprintf(folder, sizeof folder, "psp-a/" DISC_FOLDER);
    TEST_ASSERT_TRUE(temp_dir_path(dir, folder, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(storage, path));
    snprintf(folder, sizeof folder, "psp-a/" DISC_FOLDER "/DATA.BIN");
    TEST_ASSERT_TRUE(temp_dir_path(dir, folder, path, sizeof path));
    write_bytes(path, DISC_DATA, sizeof DISC_DATA - 1);
    snprintf(folder, sizeof folder, "psp-a/" DISC_FOLDER "/PARAM.SFO");
    TEST_ASSERT_TRUE(temp_dir_path(dir, folder, path, sizeof path));
    write_bytes(path, DISC_PARAM, sizeof DISC_PARAM - 1);

    TEST_PRINTF("A collects the disc's save folders into a unit, as the app does");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_unit_create(&disc_unit));
    skiff_saves_unit *unit = disc_unit;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_collect(storage, &a_roots, SKIFF_STORAGE_ROOT_SAVES,
                                                        disc_target, unit));
    TEST_ASSERT_EQUAL_size_t(2, unit->file_count);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_write_unit(storage, &a_roots, unit, "app:/" DISC_UNIT_NAME));
    char hash[SKIFF_SAVES_HASH_SIZE];
    snprintf(hash, sizeof hash, "%s", unit->content_hash);
    TEST_PRINTF("unit hash %s, %llu bytes of saves", hash, (unsigned long long)unit->total_bytes);
    char unit_path[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&a_roots, "app:/" DISC_UNIT_NAME,
                                                          unit_path, sizeof unit_path));
    const skiff_romm_upload spec = {
        disc_rom_id, psp_a.device_id, EMULATOR, slot,      hash,
        0,           KEEP_VERSIONS,   storage,  unit_path, DISC_UNIT_NAME};
    skiff_romm_save uploaded;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_upload(&psp_a.client, &spec, &uploaded));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(hash, uploaded.content_hash, "RomM hashes the unit as Skiff");

    TEST_PRINTF("RomM lists the disc among the PSP ROMs with saves");
    skiff_romm_platform platform;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_find_platform(&psp_a.client, PLATFORM_SLUG, &platform));
    const skiff_romm_list_query with_saves = {.platform_id = platform.id,
                                              .filter = SKIFF_ROMM_LIST_WITH_SAVES};
    static skiff_romm_rom_page page;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_romm_list_roms(&psp_a.client, &with_saves, 0, SKIFF_ROMM_PAGE_SIZE, &page));
    int listed = 0;
    for (size_t i = 0; i < page.count; i++) {
        TEST_PRINTF("with saves: ROM %llu %s, save ID '%s'", (unsigned long long)page.items[i].id,
                    page.items[i].fs_name, page.items[i].save_target);
        listed +=
            page.items[i].id == disc_rom_id && strcmp(page.items[i].save_target, disc_target) == 0;
    }
    TEST_ASSERT_EQUAL_INT(1, listed);

    TEST_PRINTF("B reads the save: A's device uploaded it");
    skiff_romm_save read;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_saves_get(&psp_b.client, uploaded.id, psp_b.device_id, &read));
    TEST_ASSERT_EQUAL_UINT64(uploaded.id, read.id);
    TEST_ASSERT_EQUAL_STRING(psp_a.device_id, read.origin_device_id);
    TEST_ASSERT_EQUAL_STRING(SKIFF_ROMM_PAIRING_NAME, read.origin_device_name);

    TEST_PRINTF("B downloads, checks and restores it byte for byte");
    const uint64_t roms[] = {disc_rom_id};
    skiff_romm_sync_query query = {psp_b.device_id, roms, 1, NULL, 0, 1};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_negotiate(&psp_b.client, &query, &plan));
    const skiff_romm_sync_operation *op = slot_operation(disc_rom_id);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_SYNC_DOWNLOAD, op->action);
    TEST_ASSERT_EQUAL_STRING(hash, op->server_content_hash);
    char work[SKIFF_STORAGE_PATH_MAX];
    char work_folder[SKIFF_STORAGE_PATH_MAX];
    char logical[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_work_unit_path(disc_target, logical, sizeof logical));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&b_roots, logical, work, sizeof work));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&b_roots, SKIFF_SAVES_WORK_FOLDER,
                                                          work_folder, sizeof work_folder));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(storage, work_folder));
    const skiff_romm_download download = {op->save_id, psp_b.device_id, storage, work,
                                          SKIFF_SAVES_UNIT_FILE_BYTES_MAX};
    uint64_t size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_download(&psp_b.client, &download, &size));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_saves_read_unit(storage, &b_roots, logical, disc_target, NULL, unit));
    TEST_ASSERT_EQUAL_STRING(hash, unit->content_hash);
    skiff_saves_restore_report report;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_restore(storage, &b_roots, logical, disc_target, &report));
    TEST_ASSERT_EQUAL_size_t(1, report.folders_replaced);
    static unsigned char got[UNIT_MAX];
    snprintf(folder, sizeof folder, "psp-b/" DISC_FOLDER "/DATA.BIN");
    TEST_ASSERT_TRUE(temp_dir_path(dir, folder, path, sizeof path));
    TEST_ASSERT_EQUAL_size_t(sizeof DISC_DATA - 1, read_file(path, got, sizeof got));
    TEST_ASSERT_EQUAL_MEMORY(DISC_DATA, got, sizeof DISC_DATA - 1);
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_romm_saves_downloaded(&psp_b.client, op->save_id, psp_b.device_id, hash));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_saves_complete(&psp_b.client, plan.session_id, 1, 0));

    TEST_PRINTF("B no longer lists the save it synced: RomM leaves it out unless asked back");
    query.restore_unlisted = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_negotiate(&psp_b.client, &query, &plan));
    TEST_ASSERT_NULL(slot_operation(disc_rom_id));
    query.restore_unlisted = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_saves_negotiate(&psp_b.client, &query, &plan));
    op = slot_operation(disc_rom_id);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_STRING(hash, op->server_content_hash);
}

int main(void) {
    snprintf(ca_file, sizeof ca_file, "%s/ca.crt", required_env("SKIFF_IT_CERTS"));
    admin_authorization = required_env("SKIFF_IT_ADMIN_AUTHORIZATION");
    rom_id = strtoull(required_env("SKIFF_IT_ROM_ID"), NULL, DECIMAL_BASE);
    disc_rom_id = strtoull(required_env("SKIFF_IT_SAVE_DISC_ROM_ID"), NULL, DECIMAL_BASE);
    disc_target = required_env("SKIFF_IT_SAVE_DISC_TARGET");
    snprintf(slot, sizeof slot, "it-%lld", (long long)time(NULL));
    if (skiff_net_global_init() != SKIFF_OK || temp_dir_create(dir, sizeof dir) != 0 ||
        skiff_posix_storage_create(&storage) != SKIFF_OK) {
        printf("setup failed\n");
        return EXIT_FAILURE;
    }
    const skiff_curl_config config = {.ca_file = ca_file,
                                      .connect_timeout_s = TEST_CONNECT_TIMEOUT_S,
                                      .stall_timeout_s = TEST_STALL_TIMEOUT_S};
    if (skiff_curl_transport_create(&config, &transport) != SKIFF_OK ||
        skiff_romm_client_init(&open_client, transport, TLS_SITE, NULL) != SKIFF_OK) {
        printf("setup failed: no transport\n");
        return EXIT_FAILURE;
    }
    printf("this run's slot: %s\n", slot);

    UNITY_BEGIN();
    RUN_TEST(test_two_psps_pair_with_the_save_scopes);
    RUN_TEST(test_a_save_goes_from_one_psp_to_another_and_back);
    RUN_TEST(test_a_save_folder_travels_through_the_save_disc);
    const int failures = UNITY_END();
    skiff_romm_client_clear(&psp_a.client);
    skiff_romm_client_clear(&psp_b.client);
    skiff_transport_destroy(transport);
    skiff_storage_destroy(storage);
    temp_dir_remove(dir);
    skiff_net_global_cleanup();
    return failures;
}
