/*
 * Skiff's sync baseline and decision rule (skiff/saves_sync.h): the decision table row by row
 * (design §3, with the empty unit and the deleted local save), saves.json written, read back,
 * refused when damaged and set aside, forgotten for a server change; and the folder-listing summary
 * that tells an unchanged game from a changed one without reading a file. Every save here is
 * synthetic.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/saves_sync.h"

#include "fake_storage.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define GAME "SKIF00001"
#define OTHER_GAME "SKIF00002"
#define DATA00 GAME "DATA00"
#define SAVES SKIFF_STORAGE_ROOT_SAVES
/* Four distinct content hashes, and RomM's of an empty unit. */
#define HASH_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define HASH_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define HASH_C "cccccccccccccccccccccccccccccccc"
#define HASH_D "dddddddddddddddddddddddddddddddd"
#define EMPTY SKIFF_SAVES_EMPTY_HASH
#define SAVE_ID 41U
#define OTHER_SAVE_ID 42U
#define ROM_ID 7U
#define SYNCED_MS 1790812800123LL
#define TEXT_MAX ((size_t)256 * 1024)

static char dir[TEMP_DIR_PATH_MAX];
static skiff_storage *posix;
static fake_storage storage;
static skiff_storage_roots roots;
static skiff_saves_baseline *baseline;
static char *text;

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    char app[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "PSP/GAME/Skiff", app, sizeof app));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_init(dir, app, &roots));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&posix));
    fake_storage_init(&storage, posix);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, roots.saves));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, roots.app));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_create(&baseline));
    text = malloc(TEXT_MAX);
    TEST_ASSERT_NOT_NULL(text);
}

void tearDown(void) {
    free(text);
    skiff_saves_baseline_destroy(baseline);
    skiff_storage_destroy(&storage.base);
    skiff_storage_destroy(posix);
    temp_dir_remove(dir);
}

static skiff_saves_baseline_entry make_entry(const char *save_target, const char *local,
                                             const char *server) {
    skiff_saves_baseline_entry entry;
    memset(&entry, 0, sizeof entry);
    snprintf(entry.save_target, sizeof entry.save_target, "%s", save_target);
    entry.rom_id = ROM_ID;
    entry.server_save_id = SAVE_ID;
    snprintf(entry.server_hash, sizeof entry.server_hash, "%s", server);
    snprintf(entry.local_hash, sizeof entry.local_hash, "%s", local);
    entry.synced_ms = SYNCED_MS;
    entry.tree.file_count = 2;
    snprintf(entry.tree.digest, sizeof entry.tree.digest, "%s", HASH_D);
    return entry;
}

/* ---- The decision rule ---- */

typedef struct decision_row {
    const char *name;
    const char *local;
    const char *server;
    /* B_l and B_s; NULL for no baseline. */
    const char *baseline_local;
    const char *baseline_server;
    skiff_saves_action action;
    skiff_saves_reason reason;
} decision_row;

static void assert_row(const decision_row *row) {
    skiff_saves_baseline_entry entry;
    if (row->baseline_local != NULL) {
        entry = make_entry(GAME, row->baseline_local, row->baseline_server);
    }
    const skiff_saves_sync_input in = {row->local, row->server, SAVE_ID,
                                       row->baseline_local != NULL ? &entry : NULL};
    skiff_saves_decision out;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_decide(&in, &out));
    TEST_PRINTF("%-48s L=%.4s S=%.4s B_l=%.4s B_s=%.4s -> %s (%s)", row->name,
                row->local != NULL ? row->local : "-", row->server != NULL ? row->server : "-",
                row->baseline_local != NULL ? row->baseline_local : "-",
                row->baseline_server != NULL ? row->baseline_server : "-",
                skiff_saves_action_name(out.action), skiff_saves_reason_name(out.reason));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(skiff_saves_action_name(row->action),
                                     skiff_saves_action_name(out.action), row->name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(skiff_saves_reason_name(row->reason),
                                     skiff_saves_reason_name(out.reason), row->name);
}

/* Design §3's table, each row with and without a baseline where the row says "any". */
static void test_the_decision_table_row_by_row(void) {
    static const decision_row ROWS[] = {
        /* L == S: in sync, record the baseline, whatever the baseline says. */
        {"same content, no baseline", HASH_A, HASH_A, NULL, NULL, SKIFF_SAVES_IN_SYNC,
         SKIFF_SAVES_REASON_SAME_CONTENT},
        {"same content, stale baseline", HASH_A, HASH_A, HASH_B, HASH_C, SKIFF_SAVES_IN_SYNC,
         SKIFF_SAVES_REASON_SAME_CONTENT},
        /* Local exists, server none: upload. */
        {"only local, no baseline", HASH_A, NULL, NULL, NULL, SKIFF_SAVES_UPLOAD,
         SKIFF_SAVES_REASON_ONLY_LOCAL},
        {"only local, baseline", HASH_A, NULL, HASH_A, HASH_B, SKIFF_SAVES_UPLOAD,
         SKIFF_SAVES_REASON_ONLY_LOCAL},
        /* Local none, server exists: download when there is nothing local to lose... */
        {"only server, no baseline", NULL, HASH_B, NULL, NULL, SKIFF_SAVES_DOWNLOAD,
         SKIFF_SAVES_REASON_ONLY_SERVER},
        {"only server, baseline of an empty local", NULL, HASH_B, EMPTY, HASH_C,
         SKIFF_SAVES_DOWNLOAD, SKIFF_SAVES_REASON_ONLY_SERVER},
        /* ... but a save deleted on this PSP since the last sync asks. */
        {"deleted on this PSP, server unchanged", NULL, HASH_B, HASH_A, HASH_B,
         SKIFF_SAVES_ASK_DELETED_LOCAL, SKIFF_SAVES_REASON_DELETED_LOCAL},
        {"deleted on this PSP, server changed", NULL, HASH_C, HASH_A, HASH_B,
         SKIFF_SAVES_ASK_DELETED_LOCAL, SKIFF_SAVES_REASON_DELETED_LOCAL},
        {"local unit empty counts as deleted", EMPTY, HASH_B, HASH_A, HASH_B,
         SKIFF_SAVES_ASK_DELETED_LOCAL, SKIFF_SAVES_REASON_DELETED_LOCAL},
        /* Both exist, no baseline (first sync, Pair again, server change): ask. */
        {"both differ, no baseline", HASH_A, HASH_B, NULL, NULL, SKIFF_SAVES_ASK_CONFLICT,
         SKIFF_SAVES_REASON_NO_BASELINE},
        /* L == B_l, S != B_s: download (after a backup). */
        {"server changed", HASH_A, HASH_C, HASH_A, HASH_B, SKIFF_SAVES_DOWNLOAD,
         SKIFF_SAVES_REASON_SERVER_CHANGED},
        /* L != B_l, S == B_s: upload. */
        {"local changed", HASH_C, HASH_B, HASH_A, HASH_B, SKIFF_SAVES_UPLOAD,
         SKIFF_SAVES_REASON_LOCAL_CHANGED},
        /* L != B_l, S != B_s: ask. */
        {"both changed", HASH_C, HASH_D, HASH_A, HASH_B, SKIFF_SAVES_ASK_CONFLICT,
         SKIFF_SAVES_REASON_BOTH_CHANGED},
        /* Neither changed, though they differ: nothing to do. */
        {"neither changed", HASH_A, HASH_B, HASH_A, HASH_B, SKIFF_SAVES_IN_SYNC,
         SKIFF_SAVES_REASON_UNCHANGED},
        /* Nothing on either side. */
        {"nothing anywhere", NULL, NULL, NULL, NULL, SKIFF_SAVES_IN_SYNC,
         SKIFF_SAVES_REASON_NO_SAVES},
        {"nothing anywhere, baseline", NULL, NULL, HASH_A, HASH_B, SKIFF_SAVES_IN_SYNC,
         SKIFF_SAVES_REASON_NO_SAVES},
        {"empty server, nothing local", NULL, EMPTY, HASH_A, HASH_B, SKIFF_SAVES_IN_SYNC,
         SKIFF_SAVES_REASON_NO_SAVES},
        {"both empty", EMPTY, EMPTY, NULL, NULL, SKIFF_SAVES_IN_SYNC, SKIFF_SAVES_REASON_NO_SAVES},
        /* An empty server unit is never restored on its own. */
        {"empty server, no baseline", HASH_A, EMPTY, NULL, NULL, SKIFF_SAVES_ASK_DELETED_SERVER,
         SKIFF_SAVES_REASON_SERVER_EMPTY},
        {"empty server, local unchanged", HASH_A, EMPTY, HASH_A, HASH_B,
         SKIFF_SAVES_ASK_DELETED_SERVER, SKIFF_SAVES_REASON_SERVER_EMPTY},
        {"empty server, both changed", HASH_C, EMPTY, HASH_A, HASH_B,
         SKIFF_SAVES_ASK_DELETED_SERVER, SKIFF_SAVES_REASON_SERVER_EMPTY},
        {"empty server synced, local unchanged", HASH_A, EMPTY, HASH_A, EMPTY,
         SKIFF_SAVES_ASK_DELETED_SERVER, SKIFF_SAVES_REASON_SERVER_EMPTY},
        /* An empty unit already synced, then a new save on this PSP: a plain local change. */
        {"empty server synced, new local save", HASH_A, EMPTY, EMPTY, EMPTY, SKIFF_SAVES_UPLOAD,
         SKIFF_SAVES_REASON_LOCAL_CHANGED},
    };
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) {
        assert_row(&ROWS[i]);
    }
}

static int is_empty_hash(const char *hash) { return hash != NULL && strcmp(hash, EMPTY) == 0; }

/* Whatever the hashes, a download never goes over a local change and an upload never over a
 * server change: every combination of four hashes, with and without a baseline. */
static void test_no_transfer_ever_overwrites_a_change(void) {
    static const char *const HASHES[] = {NULL, HASH_A, HASH_B, HASH_C, EMPTY};
    const size_t count = sizeof HASHES / sizeof HASHES[0];
    size_t checked = 0;
    for (size_t l = 0; l < count; l++) {
        for (size_t s = 0; s < count; s++) {
            for (size_t bl = 1; bl < count; bl++) {
                for (size_t bs = 1; bs < count; bs++) {
                    const skiff_saves_baseline_entry entry =
                        make_entry(GAME, HASHES[bl], HASHES[bs]);
                    const skiff_saves_sync_input in = {HASHES[l], HASHES[s], SAVE_ID, &entry};
                    skiff_saves_decision out;
                    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_decide(&in, &out));
                    const int local_present = HASHES[l] != NULL && !is_empty_hash(HASHES[l]);
                    const int local_changed =
                        local_present && strcmp(HASHES[l], entry.local_hash) != 0;
                    const int server_changed =
                        HASHES[s] != NULL && strcmp(HASHES[s], entry.server_hash) != 0;
                    if (out.action == SKIFF_SAVES_DOWNLOAD) {
                        TEST_ASSERT_FALSE(local_changed);
                        TEST_ASSERT_FALSE(is_empty_hash(HASHES[s]));
                    }
                    if (out.action == SKIFF_SAVES_UPLOAD && HASHES[s] != NULL) {
                        TEST_ASSERT_FALSE(server_changed);
                    }
                    if (out.action == SKIFF_SAVES_DOWNLOAD && !local_present) {
                        TEST_ASSERT_EQUAL_STRING(EMPTY, entry.local_hash);
                    }
                    checked++;
                }
            }
        }
    }
    TEST_PRINTF("%zu combinations checked", checked);
}

/* The server unchanged by content, but its head another row (the same content uploaded again, or
 * a tie within a second): no transfer, and the caller is told to record the new id. */
static void test_a_moved_server_id_with_the_same_content_is_no_change(void) {
    const skiff_saves_baseline_entry entry = make_entry(GAME, HASH_A, HASH_B);
    skiff_saves_sync_input in = {HASH_C, HASH_B, OTHER_SAVE_ID, &entry};
    skiff_saves_decision out;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_decide(&in, &out));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_UPLOAD, out.action);
    TEST_ASSERT_EQUAL_INT(1, out.server_id_moved);
    in.server_save_id = SAVE_ID;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_decide(&in, &out));
    TEST_ASSERT_EQUAL_INT(0, out.server_id_moved);
    in.server_hash = HASH_D;
    in.server_save_id = OTHER_SAVE_ID;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_decide(&in, &out));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_ASK_CONFLICT, out.action);
    TEST_ASSERT_EQUAL_INT(0, out.server_id_moved);
}

static void test_decide_rejects_malformed_hashes(void) {
    skiff_saves_decision out;
    memset(&out, 0xA5, sizeof out);
    const skiff_saves_sync_input upper = {"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", NULL, 0, NULL};
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_decide(&upper, &out));
    TEST_ASSERT_EQUAL_INT(0, out.server_id_moved);
    const skiff_saves_sync_input short_hash = {NULL, "abc", 0, NULL};
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_decide(&short_hash, &out));
    const skiff_saves_sync_input long_hash = {HASH_A "0", NULL, 0, NULL};
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_decide(&long_hash, &out));
    skiff_saves_baseline_entry entry = make_entry(GAME, HASH_A, HASH_B);
    entry.local_hash[0] = 'x';
    const skiff_saves_sync_input bad_baseline = {HASH_A, HASH_B, 0, &entry};
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_decide(&bad_baseline, &out));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_decide(NULL, &out));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_decide(&upper, NULL));
}

static void test_actions_and_reasons_have_log_names(void) {
    TEST_ASSERT_EQUAL_STRING("in-sync", skiff_saves_action_name(SKIFF_SAVES_IN_SYNC));
    TEST_ASSERT_EQUAL_STRING("ask-deleted-server",
                             skiff_saves_action_name(SKIFF_SAVES_ASK_DELETED_SERVER));
    TEST_ASSERT_EQUAL_STRING("unknown", skiff_saves_action_name((skiff_saves_action)99));
    TEST_ASSERT_EQUAL_STRING("unknown", skiff_saves_action_name((skiff_saves_action)-1));
    TEST_ASSERT_EQUAL_STRING("same-content",
                             skiff_saves_reason_name(SKIFF_SAVES_REASON_SAME_CONTENT));
    TEST_ASSERT_EQUAL_STRING("unchanged", skiff_saves_reason_name(SKIFF_SAVES_REASON_UNCHANGED));
    TEST_ASSERT_EQUAL_STRING("unknown", skiff_saves_reason_name((skiff_saves_reason)99));
}

/* ---- The baseline ---- */

static void assert_same_entry(const skiff_saves_baseline_entry *expected,
                              const skiff_saves_baseline_entry *actual) {
    TEST_ASSERT_EQUAL_STRING(expected->save_target, actual->save_target);
    TEST_ASSERT_EQUAL_UINT64(expected->rom_id, actual->rom_id);
    TEST_ASSERT_EQUAL_UINT64(expected->server_save_id, actual->server_save_id);
    TEST_ASSERT_EQUAL_STRING(expected->server_hash, actual->server_hash);
    TEST_ASSERT_EQUAL_STRING(expected->local_hash, actual->local_hash);
    TEST_ASSERT_EQUAL_INT64(expected->synced_ms, actual->synced_ms);
    TEST_ASSERT_EQUAL_UINT32(expected->tree.file_count, actual->tree.file_count);
    TEST_ASSERT_EQUAL_STRING(expected->tree.digest, actual->tree.digest);
}

static void test_the_baseline_round_trips_through_saves_json(void) {
    skiff_saves_baseline_entry first = make_entry(GAME, HASH_A, HASH_B);
    first.rom_id = SKIFF_SAVES_ID_MAX;
    skiff_saves_baseline_entry second = make_entry(OTHER_GAME, EMPTY, HASH_C);
    second.tree.file_count = SKIFF_SAVES_TREE_UNKNOWN;
    second.tree.digest[0] = '\0';
    second.synced_ms = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_record(baseline, &first));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_record(baseline, &second));
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_storage_resolve(&roots, SKIFF_SAVES_BASELINE_PATH, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_save(baseline, &storage.base, path));
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_read_whole(posix, path, text, TEXT_MAX - 1, &length));
    text[length] = '\0';
    TEST_PRINTF("saves.json: %s", text);
    skiff_saves_baseline *loaded = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_create(&loaded));
    skiff_saves_baseline_load_result result = SKIFF_SAVES_BASELINE_DAMAGED;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_baseline_load(loaded, &storage.base, path, &result));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_BASELINE_LOADED, result);
    TEST_ASSERT_EQUAL_size_t(2, loaded->count);
    const skiff_saves_baseline_entry *a = skiff_saves_baseline_find(loaded, GAME);
    const skiff_saves_baseline_entry *b = skiff_saves_baseline_find(loaded, OTHER_GAME);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    assert_same_entry(&first, a);
    assert_same_entry(&second, b);
    skiff_saves_baseline_destroy(loaded);
}

static void test_record_replaces_a_game_and_forget_drops_it(void) {
    skiff_saves_baseline_entry entry = make_entry(GAME, HASH_A, HASH_B);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_record(baseline, &entry));
    entry.server_save_id = OTHER_SAVE_ID;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_record(baseline, &entry));
    TEST_ASSERT_EQUAL_size_t(1, baseline->count);
    TEST_ASSERT_EQUAL_UINT64(OTHER_SAVE_ID,
                             skiff_saves_baseline_find(baseline, GAME)->server_save_id);
    const skiff_saves_baseline_entry other = make_entry(OTHER_GAME, HASH_C, HASH_D);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_record(baseline, &other));
    TEST_ASSERT_EQUAL_INT(1, skiff_saves_baseline_forget(baseline, GAME));
    TEST_ASSERT_EQUAL_INT(0, skiff_saves_baseline_forget(baseline, GAME));
    TEST_ASSERT_NULL(skiff_saves_baseline_find(baseline, GAME));
    TEST_ASSERT_EQUAL_STRING(OTHER_GAME, baseline->entries[0].save_target);
    TEST_ASSERT_EQUAL_INT(0, skiff_saves_baseline_forget(NULL, GAME));
    TEST_ASSERT_EQUAL_INT(0, skiff_saves_baseline_forget(baseline, NULL));
    TEST_ASSERT_NULL(skiff_saves_baseline_find(NULL, GAME));
    TEST_ASSERT_NULL(skiff_saves_baseline_find(baseline, NULL));
}

/* Server change or Pair again: every baseline goes, so any difference asks. */
static void test_forget_all_makes_every_difference_ask(void) {
    const skiff_saves_baseline_entry entry = make_entry(GAME, HASH_A, HASH_B);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_record(baseline, &entry));
    skiff_saves_baseline_forget_all(baseline);
    skiff_saves_baseline_forget_all(NULL);
    TEST_ASSERT_EQUAL_size_t(0, baseline->count);
    const skiff_saves_sync_input in = {HASH_A, HASH_C, SAVE_ID,
                                       skiff_saves_baseline_find(baseline, GAME)};
    skiff_saves_decision out;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_decide(&in, &out));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_SAVES_ASK_CONFLICT, out.action,
                                  "without the baseline, the server's change is not provable");
}

static void test_record_refuses_entries_it_could_not_save(void) {
    skiff_saves_baseline_entry entry = make_entry(GAME, HASH_A, HASH_B);
    skiff_saves_baseline_entry bad = entry;
    snprintf(bad.save_target, sizeof bad.save_target, "%s", "bad");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_baseline_record(baseline, &bad));
    bad = entry;
    bad.rom_id = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_baseline_record(baseline, &bad));
    bad = entry;
    bad.server_save_id = SKIFF_SAVES_ID_MAX + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_baseline_record(baseline, &bad));
    bad = entry;
    bad.server_hash[0] = 'G';
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_baseline_record(baseline, &bad));
    bad = entry;
    bad.synced_ms = -1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_baseline_record(baseline, &bad));
    bad = entry;
    bad.tree.file_count = SKIFF_SAVES_FILES_MAX + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_baseline_record(baseline, &bad));
    bad = entry;
    bad.tree.file_count = SKIFF_SAVES_TREE_UNKNOWN;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_baseline_record(baseline, &bad));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_baseline_record(NULL, &entry));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_baseline_record(baseline, NULL));
    TEST_ASSERT_EQUAL_size_t(0, baseline->count);
}

/* 512 games fit saves.json's cap; the 513th is refused. */
static void test_the_baseline_holds_512_games_and_saves_them_all(void) {
    for (size_t i = 0; i < SKIFF_SAVES_BASELINE_GAMES_MAX; i++) {
        char save_target[SKIFF_SAVES_TARGET_SIZE];
        snprintf(save_target, sizeof save_target, "ULUS1%04zu", i);
        skiff_saves_baseline_entry entry = make_entry(save_target, HASH_A, HASH_B);
        entry.rom_id = SKIFF_SAVES_ID_MAX;
        entry.server_save_id = SKIFF_SAVES_ID_MAX;
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_record(baseline, &entry));
    }
    const skiff_saves_baseline_entry extra = make_entry("ABCD", HASH_A, HASH_B);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_saves_baseline_record(baseline, &extra));
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_format(
                                        baseline, text, SKIFF_SAVES_BASELINE_BYTES_MAX, &length));
    TEST_PRINTF("512 games: %zu bytes of %zu", length, SKIFF_SAVES_BASELINE_BYTES_MAX);
    skiff_saves_baseline *parsed = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_create(&parsed));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_parse(parsed, text, length));
    TEST_ASSERT_EQUAL_size_t(SKIFF_SAVES_BASELINE_GAMES_MAX, parsed->count);
    skiff_saves_baseline_destroy(parsed);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_saves_baseline_format(baseline, text, 16, &length));
}

static void put_baseline_text(const char *path, const char *content, size_t length) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_replace_whole(posix, path, content, length));
}

/* A file Skiff cannot read starts the baseline empty, never crashes, and is set aside on the next
 * save rather than lost. */
static void test_a_damaged_saves_json_starts_empty_and_is_set_aside(void) {
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_storage_resolve(&roots, SKIFF_SAVES_BASELINE_PATH, path, sizeof path));
    static const char *const DAMAGED[] = {
        "{\"version\":1,\"games\":[",
        "{\"version\":2,\"games\":[]}",
        "{\"version\":1,\"games\":{}}",
        "{\"version\":1,\"games\":[{\"save_target\":\"" GAME "\"}]}",
        "{\"version\":1,\"games\":[{\"save_target\":\"" GAME "\",\"rom_id\":7,\"save_id\":41,"
        "\"server_hash\":\"" HASH_B "\",\"local_hash\":\"" HASH_A "\",\"synced_ms\":1,"
        "\"files\":2,\"tree\":\"\"}]}",
        "{\"version\":1,\"games\":[{\"save_target\":\"" GAME "\",\"rom_id\":7,\"save_id\":41,"
        "\"server_hash\":\"" HASH_B "\",\"local_hash\":\"" HASH_A "\",\"synced_ms\":1.5,"
        "\"files\":2,\"tree\":\"" HASH_D "\"}]}",
        "{\"version\":1,\"games\":[{\"save_target\":\"" GAME "\",\"rom_id\":7,\"save_id\":41,"
        "\"server_hash\":\"" HASH_B "\",\"local_hash\":\"" HASH_A "\",\"synced_ms\":1,"
        "\"files\":-1,\"tree\":\"\"},{\"save_target\":\"" GAME "\",\"rom_id\":7,\"save_id\":41,"
        "\"server_hash\":\"" HASH_B "\",\"local_hash\":\"" HASH_A "\",\"synced_ms\":1,"
        "\"files\":-1,\"tree\":\"\"}]}",
        "{\"version\":1,\"games\":[]} x",
        "not json",
    };
    for (size_t i = 0; i < sizeof DAMAGED / sizeof DAMAGED[0]; i++) {
        put_baseline_text(path, DAMAGED[i], strlen(DAMAGED[i]));
        skiff_saves_baseline_load_result result = SKIFF_SAVES_BASELINE_LOADED;
        TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                              skiff_saves_baseline_load(baseline, &storage.base, path, &result));
        TEST_PRINTF("%s -> %d", DAMAGED[i], (int)result);
        TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_BASELINE_DAMAGED, result);
        TEST_ASSERT_EQUAL_size_t(0, baseline->count);
    }
    const skiff_saves_baseline_entry entry = make_entry(GAME, HASH_A, HASH_B);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_record(baseline, &entry));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_save(baseline, &storage.base, path));
    char kept[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK,
        skiff_storage_resolve(&roots, SKIFF_SAVES_BASELINE_PATH SKIFF_SAVES_BASELINE_DAMAGED_SUFFIX,
                              kept, sizeof kept));
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_read_whole(posix, kept, text, TEXT_MAX - 1, &length));
    text[length] = '\0';
    TEST_ASSERT_EQUAL_STRING_MESSAGE("not json", text, "the damaged file is kept aside");
    skiff_saves_baseline_load_result result = SKIFF_SAVES_BASELINE_DAMAGED;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_baseline_load(baseline, &storage.base, path, &result));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_BASELINE_LOADED, result);
    TEST_ASSERT_EQUAL_size_t(1, baseline->count);
}

static void test_an_oversized_saves_json_is_damaged(void) {
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_storage_resolve(&roots, SKIFF_SAVES_BASELINE_PATH, path, sizeof path));
    memset(text, ' ', SKIFF_SAVES_BASELINE_BYTES_MAX + 1);
    put_baseline_text(path, text, SKIFF_SAVES_BASELINE_BYTES_MAX + 1);
    skiff_saves_baseline_load_result result = SKIFF_SAVES_BASELINE_LOADED;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_baseline_load(baseline, &storage.base, path, &result));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_BASELINE_DAMAGED, result);
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_saves_baseline_parse(baseline, text, SKIFF_SAVES_BASELINE_BYTES_MAX + 1));
}

/* A storage error on load leaves the baselines on the Memory Stick unknown: saving refuses to
 * replace them with an empty list. */
static void test_a_failed_load_refuses_to_save(void) {
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_storage_resolve(&roots, SKIFF_SAVES_BASELINE_PATH, path, sizeof path));
    put_baseline_text(path, "{\"version\":1,\"games\":[]}", strlen("{\"version\":1,\"games\":[]}"));
    storage.read_error = SKIFF_ERR_STORAGE_IO;
    skiff_saves_baseline_load_result result = SKIFF_SAVES_BASELINE_LOADED;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO,
                          skiff_saves_baseline_load(baseline, &storage.base, path, &result));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_BASELINE_NO_FILE, result);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO,
                          skiff_saves_baseline_save(baseline, &storage.base, path));
    storage.read_error = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_load(baseline, &storage.base, path, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_save(baseline, &storage.base, path));
}

static void test_a_missing_saves_json_is_no_file(void) {
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_storage_resolve(&roots, SKIFF_SAVES_BASELINE_PATH, path, sizeof path));
    skiff_saves_baseline_load_result result = SKIFF_SAVES_BASELINE_LOADED;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_baseline_load(baseline, &storage.base, path, &result));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_BASELINE_NO_FILE, result);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_baseline_load(NULL, &storage.base, path, &result));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_baseline_save(baseline, NULL, path));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_baseline_create(NULL));
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_baseline_format(NULL, text, TEXT_MAX, &length));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_saves_baseline_parse(NULL, "{}", 2));
}

/* ---- The tree summary ---- */

static void put_file(const char *logical, const char *content) {
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, logical, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_replace_whole(posix, path, content, strlen(content)));
}

static void put_folder(const char *logical) {
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, logical, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, path));
}

static void summarize(skiff_saves_tree_summary *out) {
    const skiff_err err = skiff_saves_tree_summarize(&storage.base, &roots, SAVES, GAME, out);
    TEST_PRINTF("summary: %s, %u files, digest \"%s\"", skiff_err_name(err),
                (unsigned)out->file_count, out->digest);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, err);
}

static void test_an_unchanged_tree_is_told_without_reading_a_file(void) {
    put_folder(SAVES "/" DATA00);
    put_file(SAVES "/" DATA00 "/DATA.BIN", "slot zero");
    put_folder(SAVES "/" GAME "DATA01");
    put_file(SAVES "/" GAME "DATA01/DATA.BIN", "slot one");
    put_folder(SAVES "/" OTHER_GAME "DATA00");
    put_file(SAVES "/" OTHER_GAME "DATA00/DATA.BIN", "other");
    skiff_saves_tree_summary then;
    summarize(&then);
    TEST_ASSERT_EQUAL_UINT32(2, then.file_count);
    const int opens = storage.opens;
    skiff_saves_tree_summary now;
    summarize(&now);
    TEST_ASSERT_EQUAL_INT_MESSAGE(opens, storage.opens, "no file is opened");
    TEST_ASSERT_FALSE(skiff_saves_tree_changed(&then, &now));
    put_file(SAVES "/" OTHER_GAME "DATA00/DATA.BIN", "another game's change");
    summarize(&now);
    TEST_ASSERT_FALSE_MESSAGE(skiff_saves_tree_changed(&then, &now), "other games do not count");
    put_file(SAVES "/" DATA00 "/DATA.BIN", "slot zero, longer now");
    summarize(&now);
    TEST_ASSERT_TRUE_MESSAGE(skiff_saves_tree_changed(&then, &now), "a resized file counts");
    summarize(&then);
    put_file(SAVES "/" DATA00 "/NEW.BIN", "x");
    summarize(&now);
    TEST_ASSERT_TRUE_MESSAGE(skiff_saves_tree_changed(&then, &now), "an added file counts");
    TEST_ASSERT_EQUAL_UINT32(3, now.file_count);
}

/* Same size, new date: the file was rewritten. */
static void test_a_rewritten_file_changes_the_tree(void) {
    put_folder(SAVES "/" DATA00);
    put_file(SAVES "/" DATA00 "/DATA.BIN", "slot zero");
    skiff_saves_tree_summary then;
    summarize(&then);
    skiff_saves_tree_summary now = then;
    now.digest[0] = now.digest[0] == '0' ? '1' : '0';
    TEST_ASSERT_TRUE(skiff_saves_tree_changed(&then, &now));
    TEST_ASSERT_TRUE(skiff_saves_tree_changed(NULL, &now));
    TEST_ASSERT_TRUE(skiff_saves_tree_changed(&then, NULL));
}

static void test_a_missing_save_root_summarizes_as_no_files(void) {
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, SAVES, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_rmdir(posix, path));
    skiff_saves_tree_summary summary;
    summarize(&summary);
    TEST_ASSERT_EQUAL_UINT32(0, summary.file_count);
    TEST_ASSERT_EQUAL_STRING(SKIFF_SAVES_EMPTY_HASH, summary.digest);
}

/* A folder inside a save folder: no unit may hold it, so the tree always counts as changed and
 * the caller's collect reports why. */
static void test_a_tree_no_unit_may_hold_is_unknown(void) {
    put_folder(SAVES "/" DATA00 "/NESTED");
    skiff_saves_tree_summary summary;
    summarize(&summary);
    TEST_ASSERT_EQUAL_UINT32(SKIFF_SAVES_TREE_UNKNOWN, summary.file_count);
    TEST_ASSERT_TRUE(skiff_saves_tree_changed(&summary, &summary));
}

static void test_summarize_reports_storage_errors_and_bad_arguments(void) {
    put_folder(SAVES "/" DATA00);
    storage.list_error = SKIFF_ERR_STORAGE_IO;
    skiff_saves_tree_summary summary;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO,
                          skiff_saves_tree_summarize(&storage.base, &roots, SAVES, GAME, &summary));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_tree_summarize(NULL, &roots, SAVES, GAME, &summary));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_tree_summarize(&storage.base, &roots, SAVES, "x", &summary));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_saves_tree_summarize(&storage.base, &roots, SAVES, GAME, NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_decision_table_row_by_row);
    RUN_TEST(test_no_transfer_ever_overwrites_a_change);
    RUN_TEST(test_a_moved_server_id_with_the_same_content_is_no_change);
    RUN_TEST(test_decide_rejects_malformed_hashes);
    RUN_TEST(test_actions_and_reasons_have_log_names);
    RUN_TEST(test_the_baseline_round_trips_through_saves_json);
    RUN_TEST(test_record_replaces_a_game_and_forget_drops_it);
    RUN_TEST(test_forget_all_makes_every_difference_ask);
    RUN_TEST(test_record_refuses_entries_it_could_not_save);
    RUN_TEST(test_the_baseline_holds_512_games_and_saves_them_all);
    RUN_TEST(test_a_damaged_saves_json_starts_empty_and_is_set_aside);
    RUN_TEST(test_an_oversized_saves_json_is_damaged);
    RUN_TEST(test_a_failed_load_refuses_to_save);
    RUN_TEST(test_a_missing_saves_json_is_no_file);
    RUN_TEST(test_an_unchanged_tree_is_told_without_reading_a_file);
    RUN_TEST(test_a_rewritten_file_changes_the_tree);
    RUN_TEST(test_a_missing_save_root_summarizes_as_no_files);
    RUN_TEST(test_a_tree_no_unit_may_hold_is_unknown);
    RUN_TEST(test_summarize_reports_storage_errors_and_bad_arguments);
    return UNITY_END();
}
