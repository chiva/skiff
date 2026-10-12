/*
 * The app's state machine (skiff/app.h), driven frame by frame as the PSP drives it: a first launch
 * that asks for the server and pairs, the version policy, browsing pages, the download checks
 * (queue full, installed list full, replace), a finished download reaching installed.json, the
 * downloads screen, settings, and what the log may hold. The platform is a fake env, RomM the fake
 * transport, the Memory Stick a temporary directory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "skiff/app.h"
#include "skiff/install.h"
#include "skiff/jobs.h"
#include "skiff/saves.h"
#include "skiff/saves_sync.h"

#include "app_internal.h"
#include "fake_storage.h"
#include "fake_transport.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define SERVER "https://romm.test"
#define TOKEN "rmm_test_token_0123456789"
#define PAIRED_TOKEN "rmm_synthetic_pairing_token_rmm_synthetic_pairing_token_rmm_syntheti"
#define USER_CODE "SKIFF234"
#define PICKED_PROFILE 2
#define PATH_INIT "/api/auth/device/init"
#define PATH_TOKEN "/api/auth/device/token"
#define LIST_QUERY                                                                                 \
    "&order_by=name&order_dir=asc&with_char_index=false&with_filter_values=false"                  \
    "&with_rom_id_index=false"
#define JSON_OK "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
/* Makes a pairing address longer than the largest QR code holds. */
#define LONG_PAD                                                                                   \
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" \
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" \
    "xxxxxxxxxxxxxxxx"
#define RAW_MAX (64 * 1024)
#define FRAME_MS 16
#define FRAMES_MAX 2000
#define BODY_BYTES 3000U
#define TEXT_MAX (SKIFF_CONFIG_TEXT_MAX + 1)
/* The measure callback: every byte is this many units wide. */
#define CHAR_WIDTH 6.0f

/* ---- The fake platform ---- */

typedef struct proxy_transport {
    skiff_transport base;
    fake_transport *fake;
} proxy_transport;

typedef struct fake_env {
    int64_t now_ms;
    int switch_on;
    int join_polls;
    skiff_err net_start_error;
    skiff_err net_poll_error;
    /* Joins that fail (SKIFF_ERR_NET_WIFI_JOIN) before they work. */
    int join_failures;
    int net_starts;
    int started_profile;
    skiff_err tls_error;
    int opens;
    /* The CA file the last browsing transport was opened with. */
    char opened_ca_file[SKIFF_STORAGE_PATH_MAX];
    int worker_starts;
    int worker_stops;
    skiff_err stop_error;
    skiff_err start_error;
    skiff_app_worker_spec spec;
    /* The browsing thread: a call runs when the app first asks whether it is done, unless held
     * (a request that takes long); call_start_error refuses to start one. */
    skiff_app_call_fn call_fn;
    void *call_arg;
    int call_pending;
    int hold_calls;
    /* The call runs inside call_start(), as a thread that finishes before it returns would. */
    int run_at_start;
    int calls_started;
    int cancels;
    skiff_err call_start_error;
    /* How many times the PSP has suspended (env.suspends()). */
    uint32_t suspends;
    /* What env.net_online() says (something else joined the Wi-Fi), and how often it was asked. */
    int online;
    int online_asks;
} fake_env;

static char dir[TEMP_DIR_PATH_MAX];
static char app_dir[TEMP_DIR_PATH_MAX];
static skiff_storage_roots roots;
static skiff_storage *posix;
static fake_storage storage;
static fake_transport transport;
static fake_env env_state;
static skiff_app *app;
static unsigned char body[BODY_BYTES];
static uint32_t body_crc;

static skiff_err proxy_perform(skiff_transport *base, const skiff_http_request *request,
                               skiff_http_response *response) {
    proxy_transport *proxy = (proxy_transport *)base;
    return proxy->fake->base.ops->perform(&proxy->fake->base, request, response);
}

static void proxy_destroy(skiff_transport *base) { free(base); }

static const skiff_transport_ops PROXY_OPS = {proxy_perform, proxy_destroy};

static int64_t env_now(void *ctx) { return ((fake_env *)ctx)->now_ms; }

static float env_measure(void *ctx, const char *text) {
    (void)ctx;
    return CHAR_WIDTH * (float)strlen(text);
}

static int env_switch_on(void *ctx) { return ((fake_env *)ctx)->switch_on; }

static skiff_err env_net_start(void *ctx, int profile) {
    fake_env *e = ctx;
    e->net_starts++;
    e->started_profile = profile;
    return e->net_start_error;
}

static skiff_err env_net_poll(void *ctx, int *joined) {
    fake_env *e = ctx;
    if (e->net_poll_error != SKIFF_OK) {
        return e->net_poll_error;
    }
    if (e->join_failures > 0) {
        e->join_failures--;
        return SKIFF_ERR_NET_WIFI_JOIN;
    }
    *joined = e->join_polls-- <= 0;
    return SKIFF_OK;
}

static int env_net_online(void *ctx) {
    fake_env *e = ctx;
    e->online_asks++;
    return e->online;
}

static skiff_err env_net_profile(void *ctx, int *profile) {
    (void)ctx;
    *profile = PICKED_PROFILE;
    return SKIFF_OK;
}

static skiff_err env_profile_name(void *ctx, int profile, char *out, size_t size) {
    (void)ctx;
    snprintf(out, size, "Home %d", profile);
    return SKIFF_OK;
}

static skiff_err env_tls(void *ctx) { return ((fake_env *)ctx)->tls_error; }

static skiff_err env_open(void *ctx, const skiff_app_transport_settings *settings,
                          skiff_transport **out) {
    fake_env *e = ctx;
    if (settings != NULL) {
        snprintf(e->opened_ca_file, sizeof e->opened_ca_file, "%s", settings->ca_file);
    }
    proxy_transport *proxy = calloc(1, sizeof *proxy);
    TEST_ASSERT_NOT_NULL(proxy);
    proxy->base.ops = &PROXY_OPS;
    proxy->fake = &transport;
    e->opens++;
    *out = &proxy->base;
    return SKIFF_OK;
}

static skiff_err env_random(void *ctx, unsigned char *out, size_t size) {
    (void)ctx;
    for (size_t i = 0; i < size; i++) {
        out[i] = (unsigned char)(0x10U + i);
    }
    return SKIFF_OK;
}

static skiff_err env_start_worker(void *ctx, const skiff_app_worker_spec *spec) {
    fake_env *e = ctx;
    e->worker_starts++;
    if (e->start_error != SKIFF_OK) {
        return e->start_error;
    }
    e->spec = *spec;
    return SKIFF_OK;
}

static skiff_err env_stop_worker(void *ctx) {
    fake_env *e = ctx;
    e->worker_stops++;
    return e->stop_error;
}

static skiff_err env_call_start(void *ctx, skiff_app_call_fn fn, void *arg) {
    fake_env *e = ctx;
    TEST_ASSERT_FALSE_MESSAGE(e->call_pending, "a call started while another ran");
    if (e->call_start_error != SKIFF_OK) {
        return e->call_start_error;
    }
    e->call_fn = fn;
    e->call_arg = arg;
    e->call_pending = 1;
    e->calls_started++;
    if (e->run_at_start) {
        fn(arg);
        e->call_pending = 0;
    }
    return SKIFF_OK;
}

static int env_call_done(void *ctx) {
    fake_env *e = ctx;
    if (e->call_pending && !e->hold_calls) {
        e->call_fn(e->call_arg);
        e->call_pending = 0;
    }
    return !e->call_pending;
}

static void env_call_cancel(void *ctx) { ((fake_env *)ctx)->cancels++; }

static uint32_t env_suspends(void *ctx) { return ((fake_env *)ctx)->suspends; }

static skiff_app_env make_env(void) {
    const skiff_app_env env = {
        .ctx = &env_state,
        .now_ms = env_now,
        .measure = env_measure,
        .switch_on = env_switch_on,
        .net_start = env_net_start,
        .net_poll = env_net_poll,
        .net_online = env_net_online,
        .net_profile = env_net_profile,
        .net_profile_name = env_profile_name,
        .tls_start = env_tls,
        .open_transport = env_open,
        .random = env_random,
        .start_worker = env_start_worker,
        .stop_worker = env_stop_worker,
        .call_start = env_call_start,
        .call_done = env_call_done,
        .call_cancel = env_call_cancel,
        .suspends = env_suspends,
    };
    return env;
}

/* ---- Files and routes ---- */

static void app_file(const char *name, char *out, size_t size) {
    TEST_ASSERT_TRUE(snprintf(out, size, "%s/%s", app_dir, name) < (int)size);
}

static void write_file(const char *path, const char *text, size_t length) {
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(length, fwrite(text, 1, length, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void write_config(const char *text) {
    char path[TEMP_DIR_PATH_MAX];
    app_file(SKIFF_CONFIG_FILE_NAME, path, sizeof path);
    write_file(path, text, strlen(text));
}

static void read_file(const char *path, char *out, size_t size) {
    out[0] = '\0';
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return;
    }
    const size_t got = fread(out, 1, size - 1, file);
    out[got] = '\0';
    fclose(file);
}

static void read_app_file(const char *name, char *out, size_t size) {
    char path[TEMP_DIR_PATH_MAX];
    app_file(name, path, sizeof path);
    read_file(path, out, size);
}

static fake_route *serve_raw(const char *path, const char *raw) {
    fake_route *route = fake_transport_add_raw(&transport, path, raw, strlen(raw));
    TEST_ASSERT_NOT_NULL(route);
    return route;
}

static void serve_heartbeat(const char *version) {
    char raw[256];
    snprintf(raw, sizeof raw, JSON_OK "{\"SYSTEM\":{\"VERSION\":\"%s\"}}", version);
    serve_raw("/api/heartbeat", raw);
}

static void serve_platforms(void) {
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, "/api/platforms", "romm/platforms.http"));
}

/* ROM id's list item: "Game <id>.iso", or an unusable name when id ends in 13 (13 in the library,
 * 113 among the favourites). */
#define UNUSABLE_ID_END 13
#define ID_END_MODULUS 100
static int rom_item(char *out, size_t size, unsigned id) {
    if (id % ID_END_MODULUS == UNUSABLE_ID_END) {
        return snprintf(out, size,
                        "{\"id\":%u,\"platform_id\":1,\"name\":\"Bad\\u0007Name\",\"fs_name\":"
                        "\"Bad\\u0007Name.iso\",\"fs_size_bytes\":%u}",
                        id, BODY_BYTES);
    }
    return snprintf(out, size,
                    "{\"id\":%u,\"platform_id\":1,\"name\":\"Game %u\",\"fs_name\":\"Game %u.iso\","
                    "\"fs_size_bytes\":%u,\"crc_hash\":\"%08x\"}",
                    id, id, id, BODY_BYTES, (unsigned)body_crc);
}

/* ROM id's list item with its file, as a list asked with_files gives it. */
static int rom_item_with_file(char *out, size_t size, unsigned id) {
    char item[512];
    rom_item(item, sizeof item, id);
    item[strlen(item) - 1] = '\0';
    return snprintf(out, size,
                    "%s,\"files\":[{\"rom_id\":%u,\"file_name\":\"Game %u.iso\","
                    "\"file_size_bytes\":%u,\"crc_hash\":\"%08x\"}]}",
                    item, id, id, BODY_BYTES, (unsigned)body_crc);
}

/* A page of a list of total ROMs (the list's query ends with filter), from offset: ids from
 * first_id + offset + 1, with their files when the filter asks for them. */
static void serve_list_page(const char *filter, unsigned first_id, unsigned offset, unsigned count,
                            unsigned total) {
    static char raw[RAW_MAX];
    const int with_files = strstr(filter, "with_files=true") != NULL;
    int used = snprintf(raw, sizeof raw, JSON_OK "{\"items\":[");
    for (unsigned i = 0; i < count; i++) {
        used += snprintf(raw + used, sizeof raw - (size_t)used, i > 0 ? "," : "");
        const unsigned id = first_id + offset + i + 1;
        used += with_files ? rom_item_with_file(raw + used, sizeof raw - (size_t)used, id)
                           : rom_item(raw + used, sizeof raw - (size_t)used, id);
    }
    snprintf(raw + used, sizeof raw - (size_t)used, "],\"total\":%u,\"limit\":%d,\"offset\":%u}",
             total, SKIFF_ROMM_PAGE_SIZE, offset);
    char path[256];
    snprintf(path, sizeof path, "/api/roms?platform_ids=1&limit=%d&offset=%u" LIST_QUERY "%s",
             SKIFF_ROMM_PAGE_SIZE, offset, filter);
    serve_raw(path, raw);
}

/* A page of every game, ids from offset + 1, for a library of total. */
static void serve_page(unsigned offset, unsigned count, unsigned total) {
    serve_list_page("", 0, offset, count, total);
}

/* The player's favourites: total games, ids from FAVOURITE_ID_BASE + 1, on their first page. */
#define FAVOURITE_ID_BASE 100
#define FAVOURITES_FILTER "&favorite=true"
static void serve_favourites(unsigned total) {
    serve_list_page(FAVOURITES_FILTER, FAVOURITE_ID_BASE, 0,
                    total < SKIFF_ROMM_PAGE_SIZE ? total : SKIFF_ROMM_PAGE_SIZE, total);
}

/* What "Download all favourites" reads: every page of the favourites, with their files. */
#define BATCH_FILTER FAVOURITES_FILTER "&with_files=true"
static void serve_batch_favourites(unsigned total) {
    for (unsigned offset = 0; offset < total || offset == 0; offset += SKIFF_ROMM_PAGE_SIZE) {
        const unsigned left = total - offset;
        serve_list_page(BATCH_FILTER, FAVOURITE_ID_BASE, offset,
                        left < SKIFF_ROMM_PAGE_SIZE ? left : SKIFF_ROMM_PAGE_SIZE, total);
    }
}

/* ROM id's details, with RomM's cover path when cover_path is not NULL. */
static fake_route *serve_rom_with_cover(unsigned id, const char *cover_path) {
    char raw[2048];
    char item[512];
    char cover[SKIFF_ROMM_COVER_PATH_MAX + 32] = "";
    rom_item(item, sizeof item, id);
    /* The item without its closing brace, then its files. */
    item[strlen(item) - 1] = '\0';
    if (cover_path != NULL) {
        snprintf(cover, sizeof cover, ",\"path_cover_small\":\"%s\"", cover_path);
    }
    snprintf(raw, sizeof raw,
             JSON_OK "%s%s,\"files\":[{\"rom_id\":%u,\"file_name\":\"Game %u.iso\","
                     "\"file_size_bytes\":%u,\"crc_hash\":\"%08x\"}]}",
             item, cover, id, id, BODY_BYTES, (unsigned)body_crc);
    char path[64];
    snprintf(path, sizeof path, "/api/roms/%u", id);
    return serve_raw(path, raw);
}

static void serve_rom(unsigned id) { serve_rom_with_cover(id, NULL); }

/* A 2x2 RGBA PNG: red, green / blue, transparent white (the self-test's). */
static const unsigned char COVER_PNG[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xB6, 0x0D,
    0x24, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
    0x1F, 0x0C, 0x81, 0x34, 0x08, 0x30, 0x00, 0x00, 0x48, 0xC9, 0x08, 0xF8, 0xC5, 0x34, 0xFD, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
/* A cover path as RomM gives it, and the URL path Skiff requests it under. */
#define COVER_PATH(id)                                                                             \
    "/assets/romm/resources/roms/1/" #id "/cover/small.png?ts=2026-10-10 16:35:18"
#define COVER_URL(id)                                                                              \
    "/assets/romm/resources/roms/1/" #id "/cover/small.png?ts=2026-10-10%2016:35:18"
#define NEWER_COVER_PATH "/assets/romm/resources/roms/1/1/cover/small.png?ts=2026-10-11 08:00:00"
#define NEWER_COVER_URL "/assets/romm/resources/roms/1/1/cover/small.png?ts=2026-10-11%2008:00:00"

/* Answers the cover at url with status_line and body. */
static fake_route *serve_cover(const char *url, const char *status_line, const unsigned char *data,
                               size_t size) {
    static char raw[256 + sizeof COVER_PNG];
    const int used =
        snprintf(raw, sizeof raw, "%s\r\nContent-Type: image/png\r\n\r\n", status_line);
    TEST_ASSERT_LESS_THAN_size_t(sizeof raw, (size_t)used + size);
    memcpy(raw + used, data, size);
    fake_route *route = fake_transport_add_raw(&transport, url, raw, (size_t)used + size);
    TEST_ASSERT_NOT_NULL(route);
    return route;
}

static void serve_content(unsigned id) {
    static char raw[BODY_BYTES + 256];
    const int used =
        snprintf(raw, sizeof raw, "HTTP/1.1 200 OK\r\nETag: \"v1\"\r\nContent-Length: %u\r\n\r\n",
                 BODY_BYTES);
    memcpy(raw + used, body, BODY_BYTES);
    char path[64];
    snprintf(path, sizeof path, "/api/roms/%u/content/Game%%20%u.iso", id, id);
    TEST_ASSERT_NOT_NULL(fake_transport_add_raw(&transport, path, raw, (size_t)used + BODY_BYTES));
}

static void serve_library(unsigned total) {
    serve_heartbeat("5.3.1");
    serve_platforms();
    serve_page(0, total < SKIFF_ROMM_PAGE_SIZE ? total : SKIFF_ROMM_PAGE_SIZE, total);
}

/* ---- Driving the app ---- */

/* 2026-09-21 14:13:20 UTC, plus the fake's monotonic clock. */
#define UTC_BASE_MS 1790000000000LL

static int fake_utc(void *ctx, int64_t *unix_ms) {
    (void)ctx;
    *unix_ms = UTC_BASE_MS + env_state.now_ms;
    return 1;
}

static void create_app(void) {
    const skiff_app_config config = {.storage = &storage.base, .roots = roots, .clock = fake_utc};
    const skiff_app_env env = make_env();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_app_create(&config, &env, &app));
}

static const skiff_app_view *view(void) { return skiff_app_view_now(app); }

/* Only Settings sets a row apart (Saves), and only in a list that never scrolls (skiff/app.h). */
static void check_new_groups(void) {
    const skiff_app_view *v = view();
    for (size_t i = 0; i < v->row_count; i++) {
        if (v->rows[i].new_group) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_APP_SCREEN_SETTINGS, v->screen,
                                          "only Settings starts a new group of rows");
            TEST_ASSERT_TRUE_MESSAGE(v->list.count <= v->list.rows,
                                     "a list with a new group never scrolls");
        }
    }
}

static void frame(unsigned actions) {
    skiff_app_update(app, actions);
    check_new_groups();
    env_state.now_ms += FRAME_MS;
}

static const char *screen_name(skiff_app_screen screen) {
    static const char *const NAMES[] = {"starting", "server",  "connecting", "pair",    "library",
                                        "details",  "queue",   "settings",   "message", "confirm",
                                        "saves",    "backups", "save choice"};
    return NAMES[screen];
}

static void print_view(void) {
    const skiff_app_view *v = view();
    TEST_PRINTF("[%s] %s | %s", screen_name(v->screen), v->title, v->status);
    for (size_t i = 0; i < v->line_count; i++) {
        TEST_PRINTF("   %s", v->lines[i]);
    }
    for (size_t i = 0; i < v->row_count; i++) {
        if (v->rows[i].new_group) {
            TEST_PRINTF("    (new group)");
        }
        TEST_PRINTF("  %c %s | %s%s%s", v->list.first + i == v->list.selected ? '>' : ' ',
                    v->rows[i].label, v->rows[i].detail, v->rows[i].dim ? " (dim)" : "",
                    v->rows[i].attention ? " (attention)" : "");
    }
    for (size_t t = 0; t < v->table_count; t++) {
        const skiff_app_table *table = &v->tables[t];
        TEST_PRINTF("   table %zu: columns at %d %d %d %d, dim from %zu", t,
                    (int)table->column_x[0], (int)table->column_x[1], (int)table->column_x[2],
                    (int)table->column_x[3], table->dim_from);
        for (size_t r = 0; r < table->row_count; r++) {
            const skiff_app_table_row *row = &table->rows[r];
            TEST_PRINTF("   [action %u] %s | %s | %s | %s", row->action, row->cells[0],
                        row->cells[1], row->cells[2], row->cells[3]);
        }
    }
}

/* Frames with no input until the screen is screen and no request is pending. */
static void run_until(skiff_app_screen screen) {
    for (int i = 0; i < FRAMES_MAX; i++) {
        frame(0);
        const int pairing_settled = screen != SKIFF_APP_SCREEN_PAIR || app->pairing.active ||
                                    app->pairing.ended != SKIFF_OK;
        if (view()->screen == screen && view()->dialog == SKIFF_APP_DIALOG_NONE &&
            app->request == REQUEST_NONE && !app_call_busy(app) && pairing_settled) {
            print_view();
            return;
        }
        if (view()->screen == SKIFF_APP_SCREEN_MESSAGE && screen != SKIFF_APP_SCREEN_MESSAGE) {
            break;
        }
    }
    print_view();
    TEST_ASSERT_EQUAL_STRING(screen_name(screen), screen_name(view()->screen));
}

static int shows(const char *text) {
    const skiff_app_view *v = view();
    for (size_t i = 0; i < v->line_count; i++) {
        if (strstr(v->lines[i], text) != NULL) {
            return 1;
        }
    }
    for (size_t i = 0; i < v->row_count; i++) {
        if (strstr(v->rows[i].label, text) != NULL || strstr(v->rows[i].detail, text) != NULL) {
            return 1;
        }
    }
    for (size_t t = 0; t < v->table_count; t++) {
        for (size_t r = 0; r < v->tables[t].row_count; r++) {
            for (size_t c = 0; c < SKIFF_APP_TABLE_COLUMNS; c++) {
                if (strstr(v->tables[t].rows[r].cells[c], text) != NULL) {
                    return 1;
                }
            }
        }
    }
    return strstr(v->status, text) != NULL || strstr(v->title, text) != NULL;
}

static int hints(unsigned action, const char *label) {
    for (size_t i = 0; i < view()->hint_count; i++) {
        if (view()->hints[i].action == action && strcmp(view()->hints[i].label, label) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *english(skiff_text_id id) { return skiff_text(SKIFF_LANGUAGE_ENGLISH, id); }

/* What a token paired by this Skiff holds, with and without the optional collections scope. */
#define BROWSE_SCOPES "platforms.read,roms.read,assets.read"
#define COLLECTIONS_SCOPES "platforms.read,roms.read,collections.read"

/* A paired library of total games, its token holding scopes. */
static void open_library_with_scopes(unsigned total, const char *scopes) {
    char config[TEXT_MAX];
    snprintf(config, sizeof config,
             "[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN
             "\ndevice_identifier = 00112233445566778899aabbccddeeff\nscopes = %s\n[network]\n"
             "profile = 1\n",
             scopes);
    write_config(config);
    serve_library(total);
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
}

static void open_paired_library(unsigned total) { open_library_with_scopes(total, BROWSE_SCOPES); }

static void wait_ms(int64_t ms) {
    const int64_t until = env_state.now_ms + ms;
    while (env_state.now_ms < until) {
        frame(0);
    }
}

/* Frames until the request a frame started has come back and been applied. */
static void finish_call(void) {
    for (int i = 0; i < FRAMES_MAX && app_call_busy(app); i++) {
        frame(0);
    }
    TEST_ASSERT_FALSE(app_call_busy(app));
}

static void open_details(unsigned id) {
    serve_rom(id);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
}

/* Opens the selected ROM's details where the request fails. */
static void open_details_failing(void) {
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_MESSAGE);
}

/* installed.json holding "Game <id>.iso" for each of ids, recorded with crcs. */
static void record_installs(const unsigned *ids, const uint32_t *crcs, size_t count) {
    skiff_install_manifest *manifest = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_create(&manifest));
    for (size_t i = 0; i < count; i++) {
        char iso[TEMP_DIR_PATH_MAX];
        char name[64];
        snprintf(name, sizeof name, "ISO/Game %u.iso", ids[i]);
        TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO", iso, sizeof iso));
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, iso));
        TEST_ASSERT_TRUE(temp_dir_path(dir, name, iso, sizeof iso));
        write_file(iso, (const char *)body, BODY_BYTES);
        skiff_install_record record;
        memset(&record, 0, sizeof record);
        record.rom_id = ids[i];
        snprintf(record.file_name, sizeof record.file_name, "Game %u.iso", ids[i]);
        snprintf(record.path, sizeof record.path, "games:/Game %u.iso", ids[i]);
        record.size = BODY_BYTES;
        record.has_crc32 = 1;
        record.crc32 = crcs[i];
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_record(manifest, &record));
    }
    char path[TEMP_DIR_PATH_MAX];
    app_file(SKIFF_INSTALL_MANIFEST_NAME, path, sizeof path);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_save(manifest, posix, path));
    skiff_install_manifest_destroy(manifest);
}

static void record_installed(unsigned id, uint32_t crc) { record_installs(&id, &crc, 1); }

static size_t list_jobs(skiff_job *out) { return skiff_jobs_list(app->jobs, out, SKIFF_JOBS_MAX); }

/* ---- The worker, as jobs_psp runs it ---- */

static skiff_err worker_open(void *ctx, skiff_transport **out) { return env_open(ctx, NULL, out); }
static int worker_yes(void *ctx) {
    (void)ctx;
    return 1;
}
static skiff_err worker_ok(void *ctx) {
    (void)ctx;
    return SKIFF_OK;
}
static uint32_t worker_suspends(void *ctx) {
    (void)ctx;
    return 0;
}
static void worker_awake(void *ctx) { (void)ctx; }
static void worker_sleep(void *ctx, uint32_t ms) { ((fake_env *)ctx)->now_ms += ms; }

static void run_worker_once(void) {
    const skiff_jobs_env jobs_env = {worker_open,  worker_yes,      worker_yes,         worker_ok,
                                     worker_ok,    worker_suspends, worker_awake,       env_now,
                                     worker_sleep, &env_state,      env_state.spec.romm};
    int ran = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_run_one(env_state.spec.jobs, &jobs_env, &ran));
    TEST_ASSERT_EQUAL_INT(1, ran);
}

/* ---- Fixture ---- */

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    TEST_ASSERT_TRUE(temp_dir_path(dir, "PSP/GAME/Skiff", app_dir, sizeof app_dir));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_init(dir, app_dir, &roots));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&posix));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, app_dir));
    fake_storage_init(&storage, posix);
    storage.has_free_bytes = 1;
    storage.free_bytes = (uint64_t)1536 * 1024 * 1024;
    fake_transport_init(&transport);
    memset(&env_state, 0, sizeof env_state);
    env_state.now_ms = 1000;
    env_state.switch_on = 1;
    for (unsigned i = 0; i < BODY_BYTES; i++) {
        body[i] = (unsigned char)(i * 7U + 3U);
    }
    body_crc = (uint32_t)crc32(0L, body, BODY_BYTES);
    app = NULL;
}

void tearDown(void) {
    skiff_app_destroy(app);
    app = NULL;
    skiff_transport_destroy(&transport.base);
    skiff_storage_destroy(&storage.base);
    skiff_storage_destroy(posix);
    temp_dir_remove(dir);
}

/* ---- First launch ---- */

static void test_a_first_launch_asks_for_the_server_then_pairs(void) {
    serve_heartbeat("5.3.1");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    fake_route *pending =
        fake_transport_add_fixture(&transport, PATH_TOKEN, "romm/device-pending.http");
    TEST_ASSERT_NOT_NULL(pending);
    pending->max_uses = 1;
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_TOKEN, "romm/device-token.http"));
    serve_platforms();
    serve_page(0, 3, 3);
    create_app();
    TEST_PRINTF("no config.ini: the player types the server's address");
    run_until(SKIFF_APP_SCREEN_SERVER);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_SERVER_PROMPT)) ||
                     shows("Enter the address of your RomM"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_DIALOG_KEYBOARD, view()->dialog);
    TEST_ASSERT_EQUAL_STRING("https://", view()->dialog_text);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, SERVER);
    TEST_PRINTF("no connection saved yet: the network picker opens, and its choice is kept");
    for (int i = 0; i < 4 && view()->dialog != SKIFF_APP_DIALOG_NETWORK; i++) {
        frame(0);
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_DIALOG_NETWORK, view()->dialog);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, NULL);
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_PRINTF("the code in small text, to check against RomM's page; the wait in the header");
    TEST_ASSERT_TRUE(shows("Code " USER_CODE ", expires in 10 min"));
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_PAIR_WAITING), view()->status);
    TEST_PRINTF("the address carries the code (RomM's page has no field to type it into), broken "
                "before it so the code stays whole");
    TEST_ASSERT_TRUE(shows(SERVER "/pair/device"));
    TEST_ASSERT_TRUE(shows("?user_code=" USER_CODE));
    TEST_PRINTF("and a QR code of it, with the text wrapped beside it");
    TEST_ASSERT_NOT_NULL(view()->qr);
    TEST_ASSERT_TRUE(view()->qr->size > 0);
    TEST_ASSERT_TRUE(shows("Scan the QR code"));
    for (size_t i = 0; i < view()->line_count; i++) {
        TEST_ASSERT_TRUE_MESSAGE(env_measure(NULL, view()->lines[i]) <= SKIFF_APP_QR_TEXT_WIDTH,
                                 view()->lines[i]);
    }
    char config[TEXT_MAX];
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_PRINTF("config.ini:\n%s", config);
    TEST_ASSERT_NOT_NULL(strstr(config, "url = " SERVER));
    TEST_ASSERT_NOT_NULL(strstr(config, "profile = 2"));
    TEST_PRINTF("the identifier comes from the random hook, saved before pairing starts");
    TEST_ASSERT_NOT_NULL(strstr(config, "device_identifier = 101112131415161718191a1b1c1d1e1f"));
    TEST_PRINTF("polls every 5 s: pending, then approved");
    const size_t requests = transport.request_count;
    wait_ms(4900);
    TEST_ASSERT_EQUAL_size_t(requests, transport.request_count);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_ASSERT_NOT_NULL(strstr(config, "token = " PAIRED_TOKEN));
    TEST_ASSERT_NOT_NULL(strstr(config, "device_id = 00000000-0000-4000-8000-000000000001"));
    TEST_ASSERT_NOT_NULL(strstr(config, "scopes = platforms.read,roms.read,assets.read"));
    TEST_ASSERT_EQUAL_size_t(3, view()->row_count);
    TEST_ASSERT_EQUAL_STRING("Game 1", view()->rows[0].label);
    TEST_PRINTF("the worker runs with the new token and the picked connection");
    TEST_ASSERT_EQUAL_INT(1, env_state.worker_starts);
    TEST_ASSERT_EQUAL_INT(PICKED_PROFILE, env_state.spec.profile);
    TEST_ASSERT_EQUAL_STRING("Bearer " PAIRED_TOKEN, env_state.spec.romm->authorization);
    TEST_PRINTF("neither the token nor the device code reaches skiff.log");
    skiff_app_destroy(app);
    app = NULL;
    char log[RAW_MAX];
    read_app_file(SKIFF_APP_LOG_FILE_NAME, log, sizeof log);
    TEST_PRINTF("skiff.log:\n%s", log);
    TEST_ASSERT_NOT_NULL(strstr(log, "paired as device"));
    TEST_ASSERT_NULL(strstr(log, PAIRED_TOKEN));
    TEST_ASSERT_NULL(strstr(log, "skiff-device-code"));
}

static void test_a_slow_down_waits_five_seconds_more(void) {
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    fake_route *slow =
        fake_transport_add_fixture(&transport, PATH_TOKEN, "romm/device-slow-down.http");
    TEST_ASSERT_NOT_NULL(slow);
    slow->max_uses = 1;
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_TOKEN, "romm/device-pending.http"));
    create_app();
    run_until(SKIFF_APP_SCREEN_PAIR);
    wait_ms(5000);
    finish_call();
    TEST_ASSERT_EQUAL_INT(1, slow->uses);
    TEST_ASSERT_EQUAL_UINT32(10, app->pairing.pairing.interval_s);
    const size_t requests = transport.request_count;
    wait_ms(9900);
    TEST_PRINTF("%zu request(s) in the 9.9 s after the slow_down",
                transport.request_count - requests);
    TEST_ASSERT_EQUAL_size_t(requests, transport.request_count);
    wait_ms(200);
    TEST_ASSERT_EQUAL_size_t(requests + 1, transport.request_count);
}

static void test_the_first_free_space_query_is_made_while_starting(void) {
    TEST_PRINTF("the first query counts the whole Memory Stick (3 s on a PSP): made at startup, "
                "not when a screen that shows free space first opens");
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    create_app();
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_PRINTF("free space queries before any screen showed it: %d", storage.free_space_queries);
    TEST_ASSERT_EQUAL_INT(1, storage.free_space_queries);
    TEST_ASSERT_TRUE(app->free_known);
}

static void test_the_first_free_space_count_runs_off_the_screen_while_the_wifi_joins(void) {
    open_paired_library(1);
    skiff_app_destroy(app);
    app = NULL;
    storage.free_space_queries = 0;
    TEST_PRINTF("the count is held on the browsing thread: the screen goes on and the Wi-Fi joins");
    env_state.hold_calls = 1;
    const int joins = env_state.net_starts;
    create_app();
    for (int i = 0; i < FRAMES_MAX && env_state.net_starts == joins; i++) {
        frame(0);
    }
    TEST_ASSERT_EQUAL_INT(joins + 1, env_state.net_starts);
    TEST_ASSERT_EQUAL_INT(CALL_FREE_SPACE, app->call.kind);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, storage.free_space_queries, "not on the screen's thread");
    TEST_ASSERT_NOT_NULL_MESSAGE(app->manifest,
                                 "started after the files the starting screen reads itself");
    TEST_ASSERT_TRUE(app->free_pending);
    TEST_ASSERT_FALSE(app->free_known);
    TEST_PRINTF("a screen that shows free space meanwhile asks nothing more, and shows none yet");
    app_set_screen(app, SKIFF_APP_SCREEN_SETTINGS);
    TEST_ASSERT_EQUAL_INT(0, storage.free_space_queries);
    TEST_ASSERT_FALSE(shows("Free space"));
    app_set_screen(app, SKIFF_APP_SCREEN_CONNECTING);
    env_state.hold_calls = 0;
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_INT(1, storage.free_space_queries);
    TEST_ASSERT_FALSE(app->free_pending);
    TEST_ASSERT_TRUE(app->free_known);
    TEST_PRINTF("once known, the screens show it and ask again as before");
    frame(SKIFF_UI_ACTION_MENU);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SETTINGS, view()->screen);
    TEST_ASSERT_EQUAL_INT(2, storage.free_space_queries);
    TEST_ASSERT_TRUE(shows("Free space"));
    TEST_ASSERT_EQUAL_UINT64(storage.free_bytes, app->free_bytes);
}

static void test_a_failed_free_space_count_shows_none_and_is_asked_again_later(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_library(1);
    storage.free_space_error = SKIFF_ERR_STORAGE_IO;
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_FALSE(app->free_known);
    TEST_ASSERT_FALSE(app->free_pending);
    storage.free_space_error = SKIFF_OK;
    frame(SKIFF_UI_ACTION_MENU);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SETTINGS, view()->screen);
    TEST_ASSERT_TRUE(app->free_known);
}

static void test_a_denied_or_expired_pairing_offers_a_new_code(void) {
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    fake_route *init = fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http");
    TEST_ASSERT_NOT_NULL(init);
    fake_route *denied =
        fake_transport_add_fixture(&transport, PATH_TOKEN, "romm/device-denied.http");
    denied->max_uses = 1;
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_TOKEN, "romm/device-expired.http"));
    create_app();
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_ASSERT_NOT_NULL(view()->qr);
    wait_ms(5100);
    print_view();
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_PAIR_DENIED)));
    TEST_PRINTF("an ended pairing shows no code to scan");
    TEST_ASSERT_NULL(view()->qr);
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_NEW_CODE)));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_ASSERT_EQUAL_INT(2, init->uses);
    wait_ms(5100);
    print_view();
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_PAIR_EXPIRED)));
}

static void test_an_address_too_long_for_a_qr_code_is_shown_as_text_only(void) {
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    TEST_PRINTF("a path past the 213 bytes the largest QR code holds");
    serve_raw(PATH_INIT, "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n\r\n"
                         "{\"device_code\":\"skiff-device-code\",\"user_code\":\"" USER_CODE "\","
                         "\"verification_path\":\"/pair/device\",\"verification_path_complete\":"
                         "\"/pair/device?user_code=" USER_CODE "&pad=" LONG_PAD "\","
                         "\"expires_in\":600,\"interval\":5}");
    create_app();
    run_until(SKIFF_APP_SCREEN_PAIR);
    print_view();
    TEST_ASSERT_NULL(view()->qr);
    TEST_ASSERT_FALSE(shows("Scan the QR code"));
    TEST_ASSERT_TRUE(shows("user_code=" USER_CODE));
    TEST_ASSERT_TRUE(shows("Code " USER_CODE));
}

/* The body lines one after another, without breaks: an address wrapped over several lines reads
 * whole in it. */
static void joined_lines(char *out, size_t size) {
    out[0] = '\0';
    for (size_t i = 0; i < view()->line_count; i++) {
        strncat(out, view()->lines[i], size - strlen(out) - 1);
    }
}

static void test_a_long_path_before_the_code_is_shown_whole(void) {
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    TEST_PRINTF(
        "no QR code fits: the address is the only way to pair (RomM's page has no field for "
        "the code), so a long path must not push its ?user_code= off the screen");
    serve_raw(PATH_INIT, "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n\r\n"
                         "{\"device_code\":\"skiff-device-code\",\"user_code\":\"" USER_CODE "\","
                         "\"verification_path\":\"/pair/device\",\"verification_path_complete\":"
                         "\"/pair/device/" LONG_PAD LONG_PAD "?user_code=" USER_CODE "\","
                         "\"expires_in\":600,\"interval\":5}");
    fake_route *down = serve_raw(PATH_TOKEN, JSON_OK "{}");
    down->fail_before_response = SKIFF_ERR_NET_CONNECT;
    create_app();
    run_until(SKIFF_APP_SCREEN_PAIR);
    wait_ms(5100);
    print_view();
    TEST_ASSERT_NULL(view()->qr);
    TEST_ASSERT_TRUE(view()->line_count <= SKIFF_APP_LINES_MAX);
    char joined[SKIFF_APP_LINES_MAX * SKIFF_TEXT_MAX];
    joined_lines(joined, sizeof joined);
    TEST_ASSERT_NOT_NULL(
        strstr(joined, SERVER "/pair/device/" LONG_PAD LONG_PAD "?user_code=" USER_CODE));
    TEST_ASSERT_TRUE(shows("?user_code=" USER_CODE));
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_PAIR_APPROVE)));
    TEST_ASSERT_TRUE(shows("Code " USER_CODE));
}

static void test_the_longest_address_leaves_room_for_the_code_and_an_error(void) {
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    TEST_PRINTF("an address of about 470 characters: seven full-width lines on its own");
    serve_raw(PATH_INIT, "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n\r\n"
                         "{\"device_code\":\"skiff-device-code\",\"user_code\":\"" USER_CODE "\","
                         "\"verification_path\":\"/pair/device\",\"verification_path_complete\":"
                         "\"/pair/device?user_code=" USER_CODE "&pad=" LONG_PAD LONG_PAD
                         "\",\"expires_in\":600,\"interval\":5}");
    fake_route *down = serve_raw(PATH_TOKEN, JSON_OK "{}");
    down->fail_before_response = SKIFF_ERR_NET_CONNECT;
    create_app();
    run_until(SKIFF_APP_SCREEN_PAIR);
    wait_ms(5100);
    print_view();
    TEST_PRINTF("a failed poll adds an error: the address gives up lines, the code and error stay");
    TEST_ASSERT_TRUE(view()->line_count <= SKIFF_APP_LINES_MAX);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_PAIR_APPROVE)));
    TEST_ASSERT_TRUE(shows("Code " USER_CODE));
    TEST_ASSERT_TRUE(shows("[102]"));
}

static void test_a_code_that_runs_out_on_the_psp_ends_the_pairing(void) {
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    TEST_PRINTF("polls that fail on the network keep trying until the code expires");
    fake_route *down = serve_raw(PATH_TOKEN, JSON_OK "{}");
    down->fail_before_response = SKIFF_ERR_NET_CONNECT;
    create_app();
    run_until(SKIFF_APP_SCREEN_PAIR);
    wait_ms(5100);
    print_view();
    TEST_ASSERT_TRUE(shows("[102]"));
    wait_ms((int64_t)600 * 1000);
    print_view();
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_PAIR_EXPIRED)));
}

/* ---- The version policy ---- */

static void test_an_old_romm_is_refused(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.2.9");
    create_app();
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows("[205]"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_QUIT)));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_MENU, english(SKIFF_TEXT_SETTINGS)));
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_TRUE(skiff_app_quit_requested(app));
    TEST_ASSERT_EQUAL_INT(0, env_state.worker_starts);
}

static void test_a_newer_romm_is_noticed_once(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.5.0");
    serve_platforms();
    serve_page(0, 1, 1);
    create_app();
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows("RomM 5.5.0 is newer"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    char config[TEXT_MAX];
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_ASSERT_NOT_NULL(strstr(config, "romm_notice = 5.5"));
    TEST_PRINTF("the next launch says nothing");
    skiff_app_destroy(app);
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
}

/* ---- Startup problems ---- */

static void test_a_bad_config_names_its_line_and_quits(void) {
    write_config("[server]\nurl = romm.test\n");
    create_app();
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows("[402]"));
    TEST_ASSERT_TRUE(shows("config.ini, line 2: [server] url"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_TRUE(skiff_app_quit_requested(app));
}

static void test_without_ark_tls_refuses_and_says_why(void) {
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    env_state.tls_error = SKIFF_ERR_NET_NEEDS_ARK;
    create_app();
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows("[109]"));
    TEST_ASSERT_EQUAL_size_t(0, transport.request_count);
}

static void test_the_wifi_switch_and_a_failed_join(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 3\n");
    serve_library(1);
    env_state.switch_on = 0;
    create_app();
    for (int i = 0; i < 10; i++) {
        frame(0);
    }
    print_view();
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_NET_WAITING_SWITCH)));
    TEST_ASSERT_EQUAL_INT(0, env_state.net_starts);
    env_state.switch_on = 1;
    env_state.join_polls = 3;
    env_state.net_poll_error = SKIFF_ERR_NET_WIFI_JOIN;
    frame(0);
    frame(0);
    TEST_ASSERT_EQUAL_INT(3, env_state.started_profile);
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_PRINTF("shown after the first join and %d more: %d joins", SKIFF_APP_JOIN_RETRIES,
                env_state.net_starts);
    TEST_ASSERT_EQUAL_INT(1 + SKIFF_APP_JOIN_RETRIES, env_state.net_starts);
    TEST_ASSERT_TRUE(shows("[110]"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_MENU, english(SKIFF_TEXT_CHOOSE_NETWORK)));
    TEST_PRINTF("retry joins the same connection; the picker is the other choice");
    env_state.net_poll_error = SKIFF_OK;
    frame(SKIFF_UI_ACTION_MENU);
    frame(0);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_DIALOG_NETWORK, view()->dialog);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, NULL);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_INT(PICKED_PROFILE, env_state.spec.profile);
}

static void test_a_refused_join_is_tried_again_before_the_player_sees_it(void) {
    TEST_PRINTF("about a third of joins fail once on a PSP-1000 (J1, A1); the next one works");
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_library(1);
    env_state.join_failures = SKIFF_APP_JOIN_RETRIES;
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_PRINTF("joins: %d", env_state.net_starts);
    TEST_ASSERT_EQUAL_INT(1 + SKIFF_APP_JOIN_RETRIES, env_state.net_starts);
    char log[TEXT_MAX];
    read_app_file(SKIFF_APP_LOG_FILE_NAME, log, sizeof log);
    TEST_ASSERT_NOT_NULL(strstr(log, "trying again (2 of 2)"));
}

/* ---- Browsing ---- */

static void test_pages_load_as_the_player_scrolls(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_library(30);
    serve_page(25, 5, 30);
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows("30 games"));
    TEST_ASSERT_EQUAL_size_t(30, view()->list.count);
    TEST_PRINTF("ROM 13's name has a control character: listed with '?', dimmed");
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    int found = 0;
    for (size_t i = 0; i < view()->row_count; i++) {
        if (strstr(view()->rows[i].label, "Bad?Name") != NULL) {
            found = 1;
            TEST_ASSERT_TRUE(view()->rows[i].dim);
        } else {
            TEST_ASSERT_FALSE(view()->rows[i].dim);
        }
    }
    TEST_ASSERT_TRUE(found);
    TEST_PRINTF("the second page loads once its rows come on screen");
    const size_t requests = transport.request_count;
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_size_t(requests + 1, transport.request_count);
    TEST_ASSERT_TRUE(shows("Game 30"));
}

static void test_the_library_fills_the_body(void) {
    const int rows = SKIFF_APP_ROWS_FIT(0, 0);
    const int used = SKIFF_APP_BLOCK_GAP + rows * SKIFF_APP_LINE_HEIGHT;
    TEST_PRINTF("%d rows take %d of the body's %d pixels", rows, used, SKIFF_APP_BODY_HEIGHT);
    TEST_ASSERT_TRUE(used <= SKIFF_APP_BODY_HEIGHT);
    TEST_ASSERT_TRUE(used + SKIFF_APP_LINE_HEIGHT > SKIFF_APP_BODY_HEIGHT);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_ROWS_MAX, rows);
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_library(30);
    serve_page(25, 5, 30);
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_size_t(0, view()->line_count);
    TEST_ASSERT_EQUAL_size_t((size_t)rows, view()->list.rows);
    TEST_ASSERT_EQUAL_size_t((size_t)rows, view()->row_count);
}

static void test_a_library_without_psp_games_says_so(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    serve_raw("/api/platforms", JSON_OK "[]");
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_LIBRARY_EMPTY)));
    TEST_PRINTF("the downloads still run: the worker starts");
    TEST_ASSERT_EQUAL_INT(1, env_state.worker_starts);
}

/* ---- Favourites ---- */

static void test_select_switches_the_library_to_favourites_and_back(void) {
    open_paired_library(30);
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_SELECT, english(SKIFF_TEXT_SHOW_FAVOURITES)));
    serve_favourites(3);
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_FAVOURITES), view()->title);
    TEST_ASSERT_TRUE(shows("3 games"));
    TEST_ASSERT_TRUE(shows("Game 101"));
    TEST_ASSERT_FALSE(shows("Game 1.iso"));
    TEST_PRINTF("the favourites start at their top, whatever was selected before");
    TEST_ASSERT_EQUAL_size_t(0, view()->list.selected);
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_SELECT, english(SKIFF_TEXT_SHOW_COLLECTIONS)));

    TEST_PRINTF("SELECT: the collections (this token cannot list them: no request)");
    const size_t requests = transport.request_count;
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_COLLECTIONS), view()->title);
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_SELECT, english(SKIFF_TEXT_SHOW_ALL_GAMES)));
    TEST_ASSERT_EQUAL_size_t(requests, transport.request_count);

    TEST_PRINTF("SELECT again: every game, from their first page again");
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_LIBRARY), view()->title);
    TEST_ASSERT_TRUE(shows("30 games"));
    TEST_ASSERT_TRUE(shows("Game 1"));
    TEST_ASSERT_EQUAL_size_t(requests + 1, transport.request_count);
}

static void test_favourites_without_any_say_where_to_mark_them(void) {
    open_paired_library(2);
    serve_favourites(0);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows("RomM"));
    TEST_ASSERT_TRUE(view()->line_count > 0);
    TEST_ASSERT_FALSE(view()->has_list);
    TEST_PRINTF("still switchable, and nothing to open");
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_SELECT, english(SKIFF_TEXT_SHOW_COLLECTIONS)));
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_SELECT)));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
}

static void test_a_page_of_the_list_left_behind_is_dropped(void) {
    open_paired_library(30);
    serve_favourites(3);
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_SELECT);
    frame(0);
    TEST_ASSERT_TRUE(app_call_busy(app));
    TEST_PRINTF("on to the collections and every game while the favourites' first page is on its "
                "way");
    frame(SKIFF_UI_ACTION_SELECT);
    frame(SKIFF_UI_ACTION_SELECT);
    env_state.hold_calls = 0;
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_LIBRARY), view()->title);
    TEST_ASSERT_TRUE(shows("30 games"));
    TEST_ASSERT_FALSE(shows("Game 101"));
    TEST_PRINTF("the favourites' page was not cancelled with its connection");
    TEST_ASSERT_EQUAL_INT(0, env_state.cancels);
}

static void test_back_from_a_favourites_details_returns_to_the_favourites(void) {
    open_paired_library(2);
    serve_favourites(3);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_DOWN);
    open_details(102);
    TEST_ASSERT_TRUE(shows("Game 102.iso"));
    const size_t requests = transport.request_count;
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_FAVOURITES), view()->title);
    TEST_ASSERT_EQUAL_size_t(1, view()->list.selected);
    TEST_PRINTF("from the page kept in memory: no request");
    TEST_ASSERT_EQUAL_size_t(requests, transport.request_count);
}

static void test_a_library_without_psp_games_offers_no_favourites(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    serve_raw("/api/platforms", JSON_OK "[]");
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_SELECT, english(SKIFF_TEXT_SHOW_FAVOURITES)));
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_LIBRARY_EMPTY)));
}

/* ---- Collections ---- */

/* A collection as RomM lists it: Skiff reads its id, name, kind and count, and skips the ROM ids
 * and covers RomM sends with it. */
static int collection_item(char *out, size_t size, unsigned id, const char *name, unsigned count,
                           int smart, int favourite) {
    return snprintf(out, size,
                    "{\"name\":\"%s\",\"description\":\"\",\"rom_ids\":[1,2],"
                    "\"path_covers_small\":[],\"is_public\":false,\"is_favorite\":%s,"
                    "\"is_virtual\":false,\"is_smart\":%s,\"id\":%u,\"rom_count\":%u}",
                    name, favourite ? "true" : "false", smart ? "true" : "false", id, count);
}

/* RomM's two collections lists, each a JSON array's items. */
static void serve_collections(const char *regular, const char *smart) {
    static char raw[RAW_MAX];
    snprintf(raw, sizeof raw, JSON_OK "[%s]", regular);
    serve_raw("/api/collections", raw);
    snprintf(raw, sizeof raw, JSON_OK "[%s]", smart);
    serve_raw("/api/collections/smart", raw);
}

/* The player's collections: their favourites (not listed), a shelf of SHELF_GAMES and an empty
 * shelf, then a smart shelf. */
#define SHELF_ID 5
#define EMPTY_SHELF_ID 6
#define SMART_SHELF_ID 2
#define SHELF_GAMES 30
#define SHELF_FILTER "&collection_id=5"
#define EMPTY_SHELF_FILTER "&collection_id=6"
#define SMART_SHELF_FILTER "&smart_collection_id=2"
#define SHELF_ID_BASE 200
static void serve_shelves(void) {
    char regular[1024];
    char smart[512];
    int used = collection_item(regular, sizeof regular, 1, "Favorites", 3, 0, 1);
    regular[used++] = ',';
    used += collection_item(regular + used, sizeof regular - (size_t)used, SHELF_ID, "Skiff Shelf",
                            SHELF_GAMES + 4, 0, 0);
    regular[used++] = ',';
    collection_item(regular + used, sizeof regular - (size_t)used, EMPTY_SHELF_ID, "Empty Shelf", 0,
                    0, 0);
    collection_item(smart, sizeof smart, SMART_SHELF_ID, "Smart Shelf", 3, 1, 0);
    serve_collections(regular, smart);
}

/* The shelf's first page of games (ids from SHELF_ID_BASE + 1). */
static void serve_shelf_page(unsigned offset) {
    const unsigned left = SHELF_GAMES - offset;
    serve_list_page(SHELF_FILTER, SHELF_ID_BASE, offset,
                    left < SKIFF_ROMM_PAGE_SIZE ? left : SKIFF_ROMM_PAGE_SIZE, SHELF_GAMES);
}

/* A library whose token lists collections, on the collections list. */
static void open_collections(void) {
    open_library_with_scopes(2, COLLECTIONS_SCOPES);
    serve_favourites(3);
    serve_shelves();
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
}

/* The URL path of request i, after the server. */
static const char *requested(size_t i) { return transport.log[i].url + strlen(SERVER); }

static void test_select_cycles_every_game_favourites_and_collections(void) {
    open_library_with_scopes(2, COLLECTIONS_SCOPES);
    serve_favourites(3);
    serve_shelves();
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_FAVOURITES), view()->title);
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_SELECT, english(SKIFF_TEXT_SHOW_COLLECTIONS)));
    const size_t requests = transport.request_count;
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_PRINTF("both of RomM's lists, regular then smart; the favourites collection left out");
    TEST_ASSERT_EQUAL_size_t(requests + 2, transport.request_count);
    TEST_ASSERT_EQUAL_STRING("/api/collections", requested(requests));
    TEST_ASSERT_EQUAL_STRING("/api/collections/smart", requested(requests + 1));
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_COLLECTIONS), view()->title);
    TEST_ASSERT_TRUE(shows("3 collections"));
    TEST_ASSERT_EQUAL_size_t(3, view()->row_count);
    TEST_ASSERT_EQUAL_STRING("Skiff Shelf", view()->rows[0].label);
    TEST_PRINTF("each with RomM's count, every platform's");
    TEST_ASSERT_EQUAL_STRING("34 games", view()->rows[0].detail);
    TEST_ASSERT_EQUAL_STRING("Empty Shelf", view()->rows[1].label);
    TEST_ASSERT_EQUAL_STRING("Smart Shelf", view()->rows[2].label);
    TEST_ASSERT_FALSE(shows("Favorites"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_SELECT)));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_SELECT, english(SKIFF_TEXT_SHOW_ALL_GAMES)));
    TEST_ASSERT_TRUE(view()->hint_count <= SKIFF_APP_HINTS_MAX);
    TEST_PRINTF("SELECT: every game again");
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_LIBRARY), view()->title);
    TEST_ASSERT_TRUE(shows("Game 1"));
    TEST_PRINTF("and the collections load again each time they are entered");
    serve_shelves();
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING("/api/collections/smart", requested(transport.request_count - 1));
    TEST_PRINTF("Back on the list goes on to every game, as SELECT does");
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_LIBRARY), view()->title);
}

static void test_a_collection_opens_its_psp_games_and_back_keeps_its_row(void) {
    open_collections();
    serve_shelf_page(0);
    serve_shelf_page(SKIFF_ROMM_PAGE_SIZE);
    TEST_PRINTF("open the shelf: its PSP games, under its name");
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING("Skiff Shelf", view()->title);
    TEST_ASSERT_TRUE(shows("30 games"));
    TEST_ASSERT_TRUE(shows("Game 201"));
    TEST_ASSERT_EQUAL_STRING("/api/roms?platform_ids=1&limit=25&offset=0" LIST_QUERY SHELF_FILTER,
                             requested(transport.request_count - 1));
    TEST_PRINTF("the same rows as the library: 213's name is unusable, so it is dimmed");
    int dimmed = 0;
    for (size_t i = 0; i < view()->row_count; i++) {
        dimmed += view()->rows[i].dim;
    }
    TEST_ASSERT_EQUAL_INT(1, dimmed);
    TEST_PRINTF("five hints at most: Back to the collections takes Settings' place");
    TEST_ASSERT_EQUAL_size_t(SKIFF_APP_HINTS_MAX, view()->hint_count);
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_START, english(SKIFF_TEXT_DOWNLOAD_ALL)));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_BACK, english(SKIFF_TEXT_BACK)));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_SELECT, english(SKIFF_TEXT_SHOW_ALL_GAMES)));
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_MENU, english(SKIFF_TEXT_SETTINGS)));
    TEST_PRINTF("pages of 25: the second loads as its rows come on screen");
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows("Game 230"));
    TEST_ASSERT_EQUAL_STRING("/api/roms?platform_ids=1&limit=25&offset=25" LIST_QUERY SHELF_FILTER,
                             requested(transport.request_count - 1));
    TEST_PRINTF("Back: the collections list, at the shelf's row, without asking RomM again");
    frame(SKIFF_UI_ACTION_DOWN);
    const size_t requests = transport.request_count;
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_COLLECTIONS), view()->title);
    TEST_ASSERT_EQUAL_size_t(0, view()->list.selected);
    TEST_ASSERT_EQUAL_size_t(requests, transport.request_count);
    TEST_PRINTF("the smart shelf opens with its own filter; an empty shelf says so");
    serve_list_page(SMART_SHELF_FILTER, SHELF_ID_BASE, 0, 3, 3);
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING("Smart Shelf", view()->title);
    TEST_ASSERT_TRUE(shows("3 games"));
    TEST_ASSERT_EQUAL_STRING(
        "/api/roms?platform_ids=1&limit=25&offset=0" LIST_QUERY SMART_SHELF_FILTER,
        requested(transport.request_count - 1));
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_size_t(2, view()->list.selected);
    serve_list_page(EMPTY_SHELF_FILTER, SHELF_ID_BASE, 0, 0, 0);
    frame(SKIFF_UI_ACTION_UP);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_COLLECTION_EMPTY)));
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_START, english(SKIFF_TEXT_DOWNLOAD_ALL)));
    TEST_PRINTF("SELECT from a collection's games goes on to every game");
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_LIBRARY), view()->title);
}

static void test_back_from_a_games_details_returns_to_its_collection(void) {
    open_collections();
    serve_shelf_page(0);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_DOWN);
    open_details(SHELF_ID_BASE + 2);
    TEST_ASSERT_TRUE(shows("Game 202.iso"));
    const size_t requests = transport.request_count;
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING("Skiff Shelf", view()->title);
    TEST_ASSERT_EQUAL_size_t(1, view()->list.selected);
    TEST_ASSERT_EQUAL_size_t(requests, transport.request_count);
}

static void test_without_collections_say_where_to_make_them(void) {
    open_library_with_scopes(2, COLLECTIONS_SCOPES);
    serve_favourites(0);
    char favourites[512];
    collection_item(favourites, sizeof favourites, 1, "Favorites", 3, 0, 1);
    serve_collections(favourites, "");
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_COLLECTIONS_EMPTY)));
    TEST_ASSERT_FALSE(view()->has_list);
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_SELECT)));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_COLLECTIONS_EMPTY)));
}

static void test_a_token_without_collections_read_offers_pair_again(void) {
    open_paired_library(2);
    serve_favourites(0);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    const size_t requests = transport.request_count;
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_PRINTF("[auth] scopes lacks collections.read: RomM is not even asked");
    TEST_ASSERT_EQUAL_size_t(requests, transport.request_count);
    TEST_ASSERT_TRUE(shows("Pair again to see your collections"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_PAIR_AGAIN)));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_SELECT, english(SKIFF_TEXT_SHOW_ALL_GAMES)));

    TEST_PRINTF("Pair again: the device flow; RomM grants collections.read this time");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_TOKEN, "romm/device-token.http"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_PAIR);
    serve_heartbeat("5.3.1");
    serve_platforms();
    serve_shelves();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    char config[TEXT_MAX];
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_ASSERT_NOT_NULL(strstr(config, "collections.read"));
    TEST_PRINTF("the address, the queue and the installs stay; the collections load");
    TEST_ASSERT_NOT_NULL(strstr(config, "url = " SERVER));
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_COLLECTIONS), view()->title);
    TEST_ASSERT_TRUE(shows("Skiff Shelf"));
}

static void test_a_forbidden_collections_list_offers_pair_again(void) {
    open_library_with_scopes(2, COLLECTIONS_SCOPES);
    serve_favourites(0);
    serve_raw("/api/collections", "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n"
                                  "\r\n{\"detail\":\"Forbidden\"}");
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_PRINTF("RomM refused the token: read as a missing scope, no error screen");
    TEST_ASSERT_TRUE(shows("Pair again to see your collections"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_PAIR_AGAIN)));
}

static void test_collections_too_large_for_the_psp_say_so_and_retry(void) {
    open_library_with_scopes(2, COLLECTIONS_SCOPES);
    serve_favourites(0);
    TEST_PRINTF("one collection more than a list keeps");
    static char many[RAW_MAX];
    int used = 0;
    for (unsigned i = 0; i <= SKIFF_ROMM_COLLECTIONS_MAX; i++) {
        if (i > 0) {
            many[used++] = ',';
        }
        used += collection_item(many + used, sizeof many - (size_t)used, i + 1, "Shelf", 1, 0, 0);
    }
    fake_route *large = NULL;
    {
        static char raw[RAW_MAX + 64];
        snprintf(raw, sizeof raw, JSON_OK "[%s]", many);
        large = serve_raw("/api/collections", raw);
        large->max_uses = 1;
    }
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows("[214]"));
    TEST_ASSERT_FALSE(view()->has_list);
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_RETRY)));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_SELECT, english(SKIFF_TEXT_SHOW_ALL_GAMES)));
    TEST_PRINTF("Retry asks again (the player made room in RomM meanwhile)");
    serve_shelves();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows("Skiff Shelf"));
    TEST_ASSERT_FALSE(shows("[214]"));
}

static void test_a_collections_list_that_lands_after_leaving_is_dropped(void) {
    open_library_with_scopes(2, COLLECTIONS_SCOPES);
    serve_favourites(0);
    serve_shelves();
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_SELECT);
    frame(0);
    TEST_ASSERT_EQUAL_INT(CALL_COLLECTIONS, (int)app->call.kind);
    TEST_PRINTF("SELECT on to every game while the list is on its way");
    frame(SKIFF_UI_ACTION_SELECT);
    env_state.hold_calls = 0;
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_LIBRARY), view()->title);
    TEST_ASSERT_FALSE(shows("Skiff Shelf"));
    TEST_ASSERT_FALSE(app->collections_known);
}

static void test_download_all_in_a_collection_names_it(void) {
    open_collections();
    serve_shelf_page(0);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    for (unsigned offset = 0; offset < SHELF_GAMES; offset += SKIFF_ROMM_PAGE_SIZE) {
        const unsigned left = SHELF_GAMES - offset;
        serve_list_page(SHELF_FILTER "&with_files=true", SHELF_ID_BASE, offset,
                        left < SKIFF_ROMM_PAGE_SIZE ? left : SKIFF_ROMM_PAGE_SIZE, SHELF_GAMES);
    }
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_START);
    frame(0);
    TEST_ASSERT_TRUE(shows("Checking the collection... 0/30"));
    env_state.hold_calls = 0;
    run_until(SKIFF_APP_SCREEN_CONFIRM);
    TEST_PRINTF("the same checks as the favourites: 213's name is unusable");
    TEST_ASSERT_TRUE(shows("Download 29 games from Skiff Shelf (84 KB)?"));
    TEST_ASSERT_TRUE(shows("1 can't be installed"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows("Added 29 downloads"));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(29, list_jobs(jobs));
    TEST_ASSERT_EQUAL_UINT64(SHELF_ID_BASE + 1, jobs[0].rom_id);
    TEST_PRINTF("still on the collection's games");
    TEST_ASSERT_EQUAL_STRING("Skiff Shelf", view()->title);
}

/* ---- Download all favourites ---- */

static void write_full_queue(void);

static int queue_file_opens;

static void count_queue_opens(void *ctx, const char *call, const char *path) {
    (void)ctx;
    queue_file_opens += strcmp(call, "open") == 0 && strstr(path, SKIFF_JOBS_FILE_NAME) != NULL;
}

/* The library on the favourites, total of them. */
static void open_favourites(unsigned total) {
    open_paired_library(2);
    serve_favourites(total);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
}

/* START, then frames until the batch asks (confirmation) or tells (notice). */
static void start_download_all(skiff_app_screen expected) {
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_START, english(SKIFF_TEXT_DOWNLOAD_ALL)));
    frame(SKIFF_UI_ACTION_START);
    run_until(expected);
}

static void test_download_all_queues_every_favourite_with_one_save(void) {
    open_paired_library(2);
    TEST_PRINTF("one game queued from its details: what one save of the queue opens");
    open_details(1);
    queue_file_opens = 0;
    storage.on_call = count_queue_opens;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    const int opens_per_save = queue_file_opens;
    TEST_PRINTF("one save opens queue.json %d time(s)", opens_per_save);
    TEST_ASSERT_GREATER_THAN_INT(0, opens_per_save);

    serve_favourites(30);
    serve_batch_favourites(30);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    start_download_all(SKIFF_APP_SCREEN_CONFIRM);
    TEST_ASSERT_TRUE(shows("Download 29 favourites (84 KB)?"));
    TEST_PRINTF("both pages read, with their files");
    queue_file_opens = 0;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    storage.on_call = NULL;
    TEST_ASSERT_TRUE(shows("Added 29 downloads"));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(30, list_jobs(jobs));
    TEST_PRINTF("29 games (113's name is unusable), one save: queue.json opened %d time(s)",
                queue_file_opens);
    TEST_ASSERT_EQUAL_INT(opens_per_save, queue_file_opens);
    TEST_ASSERT_EQUAL_UINT64(FAVOURITE_ID_BASE + 1, jobs[1].rom_id);
    TEST_ASSERT_EQUAL_STRING("Game 101", jobs[1].title);
    TEST_ASSERT_EQUAL_STRING("Game 101.iso", jobs[1].file_name);
    TEST_ASSERT_EQUAL_UINT64(BODY_BYTES, jobs[1].size);
    TEST_ASSERT_EQUAL_HEX32(body_crc, jobs[1].crc32);
    TEST_ASSERT_NOT_NULL(strstr(jobs[1].target, "ISO/Game 101.iso"));
    TEST_ASSERT_EQUAL_UINT64(FAVOURITE_ID_BASE + 30, jobs[29].rom_id);
    TEST_ASSERT_EQUAL_INT(1, env_state.worker_starts);
}

static void test_download_all_leaves_out_installed_queued_changed_and_refused_games(void) {
    open_paired_library(2);
    TEST_PRINTF("102 queued from its details; 101 installed; 103 changed in RomM; 113 unusable");
    serve_favourites(14);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_DOWN);
    open_details(FAVOURITE_ID_BASE + 2);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    const unsigned installed[] = {FAVOURITE_ID_BASE + 1, FAVOURITE_ID_BASE + 3};
    const uint32_t crcs[] = {body_crc, body_crc ^ 1U};
    record_installs(installed, crcs, 2);
    skiff_app_destroy(app);
    app = NULL;
    create_app();
    serve_favourites(14);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    serve_batch_favourites(14);
    start_download_all(SKIFF_APP_SCREEN_CONFIRM);
    TEST_ASSERT_TRUE(shows("Download 10 favourites"));
    TEST_ASSERT_TRUE(shows("1 already installed"));
    TEST_ASSERT_TRUE(shows("1 already in Downloads"));
    TEST_ASSERT_TRUE(shows("1 changed in RomM"));
    TEST_ASSERT_TRUE(shows("1 can't be installed"));
    TEST_PRINTF("Back: nothing queued");
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
    TEST_ASSERT_EQUAL_INT(BATCH_NONE, (int)app->batch.step);
}

static void test_download_all_with_a_full_queue_queues_nothing_and_says_why(void) {
    write_full_queue();
    open_favourites(3);
    serve_batch_favourites(3);
    start_download_all(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_BATCH_NOTHING)));
    TEST_ASSERT_TRUE(shows("3 don't fit in Downloads (64 at most)"));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(SKIFF_JOBS_MAX, list_jobs(jobs));
}

static void test_download_all_takes_what_fits_the_free_space_in_name_order(void) {
    open_favourites(5);
    serve_batch_favourites(5);
    TEST_PRINTF("room for two games past the 8 MiB margin");
    storage.free_bytes =
        SKIFF_STORAGE_FREE_MARGIN_BYTES + (uint64_t)2 * BODY_BYTES + BODY_BYTES / 2;
    start_download_all(SKIFF_APP_SCREEN_CONFIRM);
    TEST_ASSERT_TRUE(shows("Download 2 favourites"));
    TEST_ASSERT_TRUE(shows("3 don't fit on the Memory Stick"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(2, list_jobs(jobs));
    TEST_ASSERT_EQUAL_UINT64(FAVOURITE_ID_BASE + 1, jobs[0].rom_id);
    TEST_ASSERT_EQUAL_UINT64(FAVOURITE_ID_BASE + 2, jobs[1].rom_id);
}

static void test_a_download_that_ends_while_favourites_are_checked_frees_its_room(void) {
    open_paired_library(2);
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
    serve_favourites(3);
    serve_batch_favourites(3);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_PRINTF("room for three favourites, or two while Game 1's download is still to come");
    storage.free_bytes =
        SKIFF_STORAGE_FREE_MARGIN_BYTES + (uint64_t)3 * BODY_BYTES + BODY_BYTES / 2;
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_START);
    frame(0);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_cancel(app->jobs, jobs[0].id));
    env_state.hold_calls = 0;
    run_until(SKIFF_APP_SCREEN_CONFIRM);
    TEST_ASSERT_TRUE(shows("Download 3 favourites"));
}

static void test_download_all_keeps_room_for_installed_records(void) {
    open_favourites(3);
    serve_batch_favourites(3);
    TEST_PRINTF("installed.json one record short of full");
    app_lock_manifest(app);
    app->manifest->count = SKIFF_INSTALL_RECORDS_MAX - 1;
    app_unlock_manifest(app);
    start_download_all(SKIFF_APP_SCREEN_CONFIRM);
    TEST_ASSERT_TRUE(shows("Download 1 favourites"));
    TEST_ASSERT_TRUE(shows("2 don't fit: Skiff keeps track of 512 installed games at most"));
    app_lock_manifest(app);
    app->manifest->count = 0;
    app_unlock_manifest(app);
}

static void test_a_game_that_cannot_be_planned_is_named_apart_from_a_full_queue(void) {
    open_favourites(3);
    serve_batch_favourites(3);
    TEST_PRINTF("Game 101's name and its [101] fallback are both taken by hand-copied files");
    char iso[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO", iso, sizeof iso));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, iso));
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO/Game 101.iso", iso, sizeof iso));
    write_file(iso, "copy", 4);
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO/Game 101 [101].iso", iso, sizeof iso));
    write_file(iso, "copy", 4);
    start_download_all(SKIFF_APP_SCREEN_CONFIRM);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows("Added 2 of 3 downloads."));
    TEST_ASSERT_TRUE(shows("1 weren't added:"));
    TEST_ASSERT_FALSE(shows("Downloads is full"));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(2, list_jobs(jobs));
    TEST_ASSERT_EQUAL_UINT64(FAVOURITE_ID_BASE + 2, jobs[0].rom_id);
}

static void test_a_favourite_repeated_across_pages_is_taken_once(void) {
    open_favourites(26);
    TEST_PRINTF("the favourites changed between pages: page 1 starts with page 0's last game");
    serve_list_page(BATCH_FILTER, FAVOURITE_ID_BASE, 0, SKIFF_ROMM_PAGE_SIZE, 26);
    serve_list_page(BATCH_FILTER, FAVOURITE_ID_BASE - 1, SKIFF_ROMM_PAGE_SIZE, 1, 26);
    start_download_all(SKIFF_APP_SCREEN_CONFIRM);
    TEST_PRINTF("25 games: 113's name is unusable, 125 comes twice");
    TEST_ASSERT_TRUE(shows("Download 24 favourites"));
}

static void test_nothing_else_runs_while_download_all_queues(void) {
    open_favourites(3);
    serve_batch_favourites(3);
    start_download_all(SKIFF_APP_SCREEN_CONFIRM);
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_CONFIRM);
    frame(0);
    TEST_ASSERT_EQUAL_INT(BATCH_QUEUEING, (int)app->batch.step);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_BATCH_ADDING)));
    TEST_PRINTF("Downloads, Settings, SELECT and Back wait until the batch is queued");
    frame(SKIFF_UI_ACTION_EXTRA);
    frame(SKIFF_UI_ACTION_MENU);
    frame(SKIFF_UI_ACTION_SELECT);
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, (int)view()->screen);
    TEST_ASSERT_EQUAL_INT(SOURCE_FAVOURITES, (int)app->source);
    TEST_ASSERT_EQUAL_size_t(0, view()->hint_count);
    env_state.hold_calls = 0;
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows("Added 3 downloads"));
}

static void test_download_all_drops_a_library_page_still_loading(void) {
    open_paired_library(2);
    serve_favourites(30);
    serve_page(25, 5, 30);
    serve_batch_favourites(30);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_PRINTF("scroll to the favourites' second page, held on its way, then START");
    serve_raw("/api/roms?platform_ids=1&limit=25&offset=25" LIST_QUERY FAVOURITES_FILTER,
              "HTTP/1.1 500 Internal Server Error\r\n\r\n");
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(0);
    TEST_ASSERT_EQUAL_INT(CALL_PAGE, (int)app->call.kind);
    frame(SKIFF_UI_ACTION_START);
    TEST_ASSERT_EQUAL_INT(1, env_state.cancels);
    env_state.hold_calls = 0;
    run_until(SKIFF_APP_SCREEN_CONFIRM);
    TEST_PRINTF("the page's failure showed no error; the batch asks as usual");
    TEST_ASSERT_TRUE(shows("Download 29 favourites"));
}

static void test_an_empty_favourite_cannot_be_installed_and_spoils_nothing(void) {
    open_favourites(2);
    TEST_PRINTF("Game 101's only file is empty: the queue would refuse the whole batch");
    serve_raw("/api/roms?platform_ids=1&limit=25&offset=0" LIST_QUERY BATCH_FILTER,
              JSON_OK "{\"items\":[{\"id\":101,\"platform_id\":1,\"name\":\"Game 101\","
                      "\"fs_name\":\"Game 101.iso\",\"fs_size_bytes\":0,\"files\":[{\"rom_id\":"
                      "101,\"file_name\":\"Game 101.iso\",\"file_size_bytes\":0}]},"
                      "{\"id\":102,\"platform_id\":1,\"name\":\"Game 102\",\"fs_name\":"
                      "\"Game 102.iso\",\"fs_size_bytes\":3000,\"files\":[{\"rom_id\":102,"
                      "\"file_name\":\"Game 102.iso\",\"file_size_bytes\":3000}]}],"
                      "\"total\":2,\"limit\":25,\"offset\":0}");
    start_download_all(SKIFF_APP_SCREEN_CONFIRM);
    TEST_ASSERT_TRUE(shows("Download 1 favourites"));
    TEST_ASSERT_TRUE(shows("1 can't be installed"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows("Added 1 downloads"));
}

static void test_back_while_favourites_are_checked_stops_download_all(void) {
    open_favourites(3);
    serve_batch_favourites(3);
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_START);
    frame(0);
    TEST_ASSERT_TRUE(shows("Checking favourites... 0/3"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_BACK, english(SKIFF_TEXT_CANCEL)));
    frame(SKIFF_UI_ACTION_BACK);
    env_state.hold_calls = 0;
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_INT(BATCH_NONE, (int)app->batch.step);
    TEST_ASSERT_EQUAL_INT(1, env_state.cancels);
    TEST_ASSERT_FALSE(shows("Checking favourites"));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(0, list_jobs(jobs));
}

static void test_download_all_is_offered_only_on_favourites_with_games(void) {
    open_paired_library(2);
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_START, english(SKIFF_TEXT_DOWNLOAD_ALL)));
    frame(SKIFF_UI_ACTION_START);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_INT(BATCH_NONE, (int)app->batch.step);
    serve_favourites(0);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_START, english(SKIFF_TEXT_DOWNLOAD_ALL)));
    frame(SKIFF_UI_ACTION_START);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_INT(BATCH_NONE, (int)app->batch.step);
}

static void test_a_failed_favourites_page_ends_download_all_with_its_error(void) {
    open_favourites(3);
    serve_raw("/api/roms?platform_ids=1&limit=25&offset=0" LIST_QUERY BATCH_FILTER,
              "HTTP/1.1 500 Internal Server Error\r\n\r\n");
    frame(SKIFF_UI_ACTION_START);
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_EQUAL_INT(BATCH_NONE, (int)app->batch.step);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
}

/* ---- A batch of another list's own ---- */

/* A game as a list gives it with its file: "Game <id>", its file file_name of size bytes. */
static skiff_romm_rom_summary listed_game(unsigned id, const char *file_name, uint64_t size) {
    skiff_romm_rom_summary rom;
    memset(&rom, 0, sizeof rom);
    rom.id = id;
    rom.platform_id = 1;
    snprintf(rom.name, sizeof rom.name, "Game %u", id);
    snprintf(rom.fs_name, sizeof rom.fs_name, "%s", file_name);
    rom.size = size;
    rom.has_crc32 = 1;
    rom.crc32 = body_crc;
    rom.has_file = 1;
    snprintf(rom.file.file_name, sizeof rom.file.file_name, "%s", file_name);
    rom.file.size = size;
    rom.file.has_crc32 = 1;
    rom.file.crc32 = body_crc;
    return rom;
}

/* "Game <id>.iso" recorded as installed in the running app's installed.json, with crc. */
static void record_in_app(unsigned id, uint32_t crc) {
    skiff_install_record record;
    memset(&record, 0, sizeof record);
    record.rom_id = id;
    snprintf(record.file_name, sizeof record.file_name, "Game %u.iso", id);
    snprintf(record.path, sizeof record.path, "games:/Game %u.iso", id);
    record.size = BODY_BYTES;
    record.has_crc32 = 1;
    record.crc32 = crc;
    app_lock_manifest(app);
    const skiff_err err = skiff_install_manifest_record(app->manifest, &record);
    app_unlock_manifest(app);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, err);
}

static void assert_job_target(const skiff_job *job, uint64_t rom_id, const char *logical) {
    char target[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, logical, target, sizeof target));
    TEST_PRINTF("job for %llu: %s", (unsigned long long)job->rom_id, job->target);
    TEST_ASSERT_EQUAL_UINT64(rom_id, job->rom_id);
    TEST_ASSERT_EQUAL_STRING(target, job->target);
}

static void test_a_batch_of_its_own_sorts_and_queues_apart_from_download_all(void) {
    open_paired_library(2);
    TEST_PRINTF("Game 1 queued from its details; Game 2 installed; Game 3 changed in RomM");
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    record_in_app(2, body_crc);
    record_in_app(3, body_crc ^ 1U);

    app_batch *own = calloc(1, sizeof *own);
    TEST_ASSERT_NOT_NULL(own);
    app_batch_reset(app, own);
    TEST_PRINTF("room: %zu downloads, %zu records", own->queue_room, own->record_room);
    TEST_ASSERT_EQUAL_size_t(SKIFF_JOBS_MAX - 1, own->queue_room);
    TEST_ASSERT_EQUAL_size_t(SKIFF_INSTALL_RECORDS_MAX - 3, own->record_room);

    TEST_PRINTF("Download all's rules, game by game, in the order given");
    const skiff_romm_rom_summary games[] = {
        listed_game(1, "Game 1.iso", BODY_BYTES), listed_game(2, "Game 2.iso", BODY_BYTES),
        listed_game(3, "Game 3.iso", BODY_BYTES), listed_game(4, "Game 4.iso", 0),
        listed_game(5, "Game 5.iso", BODY_BYTES), listed_game(5, "Game 5.iso", BODY_BYTES),
        listed_game(6, "Game 5.iso", BODY_BYTES), listed_game(7, "Game 1.iso", BODY_BYTES),
        listed_game(8, "Game 8.iso", BODY_BYTES),
    };
    const app_batch_outcome expected[] = {
        OUTCOME_QUEUED,   OUTCOME_INSTALLED, OUTCOME_CHANGED, OUTCOME_REFUSED, OUTCOME_TAKEN,
        OUTCOME_REPEATED, OUTCOME_TAKEN,     OUTCOME_TAKEN,   OUTCOME_TAKEN,
    };
    TEST_ASSERT_EQUAL_size_t(sizeof games / sizeof games[0], sizeof expected / sizeof expected[0]);
    for (size_t i = 0; i < sizeof games / sizeof games[0]; i++) {
        const app_batch_outcome outcome = app_batch_take(app, own, &games[i]);
        TEST_PRINTF("game %llu (%s): outcome %d", (unsigned long long)games[i].id,
                    games[i].file.file_name, (int)outcome);
        TEST_ASSERT_EQUAL_INT(expected[i], outcome);
    }
    TEST_PRINTF("no room left in the queue, then in installed.json");
    const skiff_romm_rom_summary late = listed_game(9, "Game 9.iso", BODY_BYTES);
    own->queue_room = own->count;
    TEST_ASSERT_EQUAL_INT(OUTCOME_OVER_QUEUE, app_batch_take(app, own, &late));
    own->queue_room = SKIFF_JOBS_MAX;
    own->record_room = own->count;
    TEST_ASSERT_EQUAL_INT(OUTCOME_OVER_RECORDS, app_batch_take(app, own, &late));
    TEST_ASSERT_EQUAL_size_t(4, own->count);
    TEST_ASSERT_EQUAL_size_t(1, own->queued);
    TEST_ASSERT_EQUAL_size_t(1, own->installed);
    TEST_ASSERT_EQUAL_size_t(1, own->changed);
    TEST_ASSERT_EQUAL_size_t(1, own->refused);
    TEST_ASSERT_EQUAL_size_t(1, own->over_queue);
    TEST_ASSERT_EQUAL_size_t(1, own->over_records);

    TEST_PRINTF("room for three past the margin and Game 1's download still to come");
    app_batch_fit(app, own, SKIFF_OK,
                  SKIFF_STORAGE_FREE_MARGIN_BYTES + (uint64_t)4 * BODY_BYTES + BODY_BYTES / 2);
    TEST_ASSERT_EQUAL_UINT64(BODY_BYTES, own->pending_bytes);
    TEST_ASSERT_EQUAL_size_t(3, own->count);
    TEST_ASSERT_EQUAL_size_t(1, own->over_space);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)3 * BODY_BYTES, own->bytes);

    TEST_PRINTF("queued with targets kept apart from Game 1's and from each other");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, app_batch_queue_run(app, own));
    TEST_ASSERT_EQUAL_size_t(3, own->request_count);
    TEST_ASSERT_EQUAL_size_t(3, own->added);
    TEST_ASSERT_EQUAL_size_t(0, own->plan_failures);
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(4, list_jobs(jobs));
    assert_job_target(&jobs[0], 1, "ISO/Game 1.iso");
    assert_job_target(&jobs[1], 5, "ISO/Game 5.iso");
    assert_job_target(&jobs[2], 6, "ISO/Game 5 [6].iso");
    assert_job_target(&jobs[3], 7, "ISO/Game 1 [7].iso");
    free(own);

    TEST_PRINTF("Download all's own batch was never touched");
    TEST_ASSERT_EQUAL_INT(BATCH_NONE, (int)app->batch.step);
    TEST_ASSERT_EQUAL_size_t(0, app->batch.count);
    TEST_ASSERT_EQUAL_size_t(0, app->batch.request_count);
}

/* ---- Downloading ---- */

static void test_a_download_is_queued_into_the_iso_folder(void) {
    open_paired_library(2);
    open_details(1);
    TEST_ASSERT_TRUE(shows("Game 1.iso"));
    TEST_ASSERT_TRUE(shows("Free space: 1.5 GB"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_DOWNLOAD)));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows("Added to downloads: Game 1"));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
    char target[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO/Game 1.iso", target, sizeof target));
    TEST_ASSERT_EQUAL_STRING(target, jobs[0].target);
    TEST_ASSERT_EQUAL_STRING("Game 1.iso", jobs[0].file_name);
    TEST_ASSERT_EQUAL_HEX32(body_crc, jobs[0].crc32);
    TEST_ASSERT_FALSE(jobs[0].replace_target);
    TEST_ASSERT_EQUAL_INT(1, storage.mkdirs > 0);
    TEST_PRINTF("the row shows the download; Download on it again opens the queue");
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_QUEUE_WAITING), view()->rows[0].detail);
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_QUEUE, view()->screen);
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
}

static void test_a_finished_download_shows_as_installed(void) {
    open_paired_library(1);
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    serve_content(1);
    TEST_PRINTF("the worker downloads the job with the spec it was started with");
    run_worker_once();
    char manifest[RAW_MAX];
    read_app_file(SKIFF_INSTALL_MANIFEST_NAME, manifest, sizeof manifest);
    TEST_PRINTF("installed.json: %s", manifest);
    TEST_ASSERT_NOT_NULL(strstr(manifest, "\"path\":\"games:/Game 1.iso\""));
    TEST_PRINTF("the install time comes from the real-time clock");
    TEST_ASSERT_NOT_NULL(strstr(manifest, "\"installed_ms\":179000"));
    frame(0);
    print_view();
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_INSTALLED), view()->rows[0].detail);
    TEST_PRINTF("downloading it again asks first, and the job may replace Skiff's copy");
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_CONFIRM, view()->screen);
    TEST_ASSERT_TRUE(shows("Replace your installed copy?"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    skiff_job jobs[SKIFF_JOBS_MAX];
    const size_t count = list_jobs(jobs);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, jobs[count - 1].state);
    TEST_ASSERT_TRUE(jobs[count - 1].replace_target);
    TEST_ASSERT_EQUAL_UINT64(BODY_BYTES, jobs[count - 1].replace_size);
}

static void test_a_changed_game_and_a_hand_copy(void) {
    TEST_PRINTF("Skiff's copy of ROM 1 has another CRC-32 than RomM lists now");
    record_installed(1, body_crc ^ 1U);
    TEST_PRINTF("a game copied by hand under ROM 2's name");
    char iso[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO/Game 2.iso", iso, sizeof iso));
    write_file(iso, "hand", 4);
    open_paired_library(2);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_INSTALL_CHANGED), view()->rows[0].detail);
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_TRUE(shows("RomM has a different version"));
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_DETAILS, view()->screen);
    frame(SKIFF_UI_ACTION_BACK);
    frame(SKIFF_UI_ACTION_DOWN);
    open_details(2);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
    TEST_PRINTF("the hand copy keeps its name: %s", jobs[0].target);
    TEST_ASSERT_NOT_NULL(strstr(jobs[0].target, "Game 2 [2].iso"));
}

/* Clear of every ROM id the library and the favourites use. */
#define QUEUED_ID_BASE 1000
static void write_full_queue(void) {
    static skiff_job jobs[SKIFF_JOBS_MAX];
    memset(jobs, 0, sizeof jobs);
    for (unsigned i = 0; i < SKIFF_JOBS_MAX; i++) {
        jobs[i].id = i + 1;
        jobs[i].rom_id = QUEUED_ID_BASE + i;
        jobs[i].size = 1;
        jobs[i].state = SKIFF_JOB_QUEUED;
        snprintf(jobs[i].title, sizeof jobs[i].title, "Queued %u", i);
        snprintf(jobs[i].file_name, sizeof jobs[i].file_name, "q%u.iso", i);
        char name[32];
        snprintf(name, sizeof name, "ISO/q%u.iso", i);
        TEST_ASSERT_TRUE(temp_dir_path(dir, name, jobs[i].target, sizeof jobs[i].target));
    }
    static char text[SKIFF_JOBS_FILE_MAX];
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_format(jobs, SKIFF_JOBS_MAX, SKIFF_JOBS_MAX + 1,
                                                      text, sizeof text, &length));
    char path[TEMP_DIR_PATH_MAX];
    app_file(SKIFF_JOBS_FILE_NAME, path, sizeof path);
    write_file(path, text, length);
}

static void test_a_full_queue_shows_its_hint(void) {
    write_full_queue();
    open_paired_library(1);
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    print_view();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_MESSAGE, view()->screen);
    TEST_ASSERT_TRUE(shows("The download list is full."));
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_DETAILS, view()->screen);
}

static void test_a_full_installed_list_shows_its_hint(void) {
    open_paired_library(1);
    app->manifest->count = SKIFF_INSTALL_RECORDS_MAX;
    for (size_t i = 0; i < SKIFF_INSTALL_RECORDS_MAX; i++) {
        app->manifest->records[i].rom_id = 1000 + i;
    }
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    print_view();
    TEST_ASSERT_TRUE(shows("Skiff can't keep track of more installed games."));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(0, list_jobs(jobs));
}

/* ---- The downloads screen ---- */

static void test_the_downloads_screen_follows_the_worker(void) {
    open_paired_library(2);
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_EXTRA);
    run_until(SKIFF_APP_SCREEN_QUEUE);
    skiff_job jobs[SKIFF_JOBS_MAX];
    list_jobs(jobs);
    TEST_PRINTF("progress and the speed over 2 s, as the worker reports them");
    skiff_jobs_event event = {
        .kind = SKIFF_JOBS_EVENT_STATE, .job_id = jobs[0].id, .state = SKIFF_JOB_ACTIVE};
    app->queue.jobs[0].state = SKIFF_JOB_ACTIVE;
    event.kind = SKIFF_JOBS_EVENT_PROGRESS;
    event.done = 0;
    event.total = (uint64_t)100 * 1024 * 1024;
    app_queue_event(app, &event);
    env_state.now_ms += 2000;
    event.done = (uint64_t)50 * 1024 * 1024;
    app_queue_event(app, &event);
    app->dirty = 1;
    frame(0);
    print_view();
    TEST_ASSERT_TRUE(shows("50.0 MB of 100 MB (50%)"));
    TEST_ASSERT_TRUE(shows("25.0 MB/s, about 2 s left"));
    TEST_ASSERT_TRUE(view()->has_progress);
    TEST_ASSERT_EQUAL_UINT(50, view()->percent);
    event.kind = SKIFF_JOBS_EVENT_RECOVERY;
    event.step = SKIFF_JOBS_RETRYING;
    event.retry_in_ms = 4000;
    app_queue_event(app, &event);
    frame(0);
    TEST_ASSERT_TRUE(shows("Trying again in 4 s"));
    TEST_PRINTF("progress, speed and recovery: %zu lines under the list (room for %d)",
                view()->line_count, APP_QUEUE_DETAIL_LINES);
    TEST_ASSERT_TRUE(view()->lines_below);
    TEST_ASSERT_EQUAL_size_t(APP_QUEUE_DETAIL_LINES, view()->line_count);
    event.step = SKIFF_JOBS_WAITING_FOR_WIFI;
    app_queue_event(app, &event);
    frame(0);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_QUEUE_WAITING_WIFI)));
}

static void test_the_downloads_list_stays_put_as_the_details_come_and_go(void) {
    open_paired_library(2);
    for (unsigned id = 1; id <= 2; id++) {
        if (id > 1) {
            frame(SKIFF_UI_ACTION_DOWN);
        }
        open_details(id);
        frame(SKIFF_UI_ACTION_CONFIRM);
        run_until(SKIFF_APP_SCREEN_LIBRARY);
    }
    frame(SKIFF_UI_ACTION_EXTRA);
    run_until(SKIFF_APP_SCREEN_QUEUE);
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(2, list_jobs(jobs));
    app->queue.jobs[0].state = SKIFF_JOB_ACTIVE;
    skiff_jobs_event event = {.kind = SKIFF_JOBS_EVENT_PROGRESS,
                              .job_id = jobs[0].id,
                              .done = (uint64_t)1024 * 1024,
                              .total = (uint64_t)4 * 1024 * 1024};
    app_queue_event(app, &event);
    app->dirty = 1;
    frame(0);
    print_view();
    TEST_ASSERT_TRUE(view()->has_progress);
    TEST_ASSERT_TRUE(view()->line_count > 0);
    const size_t rows = view()->list.rows;
    TEST_PRINTF("the active download's details under %zu rows", rows);
    TEST_ASSERT_TRUE(view()->lines_below);
    TEST_ASSERT_EQUAL_size_t(SKIFF_APP_ROWS_FIT(APP_QUEUE_DETAIL_LINES, 1), rows);
    frame(SKIFF_UI_ACTION_DOWN);
    print_view();
    TEST_PRINTF("a waiting download has no details: the list keeps its rows and its place");
    TEST_ASSERT_FALSE(view()->has_progress);
    TEST_ASSERT_EQUAL_size_t(0, view()->line_count);
    TEST_ASSERT_TRUE(view()->lines_below);
    TEST_ASSERT_EQUAL_size_t(rows, view()->list.rows);
    TEST_ASSERT_EQUAL_size_t(1, view()->list.selected);
}

static void test_every_error_fits_under_the_downloads_list(void) {
    static const skiff_err errors[] = {
#define TEST_ERROR_CODE(name, value, message) name,
        SKIFF_ERROR_TABLE(TEST_ERROR_CODE)
#undef TEST_ERROR_CODE
    };
    char lines[SKIFF_APP_LINES_MAX][SKIFF_TEXT_MAX];
    size_t longest = 0;
    for (int language = 0; language < SKIFF_LANGUAGE_COUNT; language++) {
        for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++) {
            char text[SKIFF_TEXT_MAX];
            TEST_ASSERT_EQUAL_INT(
                SKIFF_OK, skiff_error_line((skiff_language)language, errors[i], text, sizeof text));
            const size_t count = skiff_app_wrap(text, SKIFF_APP_TEXT_WIDTH, env_measure, NULL,
                                                lines, SKIFF_APP_LINES_MAX);
            longest = count > longest ? count : longest;
            TEST_ASSERT_TRUE_MESSAGE(count <= APP_QUEUE_DETAIL_LINES, text);
        }
    }
    TEST_PRINTF("the longest error takes %zu of the %d lines under the list", longest,
                APP_QUEUE_DETAIL_LINES);
}

static void test_cancel_retry_and_clear_from_the_downloads_screen(void) {
    open_paired_library(2);
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_EXTRA);
    run_until(SKIFF_APP_SCREEN_QUEUE);
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_EXTRA, english(SKIFF_TEXT_CANCEL_DOWNLOAD)));
    frame(SKIFF_UI_ACTION_EXTRA);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_CONFIRM, view()->screen);
    TEST_PRINTF("back keeps the download");
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_QUEUE, view()->screen);
    frame(SKIFF_UI_ACTION_EXTRA);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_QUEUE);
    TEST_ASSERT_TRUE(shows("Cancelled"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_RETRY)));
    frame(SKIFF_UI_ACTION_CONFIRM);
    frame(0);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_QUEUE_WAITING)));
    TEST_PRINTF("a failed job shows its sentence and code");
    serve_content(1);
    fake_route *content = &transport.routes[transport.route_count - 1];
    content->fail_before_response = SKIFF_ERR_STORAGE_NAME_TAKEN;
    run_worker_once();
    frame(0);
    print_view();
    TEST_ASSERT_TRUE(shows("[305]"));
    TEST_ASSERT_TRUE(shows("Failed: 305"));
    frame(SKIFF_UI_ACTION_EXTRA);
    frame(SKIFF_UI_ACTION_CONFIRM);
    frame(SKIFF_UI_ACTION_MENU);
    frame(0);
    print_view();
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_QUEUE_EMPTY)));
}

static void test_downloading_lasts_while_the_worker_has_a_download_to_run(void) {
    TEST_ASSERT_EQUAL_INT(0, skiff_app_downloading(NULL));
    open_paired_library(2);
    TEST_PRINTF("the worker runs with nothing queued: not downloading");
    TEST_ASSERT_EQUAL_INT(0, skiff_app_downloading(app));
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_PRINTF("a download queued: downloading");
    TEST_ASSERT_EQUAL_INT(1, skiff_app_downloading(app));
    TEST_PRINTF("the worker stopped (a new server, a quit) with it still queued: not downloading");
    TEST_ASSERT_TRUE(app_stop_worker(app));
    TEST_ASSERT_EQUAL_INT(0, skiff_app_downloading(app));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, app_start_worker(app));
    TEST_ASSERT_EQUAL_INT(1, skiff_app_downloading(app));
    TEST_PRINTF("active, as the worker reports it: downloading");
    app->queue.jobs[0].state = SKIFF_JOB_ACTIVE;
    TEST_ASSERT_EQUAL_INT(1, skiff_app_downloading(app));
    TEST_PRINTF("it failed and waits for the player: not downloading");
    serve_content(1);
    transport.routes[transport.route_count - 1].fail_before_response = SKIFF_ERR_STORAGE_NAME_TAKEN;
    run_worker_once();
    frame(0);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_FAILED, app->queue.jobs[0].state);
    TEST_ASSERT_EQUAL_INT(0, skiff_app_downloading(app));
    TEST_PRINTF("another one queued, then done: downloading, then not");
    frame(SKIFF_UI_ACTION_DOWN);
    open_details(2);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_INT(1, skiff_app_downloading(app));
    serve_content(2);
    run_worker_once();
    frame(0);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, app->queue.jobs[1].state);
    TEST_ASSERT_EQUAL_INT(0, skiff_app_downloading(app));
}

/* ---- Settings ---- */

static void test_saves_is_set_apart_from_the_server_rows_in_settings(void) {
    open_paired_library(1);
    TEST_PRINTF("the library's rows never start a new group");
    for (size_t i = 0; i < view()->row_count; i++) {
        TEST_ASSERT_FALSE(view()->rows[i].new_group);
    }
    serve_heartbeat("5.3.1");
    frame(SKIFF_UI_ACTION_MENU);
    run_until(SKIFF_APP_SCREEN_SETTINGS);
    TEST_ASSERT_EQUAL_size_t(APP_SETTINGS_ROW_COUNT, view()->row_count);
    TEST_ASSERT_TRUE(view()->list.count <= view()->list.rows);
    TEST_ASSERT_TRUE(shows("Server: " SERVER));
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_PAIR_AGAIN), view()->rows[1].label);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_SAVES), view()->rows[2].label);
    TEST_PRINTF("the server and Pair again form one group, Saves starts the next");
    TEST_ASSERT_FALSE(view()->rows[0].new_group);
    TEST_ASSERT_FALSE(view()->rows[1].new_group);
    TEST_ASSERT_TRUE(view()->rows[2].new_group);
    TEST_PRINTF("moving the selection keeps the group where it is");
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_DOWN);
    TEST_ASSERT_EQUAL_size_t(2, view()->list.selected);
    TEST_ASSERT_FALSE(view()->rows[1].new_group);
    TEST_ASSERT_TRUE(view()->rows[2].new_group);
    TEST_PRINTF("every other screen is checked on every frame (check_new_groups)");
}

static void test_a_new_server_stops_the_worker_and_asks_to_pair(void) {
    open_paired_library(1);
    serve_heartbeat("5.3.1");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    frame(SKIFF_UI_ACTION_MENU);
    run_until(SKIFF_APP_SCREEN_SETTINGS);
    TEST_ASSERT_TRUE(shows("Server: " SERVER));
    TEST_ASSERT_TRUE(shows("RomM 5.3.1"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_DIALOG_KEYBOARD, view()->dialog);
    TEST_ASSERT_EQUAL_STRING(SERVER, view()->dialog_text);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, "https://other.test");
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_ASSERT_EQUAL_INT(1, env_state.worker_stops);
    char config[TEXT_MAX];
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_PRINTF("config.ini:\n%s", config);
    TEST_ASSERT_NOT_NULL(strstr(config, "url = https://other.test"));
    TEST_ASSERT_NULL(strstr(config, TOKEN));
    TEST_ASSERT_NULL_MESSAGE(strstr(config, "assets.read"), "the old token's scopes go with it");
    TEST_ASSERT_NOT_NULL(strstr(config, "device_identifier = 00112233445566778899aabbccddeeff"));
    TEST_ASSERT_TRUE(shows("https://other.test/pair/device"));
    TEST_ASSERT_TRUE(shows("?user_code=" USER_CODE));
}

static void test_a_worker_that_will_not_stop_changes_nothing(void) {
    open_paired_library(1);
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    env_state.stop_error = SKIFF_ERR_NET_TIMEOUT;
    frame(SKIFF_UI_ACTION_MENU);
    frame(SKIFF_UI_ACTION_CONFIRM);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, "https://other.test");
    frame(0);
    frame(SKIFF_UI_ACTION_CONFIRM);
    frame(0);
    print_view();
    TEST_ASSERT_TRUE(shows("could not stop the downloads in time"));
    TEST_PRINTF("the address, the token and the download all stay");
    char config[TEXT_MAX];
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_ASSERT_NOT_NULL(strstr(config, "url = " SERVER));
    TEST_ASSERT_NOT_NULL(strstr(config, "token = " TOKEN));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
    TEST_ASSERT_EQUAL_STRING("Bearer " TOKEN, env_state.spec.romm->authorization);
}

static void test_an_address_without_a_scheme_is_refused(void) {
    create_app();
    run_until(SKIFF_APP_SCREEN_SERVER);
    frame(SKIFF_UI_ACTION_CONFIRM);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, "romm.test");
    frame(0);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_MESSAGE, view()->screen);
    TEST_ASSERT_TRUE(shows("[402]"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SERVER, view()->screen);
}

static void test_a_new_server_cancels_the_old_downloads(void) {
    open_paired_library(1);
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    serve_heartbeat("5.3.1");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    frame(SKIFF_UI_ACTION_MENU);
    frame(SKIFF_UI_ACTION_CONFIRM);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, "https://other.test");
    frame(0);
    print_view();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_CONFIRM, view()->screen);
    TEST_ASSERT_TRUE(shows("A new server cancels the current downloads"));
    TEST_PRINTF("back keeps the server and the download");
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SETTINGS, view()->screen);
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
    TEST_ASSERT_EQUAL_INT(0, env_state.worker_stops);
    frame(SKIFF_UI_ACTION_CONFIRM);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, "https://other.test");
    frame(0);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_ASSERT_EQUAL_INT(1, env_state.worker_stops);
    TEST_PRINTF("the old server's job is gone, so it never runs against the new one");
    TEST_ASSERT_EQUAL_size_t(0, list_jobs(jobs));
}

static void test_a_failed_page_can_be_retried(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    serve_platforms();
    char path[256];
    snprintf(path, sizeof path, "/api/roms?platform_ids=1&limit=%d&offset=0" LIST_QUERY,
             SKIFF_ROMM_PAGE_SIZE);
    fake_route *broken = serve_raw(path, "HTTP/1.1 500 Internal Server Error\r\n\r\n");
    broken->max_uses = 1;
    serve_page(0, 2, 2);
    create_app();
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows("[203]"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_RETRY)));
    const int joins = env_state.net_starts;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_size_t(2, view()->row_count);
    TEST_ASSERT_TRUE(view()->line_count <= APP_SETTINGS_LINES);
    TEST_PRINTF("a RomM error needs no new join");
    TEST_ASSERT_EQUAL_INT(joins, env_state.net_starts);
}

/* ---- Requests off the screen's thread ---- */

static void test_the_library_answers_while_a_page_loads(void) {
    open_paired_library(30);
    serve_page(25, 5, 30);
    env_state.hold_calls = 1;
    const int calls = env_state.calls_started;
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(0);
    TEST_ASSERT_TRUE(app_call_busy(app));
    TEST_PRINTF("the second page's request is held: the screen still moves");
    const size_t selected = view()->list.selected;
    frame(SKIFF_UI_ACTION_UP);
    TEST_ASSERT_EQUAL_size_t(selected - 1, view()->list.selected);
    TEST_ASSERT_EQUAL_INT(calls + 1, env_state.calls_started);
    TEST_PRINTF("Settings opens, but changing the server waits for the request");
    frame(SKIFF_UI_ACTION_MENU);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SETTINGS, view()->screen);
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_DIALOG_NONE, view()->dialog);
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    env_state.hold_calls = 0;
    finish_call();
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_TRUE(shows("Game 30"));
    TEST_ASSERT_EQUAL_INT(0, env_state.cancels);
}

static void test_leaving_a_game_while_it_loads_drops_its_details(void) {
    open_paired_library(2);
    serve_rom(1);
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_CONFIRM);
    frame(0);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_DETAILS, view()->screen);
    TEST_ASSERT_TRUE(app_call_busy(app));
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_LIBRARY_LOADING)));
    TEST_PRINTF("Back while the game loads: its request is told to stop, the library is back");
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(1, env_state.cancels);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    env_state.hold_calls = 0;
    finish_call();
    TEST_PRINTF("the answer that came anyway is dropped");
    TEST_ASSERT_FALSE(app->has_rom);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    frame(SKIFF_UI_ACTION_DOWN);
    open_details(2);
    TEST_ASSERT_TRUE(shows("Game 2.iso"));
}

/* ---- Covers ---- */

static int cover_slot_exists(unsigned id) {
    char path[TEMP_DIR_PATH_MAX];
    char name[32];
    snprintf(name, sizeof name, "covers/%u.cov", id % SKIFF_COVER_CACHE_SLOTS);
    app_file(name, path, sizeof path);
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        fclose(file);
    }
    return file != NULL;
}

/* Every body line fits beside the cover box. */
static void assert_lines_beside_the_cover(void) {
    for (size_t i = 0; i < view()->line_count; i++) {
        TEST_ASSERT_TRUE(env_measure(NULL, view()->lines[i]) <= SKIFF_APP_COVER_TEXT_WIDTH);
    }
}

static void test_a_game_shows_its_cover_and_keeps_it_on_the_memory_stick(void) {
    open_paired_library(2);
    fake_route *details = serve_rom_with_cover(1, COVER_PATH(1));
    fake_route *cover = serve_cover(COVER_URL(1), "HTTP/1.1 200 OK", COVER_PNG, sizeof COVER_PNG);
    const int calls = env_state.calls_started;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    TEST_PRINTF("details, then the cover: %d calls, cover asked %d time(s)",
                env_state.calls_started - calls, cover->uses);
    TEST_ASSERT_EQUAL_INT(2, env_state.calls_started - calls);
    TEST_ASSERT_EQUAL_INT(1, cover->uses);
    TEST_ASSERT_TRUE(view()->has_cover_box);
    TEST_ASSERT_NOT_NULL(view()->cover);
    TEST_ASSERT_EQUAL_UINT16(2, view()->cover->width);
    TEST_ASSERT_EQUAL_UINT16(2, view()->cover->height);
    TEST_ASSERT_EQUAL_HEX16(skiff_cover_rgb565(0xFF, 0, 0), view()->cover->pixels[0]);
    TEST_ASSERT_TRUE(cover_slot_exists(1));
    assert_lines_beside_the_cover();
    TEST_ASSERT_TRUE(shows("Game 1.iso"));
    TEST_PRINTF("the second visit reads it from the Memory Stick, not RomM");
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_NULL(view()->cover);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    TEST_ASSERT_EQUAL_INT(1, cover->uses);
    TEST_ASSERT_NOT_NULL(view()->cover);
    TEST_ASSERT_EQUAL_HEX16(skiff_cover_rgb565(0, 0xFF, 0), view()->cover->pixels[1]);
    TEST_PRINTF("a cover changed in RomM (new ts) is fetched again, not the one in memory reused");
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    /* Later routes for a path answer once the earlier one is used up. */
    details->max_uses = details->uses;
    details = serve_rom_with_cover(1, NEWER_COVER_PATH);
    fake_route *newer =
        serve_cover(NEWER_COVER_URL, "HTTP/1.1 200 OK", COVER_PNG, sizeof COVER_PNG);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    TEST_ASSERT_EQUAL_INT(1, newer->uses);
    TEST_ASSERT_NOT_NULL(view()->cover);
    TEST_PRINTF("a cover removed in RomM is not shown");
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    details->max_uses = details->uses;
    serve_rom(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    TEST_ASSERT_NULL(view()->cover);
    TEST_PRINTF("a new server forgets the cover in memory: its ROM 1 is another game");
    TEST_ASSERT_TRUE(app->has_cover);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, app_reset_server(app));
    TEST_ASSERT_FALSE(app->has_cover);
}

static void test_a_game_without_a_usable_cover_keeps_its_placeholder(void) {
    open_paired_library(4);
    TEST_PRINTF("no cover: no request for one, the box stays empty");
    const int calls = env_state.calls_started;
    open_details(1);
    TEST_ASSERT_EQUAL_INT(1, env_state.calls_started - calls);
    TEST_ASSERT_TRUE(view()->has_cover_box);
    TEST_ASSERT_NULL(view()->cover);
    assert_lines_beside_the_cover();
    const struct {
        unsigned id;
        const char *status;
        const unsigned char *body;
        size_t size;
    } CASES[] = {
        {2, "HTTP/1.1 404 Not Found", (const unsigned char *)"<html>", 6},
        {3, "HTTP/1.1 200 OK", (const unsigned char *)"\xFF\xD8\xFF\xE0 JPEG", 9},
        {4, "HTTP/1.1 200 OK", COVER_PNG, sizeof COVER_PNG - 20},
    };
    static const char *const PATHS[] = {COVER_PATH(2), COVER_PATH(3), COVER_PATH(4)};
    static const char *const URLS[] = {COVER_URL(2), COVER_URL(3), COVER_URL(4)};
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        frame(SKIFF_UI_ACTION_BACK);
        run_until(SKIFF_APP_SCREEN_LIBRARY);
        frame(SKIFF_UI_ACTION_DOWN);
        serve_rom_with_cover(CASES[i].id, PATHS[i]);
        serve_cover(URLS[i], CASES[i].status, CASES[i].body, CASES[i].size);
        frame(SKIFF_UI_ACTION_CONFIRM);
        run_until(SKIFF_APP_SCREEN_DETAILS);
        TEST_PRINTF("game %u (%s): details shown, no cover, nothing cached", CASES[i].id,
                    CASES[i].status);
        TEST_ASSERT_TRUE(shows("Game"));
        TEST_ASSERT_NULL(view()->cover);
        TEST_ASSERT_FALSE(cover_slot_exists(CASES[i].id));
        TEST_ASSERT_TRUE(app->net_joined);
    }
}

static void test_long_names_beside_the_cover_leave_room_for_the_details(void) {
    open_paired_library(1);
    /* A title and a file name of 250 bytes: each would wrap to 6 lines beside the cover. */
    char title[251];
    char file_name[251];
    memset(title, 'T', sizeof title - 1);
    title[sizeof title - 1] = '\0';
    for (size_t i = 0; i < sizeof title - 1; i += 9) {
        title[i] = ' ';
    }
    memset(file_name, 'F', sizeof file_name - 1);
    memcpy(file_name + sizeof file_name - 5, ".zip", 5);
    for (size_t i = 0; i < sizeof file_name - 5; i += 11) {
        file_name[i] = ' ';
    }
    static char raw[RAW_MAX];
    snprintf(raw, sizeof raw,
             JSON_OK "{\"id\":1,\"platform_id\":1,\"name\":\"%s\",\"fs_name\":\"%s\","
                     "\"fs_size_bytes\":%u,\"files\":[{\"rom_id\":1,\"file_name\":\"%s\","
                     "\"file_size_bytes\":%u}]}",
             title, file_name, BODY_BYTES, file_name, BODY_BYTES);
    serve_raw("/api/roms/1", raw);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    TEST_PRINTF("%zu lines: names cut at %d lines each, the rest still shown", view()->line_count,
                APP_DETAILS_NAME_LINES);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(SKIFF_APP_LINES_MAX, view()->line_count);
    TEST_ASSERT_NOT_NULL(strstr(view()->lines[APP_DETAILS_NAME_LINES - 1], "..."));
    TEST_ASSERT_NOT_NULL(strstr(view()->lines[(size_t)2 * APP_DETAILS_NAME_LINES - 1], "..."));
    TEST_ASSERT_EQUAL_STRING("Size: 2 KB", view()->lines[(size_t)2 * APP_DETAILS_NAME_LINES]);
    TEST_ASSERT_TRUE(shows("Skiff can't install this file"));
    TEST_ASSERT_EQUAL_STRING("Free space: 1.5 GB", view()->lines[view()->line_count - 1]);
    assert_lines_beside_the_cover();
}

static void test_leaving_a_game_while_its_cover_loads_drops_the_cover(void) {
    open_paired_library(2);
    serve_rom_with_cover(1, COVER_PATH(1));
    fake_route *cover = serve_cover(COVER_URL(1), "HTTP/1.1 200 OK", COVER_PNG, sizeof COVER_PNG);
    frame(SKIFF_UI_ACTION_CONFIRM);
    for (int i = 0; i < FRAMES_MAX && !app->has_rom; i++) {
        frame(0);
    }
    env_state.hold_calls = 1;
    frame(0);
    TEST_ASSERT_EQUAL_INT(CALL_COVER, app->call.kind);
    TEST_PRINTF("Back while the cover loads: told to stop, the library is back, no error");
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(1, env_state.cancels);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    env_state.hold_calls = 0;
    finish_call();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    TEST_ASSERT_FALSE(app->has_cover);
    TEST_PRINTF("the cover that came anyway is not shown on the next game");
    frame(SKIFF_UI_ACTION_DOWN);
    open_details(2);
    TEST_ASSERT_NULL(view()->cover);
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, cover->uses);
}

static void test_a_cover_that_lands_under_a_confirmation_is_kept(void) {
    record_installed(1, body_crc);
    open_paired_library(1);
    serve_rom_with_cover(1, COVER_PATH(1));
    serve_cover(COVER_URL(1), "HTTP/1.1 200 OK", COVER_PNG, sizeof COVER_PNG);
    frame(SKIFF_UI_ACTION_CONFIRM);
    for (int i = 0; i < FRAMES_MAX && !app->has_rom; i++) {
        frame(0);
    }
    env_state.hold_calls = 1;
    frame(0);
    TEST_ASSERT_EQUAL_INT(CALL_COVER, app->call.kind);
    TEST_PRINTF("Download on an installed game asks to replace it while the cover loads");
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_CONFIRM, view()->screen);
    env_state.hold_calls = 0;
    finish_call();
    TEST_PRINTF("No: back on the game, with its cover");
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_DETAILS, view()->screen);
    TEST_ASSERT_NOT_NULL(view()->cover);
}

static void test_a_cover_lost_with_the_network_shows_no_error(void) {
    open_paired_library(2);
    serve_rom_with_cover(1, COVER_PATH(1));
    fake_route *cover = serve_cover(COVER_URL(1), "HTTP/1.1 200 OK", COVER_PNG, sizeof COVER_PNG);
    cover->fail_before_response = SKIFF_ERR_NET_CONNECTION_LOST;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    TEST_PRINTF("the game shows without its cover; the next request joins the Wi-Fi again");
    TEST_ASSERT_TRUE(shows("Game 1.iso"));
    TEST_ASSERT_NULL(view()->cover);
    TEST_ASSERT_FALSE(app->net_joined);
    TEST_PRINTF("with the network gone, no cover request is made to join it again");
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    app->request = REQUEST_COVER;
    app->screen = SKIFF_APP_SCREEN_DETAILS;
    const int joins = env_state.net_starts;
    frame(0);
    TEST_ASSERT_EQUAL_INT(joins, env_state.net_starts);
    TEST_ASSERT_FALSE(app_call_busy(app));
}

static void test_a_lost_connection_under_a_dropped_request_still_counts(void) {
    open_paired_library(2);
    fake_route *lost = serve_raw("/api/roms/1", JSON_OK "{}");
    lost->fail_before_response = SKIFF_ERR_NET_CONNECTION_LOST;
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_CONFIRM);
    frame(0);
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_NOT_NULL(app->transport);
    env_state.hold_calls = 0;
    finish_call();
    TEST_PRINTF("the connection is given up and the Wi-Fi joined again before the next request");
    TEST_ASSERT_NULL(app->transport);
    TEST_ASSERT_FALSE(app->net_joined);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    const int joins = env_state.net_starts;
    frame(SKIFF_UI_ACTION_DOWN);
    serve_rom(2);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    TEST_ASSERT_EQUAL_INT(joins + 1, env_state.net_starts);
    TEST_ASSERT_TRUE(shows("Game 2.iso"));
}

static void test_a_page_asked_for_while_another_loaded_is_dropped_once_off_screen(void) {
    open_paired_library(80);
    serve_page(25, 25, 80);
    serve_page(50, 25, 80);
    serve_page(75, 5, 80);
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(0);
    TEST_ASSERT_EQUAL_INT(CALL_PAGE, app->call.kind);
    TEST_ASSERT_EQUAL_UINT64(1, app->call.page_index);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    TEST_PRINTF("page 2 waits for page 1's request; then the player goes back to the top");
    TEST_ASSERT_EQUAL_INT(REQUEST_PAGE, app->request);
    TEST_ASSERT_EQUAL_UINT64(2, app->request_page);
    for (int i = 0; i < 6; i++) {
        frame(SKIFF_UI_ACTION_PAGE_UP);
    }
    TEST_ASSERT_EQUAL_size_t(0, view()->list.first);
    const size_t requests = transport.request_count;
    env_state.hold_calls = 0;
    finish_call();
    wait_ms(500);
    TEST_PRINTF("%zu request(s) after page 1's: page 2 is no longer on screen",
                transport.request_count - requests - 1);
    TEST_ASSERT_EQUAL_size_t(requests + 1, transport.request_count);
    TEST_ASSERT_FALSE(app_call_busy(app));
}

static void test_a_new_server_drops_what_the_old_one_was_still_sending(void) {
    open_paired_library(30);
    serve_page(25, 5, 30);
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(0);
    TEST_ASSERT_EQUAL_INT(CALL_PAGE, app->call.kind);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, app_reset_server(app));
    TEST_PRINTF("reset under a running page request: the request is dropped, the client after it");
    TEST_ASSERT_EQUAL_INT(1, env_state.cancels);
    TEST_ASSERT_NOT_NULL(app->transport);
    env_state.hold_calls = 0;
    finish_call();
    TEST_ASSERT_NULL(app->transport);
    for (size_t i = 0; i < SKIFF_APP_CACHED_PAGES; i++) {
        TEST_ASSERT_FALSE(app->pages[i].valid);
    }
}

static void test_a_request_done_before_its_start_returns_keeps_its_error(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_raw("/api/heartbeat", "HTTP/1.1 500 Internal Server Error\r\n\r\n");
    env_state.run_at_start = 1;
    create_app();
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows(skiff_error_text(SKIFF_LANGUAGE_ENGLISH, SKIFF_ERR_ROMM_SERVER)));
}

static void test_a_request_that_cannot_start_shows_why(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    env_state.call_start_error = SKIFF_ERR_NO_MEMORY;
    create_app();
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows(skiff_error_text(SKIFF_LANGUAGE_ENGLISH, SKIFF_ERR_NO_MEMORY)));
    TEST_ASSERT_EQUAL_INT(0, env_state.calls_started);
    TEST_ASSERT_FALSE(app_call_busy(app));
}

static void test_leaving_a_pairing_drops_its_poll_and_wipes_its_codes(void) {
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_TOKEN, "romm/device-token.http"));
    create_app();
    run_until(SKIFF_APP_SCREEN_PAIR);
    env_state.hold_calls = 1;
    wait_ms(5000);
    TEST_ASSERT_EQUAL_INT(CALL_PAIRING_POLL, app->call.kind);
    TEST_PRINTF("Settings during the poll: the pairing ends and its answer is not used");
    frame(SKIFF_UI_ACTION_MENU);
    TEST_ASSERT_EQUAL_INT(1, env_state.cancels);
    env_state.hold_calls = 0;
    finish_call();
    TEST_ASSERT_EQUAL_STRING("", app->call.pairing.device_code);
    TEST_ASSERT_EQUAL_STRING("", app->call.pairing_result.token);
    char config[TEXT_MAX];
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_ASSERT_NULL(strstr(config, "token"));
}

static void test_a_lost_network_is_joined_again_before_retrying(void) {
    open_paired_library(30);
    char path[256];
    snprintf(path, sizeof path, "/api/roms?platform_ids=1&limit=%d&offset=25" LIST_QUERY,
             SKIFF_ROMM_PAGE_SIZE);
    fake_route *lost = serve_raw(path, JSON_OK "{}");
    lost->fail_before_response = SKIFF_ERR_NET_CONNECTION_LOST;
    lost->max_uses = 1;
    serve_page(25, 5, 30);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows("[111]"));
    const int joins = env_state.net_starts;
    const int opens = env_state.opens;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_PRINTF("joins %d -> %d, transports %d -> %d", joins, env_state.net_starts, opens,
                env_state.opens);
    TEST_ASSERT_EQUAL_INT(joins + 1, env_state.net_starts);
    TEST_ASSERT_GREATER_THAN_INT(opens, env_state.opens);
    TEST_ASSERT_TRUE(shows("Game 30"));
    TEST_PRINTF("the worker's client never used the browse transport the failure replaced");
    TEST_ASSERT_TRUE(env_state.spec.romm->transport != app->transport);
    skiff_http_response response;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NOT_IMPLEMENTED,
                          env_state.spec.romm->transport->ops->perform(
                              env_state.spec.romm->transport, NULL, &response));
}

static void test_cancelling_pair_again_returns_to_the_library(void) {
    open_paired_library(1);
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    frame(SKIFF_UI_ACTION_MENU);
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_BACK, english(SKIFF_TEXT_BACK)));
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SETTINGS, view()->screen);
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    char config[TEXT_MAX];
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_ASSERT_NOT_NULL(strstr(config, "token = " TOKEN));
    TEST_ASSERT_EQUAL_INT(0, env_state.worker_stops);
}

static void test_a_server_change_that_cannot_be_saved_keeps_the_old_server(void) {
    open_paired_library(1);
    storage.fail_suffix = SKIFF_STORAGE_DRAFT_SUFFIX;
    storage.rename_error = SKIFF_ERR_STORAGE_IO;
    frame(SKIFF_UI_ACTION_MENU);
    frame(SKIFF_UI_ACTION_CONFIRM);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, "https://other.test");
    frame(0);
    print_view();
    TEST_ASSERT_TRUE(shows("[302]"));
    TEST_ASSERT_EQUAL_STRING(SERVER, app->settings.server_url);
    TEST_PRINTF("OK connects to the old server again, with a new worker");
    storage.rename_error = SKIFF_OK;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_INT(1, env_state.worker_stops);
    TEST_ASSERT_EQUAL_INT(2, env_state.worker_starts);
    TEST_ASSERT_EQUAL_STRING("Bearer " TOKEN, env_state.spec.romm->authorization);
}

static void test_a_failed_details_request_is_retried_for_the_same_rom(void) {
    open_paired_library(2);
    fake_route *broken = serve_raw("/api/roms/1", "HTTP/1.1 500 Internal Server Error\r\n\r\n");
    broken->max_uses = 1;
    open_details_failing();
    TEST_ASSERT_TRUE(shows("[203]"));
    serve_rom(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    TEST_ASSERT_TRUE(app->has_rom);
    TEST_ASSERT_TRUE(shows("Game 1.iso"));
}

static void test_a_lost_connection_joins_again_then_opens_the_same_rom(void) {
    open_paired_library(2);
    fake_route *lost = serve_raw("/api/roms/1", JSON_OK "{}");
    lost->fail_before_response = SKIFF_ERR_NET_CONNECTION_LOST;
    lost->max_uses = 1;
    serve_rom(1);
    const int joins = env_state.net_starts;
    open_details_failing();
    TEST_ASSERT_TRUE(shows("[111]"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    TEST_ASSERT_EQUAL_INT(joins + 1, env_state.net_starts);
    TEST_ASSERT_TRUE(app->has_rom);
    TEST_ASSERT_TRUE(shows("Game 1.iso"));
}

static void test_a_worker_that_cannot_start_is_shown(void) {
    env_state.start_error = SKIFF_ERR_NO_MEMORY;
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_library(1);
    create_app();
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows("[2]"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_RETRY)));
    env_state.start_error = SKIFF_OK;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_INT(2, env_state.worker_starts);
}

static void test_a_new_server_forgets_the_old_installs(void) {
    record_installed(1, body_crc);
    open_paired_library(1);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_INSTALLED), view()->rows[0].detail);
    serve_heartbeat("5.3.1");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    frame(SKIFF_UI_ACTION_MENU);
    frame(SKIFF_UI_ACTION_CONFIRM);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, "https://other.test");
    frame(0);
    TEST_PRINTF("no downloads, but an installed game: the player is asked too");
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_CONFIRM, view()->screen);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_ASSERT_EQUAL_size_t(0, app->manifest->count);
    char manifest[RAW_MAX];
    read_app_file(SKIFF_INSTALL_MANIFEST_NAME, manifest, sizeof manifest);
    TEST_PRINTF("installed.json: %s", manifest);
    TEST_ASSERT_NULL(strstr(manifest, "Game 1.iso"));
    TEST_PRINTF("the game itself stays");
    char iso[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO/Game 1.iso", iso, sizeof iso));
    FILE *still = fopen(iso, "rb");
    TEST_ASSERT_NOT_NULL(still);
    fclose(still);
}

static void test_an_abandoned_retry_is_forgotten(void) {
    open_paired_library(2);
    fake_route *broken = serve_raw("/api/roms/1", "HTTP/1.1 500 Internal Server Error\r\n\r\n");
    TEST_ASSERT_NOT_NULL(broken);
    open_details_failing();
    TEST_ASSERT_EQUAL_INT(REQUEST_ROM, app->failed_request);
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    TEST_ASSERT_EQUAL_INT(REQUEST_NONE, app->failed_request);
}

static void test_a_new_code_after_a_lost_connection_joins_first(void) {
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    fake_route *lost = serve_raw(PATH_INIT, JSON_OK "{}");
    lost->fail_before_response = SKIFF_ERR_NET_CONNECTION_LOST;
    lost->max_uses = 1;
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    create_app();
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows("[111]"));
    const int joins = env_state.net_starts;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_ASSERT_EQUAL_INT(joins + 1, env_state.net_starts);
    TEST_ASSERT_TRUE(app->pairing.active);
}

static void test_queued_downloads_hold_their_installed_records(void) {
    open_paired_library(2);
    app->manifest->count = SKIFF_INSTALL_RECORDS_MAX - 1;
    for (size_t i = 0; i < app->manifest->count; i++) {
        app->manifest->records[i].rom_id = 1000 + i;
    }
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_PRINTF("511 records and one queued download: the 512th place is promised");
    frame(SKIFF_UI_ACTION_DOWN);
    open_details(2);
    frame(SKIFF_UI_ACTION_CONFIRM);
    print_view();
    TEST_ASSERT_TRUE(shows("Skiff can't keep track of more installed games."));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
}

static void test_a_cancelled_retry_needs_room_in_the_installed_list(void) {
    open_paired_library(2);
    app->manifest->count = SKIFF_INSTALL_RECORDS_MAX - 1;
    for (size_t i = 0; i < app->manifest->count; i++) {
        app->manifest->records[i].rom_id = 1000 + i;
    }
    TEST_PRINTF("A queued, then cancelled; B takes the last place; A cannot come back");
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_EXTRA);
    run_until(SKIFF_APP_SCREEN_QUEUE);
    frame(SKIFF_UI_ACTION_EXTRA);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_QUEUE);
    frame(SKIFF_UI_ACTION_BACK);
    frame(SKIFF_UI_ACTION_DOWN);
    open_details(2);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    frame(SKIFF_UI_ACTION_EXTRA);
    run_until(SKIFF_APP_SCREEN_QUEUE);
    skiff_job jobs[SKIFF_JOBS_MAX];
    list_jobs(jobs);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_CANCELLED, jobs[0].state);
    frame(SKIFF_UI_ACTION_CONFIRM);
    print_view();
    TEST_ASSERT_TRUE(shows("Skiff can't keep track of more installed games."));
    list_jobs(jobs);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_CANCELLED, jobs[0].state);
}

static void test_another_network_restarts_the_worker_on_it(void) {
    open_paired_library(30);
    TEST_ASSERT_EQUAL_INT(1, env_state.spec.profile);
    char path[256];
    snprintf(path, sizeof path, "/api/roms?platform_ids=1&limit=%d&offset=25" LIST_QUERY,
             SKIFF_ROMM_PAGE_SIZE);
    fake_route *lost = serve_raw(path, JSON_OK "{}");
    lost->fail_before_response = SKIFF_ERR_NET_DNS;
    lost->max_uses = 1;
    serve_page(25, 5, 30);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    frame(SKIFF_UI_ACTION_PAGE_DOWN);
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows("[101]"));
    frame(SKIFF_UI_ACTION_MENU);
    frame(0);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_DIALOG_NETWORK, view()->dialog);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, NULL);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_PRINTF("worker stops %d, starts %d, profile %d", env_state.worker_stops,
                env_state.worker_starts, env_state.spec.profile);
    TEST_ASSERT_EQUAL_INT(1, env_state.worker_stops);
    TEST_ASSERT_EQUAL_INT(2, env_state.worker_starts);
    TEST_ASSERT_EQUAL_INT(PICKED_PROFILE, env_state.spec.profile);
}

static int lock_held[4];
static int manifest_saves_under_lock;
static int manifest_saves;
static void tracked_lock(void *ctx) { *(int *)ctx = 1; }
static void tracked_unlock(void *ctx) { *(int *)ctx = 0; }
static void watch_manifest(void *ctx, const char *call, const char *path) {
    (void)ctx;
    if (strcmp(call, "open") == 0 && strstr(path, SKIFF_INSTALL_MANIFEST_NAME) != NULL) {
        manifest_saves++;
        manifest_saves_under_lock += lock_held[3];
    }
}

static void test_installed_json_is_saved_without_the_lock_the_ui_reads(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_library(1);
    memset(lock_held, 0, sizeof lock_held);
    const skiff_app_config config = {.storage = &storage.base,
                                     .roots = roots,
                                     .lock = tracked_lock,
                                     .unlock = tracked_unlock,
                                     .log_lock = &lock_held[0],
                                     .jobs_lock = &lock_held[1],
                                     .jobs_save_lock = &lock_held[2],
                                     .manifest_lock = &lock_held[3]};
    const skiff_app_env env = make_env();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_app_create(&config, &env, &app));
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    open_details(1);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    serve_content(1);
    manifest_saves = 0;
    manifest_saves_under_lock = 0;
    storage.on_call = watch_manifest;
    run_worker_once();
    storage.on_call = NULL;
    TEST_PRINTF("installed.json opened %d time(s) by the save, %d with the manifest lock held",
                manifest_saves, manifest_saves_under_lock);
    TEST_ASSERT_GREATER_THAN_INT(0, manifest_saves);
    TEST_ASSERT_EQUAL_INT(0, manifest_saves_under_lock);
    TEST_ASSERT_EQUAL_size_t(1, app->manifest->count);
}

/* ---- The log ---- */

static void test_secrets_and_the_log_level(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN
                 "\n[headers]\nX-Short = abc\nCF-Access-Client-Secret = proxy-secret-value\n"
                 "[network]\nprofile = 1\n[log]\nlevel = debug\n");
    serve_library(1);
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    skiff_log_write(skiff_app_log(app), SKIFF_LOG_INFO, "test", "%s %s %s", TOKEN,
                    "proxy-secret-value", "abc");
    skiff_app_destroy(app);
    app = NULL;
    char log[RAW_MAX];
    read_app_file(SKIFF_APP_LOG_FILE_NAME, log, sizeof log);
    TEST_PRINTF("skiff.log:\n%s", log);
    TEST_ASSERT_NOT_NULL(strstr(log, "test: [redacted] [redacted] abc"));
    TEST_PRINTF("the 3-character header is not registered, so nothing is withheld");
    TEST_ASSERT_NULL(strstr(log, "withheld"));
    TEST_PRINTF("level = debug keeps the page loads");
    TEST_ASSERT_NOT_NULL(strstr(log, "D app: page 0"));
}

static void test_transport_settings_come_from_the_skiff_folder(void) {
    static const char text[] = "[server]\nca_file = ca.pem\n[mtls]\ncert_file = c.pem\n"
                               "key_file = k.pem\n[headers]\nX-A = 1\n";
    skiff_config config;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_parse(text, sizeof text - 1, &config, NULL));
    skiff_app_transport_settings settings;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_app_transport_settings_from(&config, &roots, &settings));
    char expected[TEMP_DIR_PATH_MAX];
    app_file("ca.pem", expected, sizeof expected);
    TEST_ASSERT_EQUAL_STRING(expected, settings.ca_file);
    app_file("k.pem", expected, sizeof expected);
    TEST_ASSERT_EQUAL_STRING(expected, settings.client_key);
    TEST_ASSERT_EQUAL_size_t(1, settings.header_count);
    TEST_ASSERT_EQUAL_STRING("X-A", settings.headers[0].name);
    TEST_PRINTF("a set ca_file replaces the bundle: %s", settings.ca_file);
    TEST_ASSERT_NULL(strstr(settings.ca_file, SKIFF_APP_DEFAULT_CA_FILE));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_app_transport_settings_from(NULL, &roots, &settings));
}

static void test_an_unset_ca_file_trusts_the_bundled_cas(void) {
    static const char text[] = "[server]\nurl = https://romm.test\nca_file =\n";
    skiff_config config;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_parse(text, sizeof text - 1, &config, NULL));
    skiff_app_transport_settings settings;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_app_transport_settings_from(&config, &roots, &settings));
    char expected[TEMP_DIR_PATH_MAX];
    app_file(SKIFF_APP_DEFAULT_CA_FILE, expected, sizeof expected);
    TEST_PRINTF("an empty ca_file: %s", settings.ca_file);
    TEST_ASSERT_EQUAL_STRING(expected, settings.ca_file);
    TEST_PRINTF("no client certificate unless one is set");
    TEST_ASSERT_EQUAL_STRING("", settings.client_cert);
    TEST_ASSERT_EQUAL_STRING("", settings.client_key);
}

static void test_the_app_browses_and_downloads_with_the_bundled_cas(void) {
    open_paired_library(1);
    char expected[TEMP_DIR_PATH_MAX];
    app_file(SKIFF_APP_DEFAULT_CA_FILE, expected, sizeof expected);
    TEST_PRINTF("browsing transport CA file: %s", env_state.opened_ca_file);
    TEST_ASSERT_EQUAL_STRING(expected, env_state.opened_ca_file);
    TEST_ASSERT_NOT_NULL(env_state.spec.transport);
    TEST_PRINTF("download worker CA file: %s", env_state.spec.transport->ca_file);
    TEST_ASSERT_EQUAL_STRING(expected, env_state.spec.transport->ca_file);
}

static void test_wrapping_breaks_between_words_and_inside_long_ones(void) {
    char lines[4][SKIFF_TEXT_MAX];
    TEST_PRINTF("10 characters a line");
    size_t count = skiff_app_wrap("one two three four", 60.0f, env_measure, NULL, lines, 4);
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_EQUAL_STRING("one two", lines[0]);
    TEST_ASSERT_EQUAL_STRING("three four", lines[1]);
    count = skiff_app_wrap("https://romm.example/pair", 60.0f, env_measure, NULL, lines, 4);
    TEST_ASSERT_EQUAL_size_t(3, count);
    TEST_ASSERT_EQUAL_STRING("https://ro", lines[0]);
    TEST_ASSERT_EQUAL_STRING("mm.example", lines[1]);
    TEST_ASSERT_EQUAL_STRING("/pair", lines[2]);
    TEST_PRINTF("UTF-8 is cut between characters: caf\\xC3\\xA9 counts 5 bytes");
    count = skiff_app_wrap("caf\xC3\xA9"
                           "caf\xC3\xA9",
                           24.0f, env_measure, NULL, lines, 4);
    TEST_ASSERT_EQUAL_size_t(3, count);
    TEST_ASSERT_EQUAL_STRING("caf", lines[0]);
    TEST_ASSERT_EQUAL_STRING("\xC3\xA9"
                             "ca",
                             lines[1]);
    TEST_PRINTF("what does not fit the lines ends in an ellipsis");
    count = skiff_app_wrap("aa bb cc dd ee ff", 30.0f, env_measure, NULL, lines, 2);
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_EQUAL_STRING("aa bb", lines[0]);
    TEST_ASSERT_EQUAL_STRING("cc...", lines[1]);
    TEST_ASSERT_EQUAL_size_t(0, skiff_app_wrap("   ", 30.0f, env_measure, NULL, lines, 2));
    TEST_ASSERT_EQUAL_size_t(0, skiff_app_wrap(NULL, 30.0f, env_measure, NULL, lines, 2));
}

static int lock_takes;
static void count_lock(void *ctx) {
    (void)ctx;
    lock_takes++;
}
static void no_unlock(void *ctx) { (void)ctx; }

static void test_bad_arguments_and_locks(void) {
    skiff_app_env env = make_env();
    skiff_app_config config = {.storage = &storage.base, .roots = roots};
    skiff_app *other = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_app_create(NULL, &env, &other));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_app_create(&config, &env, NULL));
    env.net_profile_name = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_app_create(&config, &env, &other));
    TEST_ASSERT_NULL(other);
    env = make_env();
    env.call_cancel = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_app_create(&config, &env, &other));
    env = make_env();
    env.suspends = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_app_create(&config, &env, &other));
    env = make_env();
    env.net_online = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_app_create(&config, &env, &other));
    env = make_env();
    int mutexes[4];
    config.lock = count_lock;
    config.unlock = no_unlock;
    config.log_lock = &mutexes[0];
    config.jobs_lock = &mutexes[1];
    config.jobs_save_lock = &mutexes[2];
    TEST_PRINTF("three of the four mutexes");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_app_create(&config, &env, &other));
    config.manifest_lock = &mutexes[1];
    TEST_PRINTF("one mutex for two locks");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_app_create(&config, &env, &other));
    config.manifest_lock = &mutexes[3];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_app_create(&config, &env, &app));
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_library(1);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_PRINTF("the log, the queue and installed.json took their locks %d times", lock_takes);
    TEST_ASSERT_GREATER_THAN_INT(0, lock_takes);
    skiff_app_update(NULL, 0);
    TEST_ASSERT_NULL(skiff_app_view_now(NULL));
    TEST_ASSERT_FALSE(skiff_app_quit_requested(NULL));
    TEST_ASSERT_NULL(skiff_app_log(NULL));
    skiff_app_dialog_done(NULL, SKIFF_APP_DIALOG_ACCEPTED, "x");
    TEST_PRINTF("START does not quit: HOME -> Quit (the platform) is the way out");
    frame(SKIFF_UI_ACTION_START);
    TEST_ASSERT_FALSE(skiff_app_quit_requested(app));
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_START, english(SKIFF_TEXT_QUIT)));
}

/* ---- Save sync ---- */

/* A token paired with every scope save sync needs, and the device RomM gave this PSP. */
#define SAVE_SCOPES "platforms.read,roms.read,assets.read,assets.write,devices.read,devices.write"
#define SAVE_DEVICE "00000000-0000-4000-8000-0000000000aa"
#define SAVES_ROMM "5.4.0"
/* The games of the launch check: one only on this PSP (installed by Skiff, so RomM is asked its
 * save ID), one only in RomM, one with different saves on both and no baseline. */
#define GAME_UP "SKIF00001"
#define GAME_DOWN "SKIF00002"
#define GAME_ASK "SKIF00003"
#define ROM_UP 101U
#define ROM_DOWN 102U
#define ROM_ASK 103U
#define SAVE_UP 201U
#define SAVE_DOWN 202U
#define SAVE_ASK 203U
#define SAVE_KEPT 204U
#define SAVE_SESSION 7U
#define DATA_FOLDER "DATA00"
#define SAVES_LIST_PATH "/api/roms?platform_ids=1&limit=25&offset=0" LIST_QUERY "&has_saves=true"
#define NEGOTIATE_PATH "/api/sync/negotiate"
#define COMPLETE_PATH "/api/sync/sessions/7/complete"
#define SERVER_TIME "2026-10-11T18:19:00+00:00"
#define OTHER_DEVICE_NAME "Argosy on Pixel"
#define HASH_TEXT_MAX 33
/* A save another device uploaded meanwhile. */
#define OTHER_HASH "0123456789abcdef0123456789abcdef"
#define UNIT_BYTES_MAX 4096
#define SAVE_JSON_MAX 1024
#define OPS_MAX 2048
/* A path under the save root: the root and a save file's folder and name. */
#define SAVE_PATH_MAX (SKIFF_STORAGE_PATH_MAX + 64)

/* What each game's saves hold, here and in RomM. */
#define UP_LOCAL "slot one, played on this PSP"
#define DOWN_SERVER "slot one, saved in PPSSPP"
#define DOWN_OLD "slot one, an older save of this PSP"
#define ASK_LOCAL "slot one, this PSP's run"
#define ASK_SERVER "slot one, the phone's run"

static void write_saves_config(const char *scopes) {
    char config[TEXT_MAX];
    snprintf(config, sizeof config,
             "[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN
             "\ndevice_identifier = 00112233445566778899aabbccddeeff\ndevice_id = " SAVE_DEVICE
             "\nscopes = %s\n[network]\nprofile = 1\n",
             scopes);
    write_config(config);
}

/* A save file in roots' PSP/SAVEDATA: <save_target>DATA00/DATA.BIN. */
static void put_save_in(const skiff_storage_roots *where, const char *save_target,
                        const char *text) {
    char folder[SAVE_PATH_MAX];
    snprintf(folder, sizeof folder, "%s/%s" DATA_FOLDER, where->saves, save_target);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, folder));
    char path[SAVE_PATH_MAX + 16];
    snprintf(path, sizeof path, "%s/DATA.BIN", folder);
    write_file(path, text, strlen(text));
}

static void put_save(const char *save_target, const char *text) {
    put_save_in(&roots, save_target, text);
}

static void read_save(const char *save_target, char *out, size_t size) {
    char path[SAVE_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s" DATA_FOLDER "/DATA.BIN", roots.saves, save_target);
    read_file(path, out, size);
}

/* RomM's content_hash of the game's saves on this PSP, as Skiff computes it. */
static void local_hash(const char *save_target, char *out) {
    skiff_saves_unit *unit = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_unit_create(&unit));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_saves_collect(posix, &roots, SKIFF_STORAGE_ROOT_SAVES, save_target, unit));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_hash_unit(posix, &roots, unit));
    snprintf(out, HASH_TEXT_MAX, "%s", unit->content_hash);
    skiff_saves_unit_destroy(unit);
}

/* The unit RomM would serve for save_target holding text, made on another Memory Stick: its bytes
 * into out and its hash. */
static size_t server_unit(const char *save_target, const char *text, unsigned char *out,
                          size_t size, char *hash) {
    char root[TEMP_DIR_PATH_MAX];
    char app_root[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "server-side", root, sizeof root));
    TEST_ASSERT_TRUE(temp_dir_path(dir, "server-side/app", app_root, sizeof app_root));
    skiff_storage_roots other;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_init(root, app_root, &other));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, other.app));
    put_save_in(&other, save_target, text);
    skiff_saves_unit *unit = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_unit_create(&unit));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_saves_collect(posix, &other, SKIFF_STORAGE_ROOT_SAVES, save_target, unit));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_write_unit(posix, &other, unit, "app:/unit.zip"));
    snprintf(hash, HASH_TEXT_MAX, "%s", unit->content_hash);
    skiff_saves_unit_destroy(unit);
    char path[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "server-side/app/unit.zip", path, sizeof path));
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    const size_t got = fread(out, 1, size, file);
    fclose(file);
    return got;
}

/* A PSP ROM with saves as RomM lists it, its save ID read by RomM's scan. */
static int saves_item(char *out, size_t size, unsigned id, const char *title, const char *target) {
    return snprintf(out, size,
                    "{\"id\":%u,\"platform_id\":1,\"name\":\"%s\",\"fs_name\":\"%s.iso\","
                    "\"fs_size_bytes\":%u,\"save_target\":\"%s\",\"save_target_layout\":"
                    "\"folder-prefix\"}",
                    id, title, title, BODY_BYTES, target);
}

/* The PSP ROMs RomM holds saves for: items, a JSON list's inside. */
static void serve_saves_list(const char *items, unsigned count) {
    char raw[RAW_MAX];
    snprintf(raw, sizeof raw, JSON_OK "{\"items\":[%s],\"total\":%u,\"limit\":%d,\"offset\":0}",
             items, count, SKIFF_ROMM_PAGE_SIZE);
    serve_raw(SAVES_LIST_PATH, raw);
}

/* A ROM's details with its save ID, as RomM 5.4 answers for a scanned disc. */
static void serve_rom_with_save(unsigned id, const char *title, const char *target) {
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, id, title, target);
    item[strlen(item) - 1] = '\0';
    char raw[2 * SAVE_JSON_MAX];
    snprintf(raw, sizeof raw,
             JSON_OK "%s,\"files\":[{\"rom_id\":%u,\"file_name\":\"%s.iso\","
                     "\"file_size_bytes\":%u}]}",
             item, id, title, BODY_BYTES);
    char path[64];
    snprintf(path, sizeof path, "/api/roms/%u", id);
    serve_raw(path, raw);
}

/* One negotiate operation of slot "autosave" (another slot with other_slot); save 0 and hash
 * NULL for none. */
static int sync_op(char *out, size_t size, const char *action, unsigned rom, unsigned save,
                   const char *hash, const char *slot) {
    char save_id[16] = "null";
    char server_hash[48] = "null";
    if (save != 0) {
        snprintf(save_id, sizeof save_id, "%u", save);
    }
    if (hash != NULL) {
        snprintf(server_hash, sizeof server_hash, "\"%s\"", hash);
    }
    return snprintf(out, size,
                    "{\"action\":\"%s\",\"rom_id\":%u,\"save_id\":%s,\"file_name\":\"save.zip\","
                    "\"slot\":\"%s\",\"emulator\":\"ppsspp\",\"reason\":\"test\","
                    "\"server_updated_at\":%s,\"server_content_hash\":%s}",
                    action, rom, save_id, slot, hash != NULL ? "\"" SERVER_TIME "\"" : "null",
                    server_hash);
}

static fake_route *serve_negotiate(const char *operations) {
    char raw[OPS_MAX + 128];
    snprintf(raw, sizeof raw, JSON_OK "{\"session_id\":%u,\"operations\":[%s]}", SAVE_SESSION,
             operations);
    return serve_raw(NEGOTIATE_PATH, raw);
}

/* A save as RomM answers for it (SaveSchema), uploaded by device_name's device. */
static void save_json(char *out, size_t size, unsigned id, unsigned rom, const char *hash,
                      const char *device_name) {
    snprintf(out, size,
             "{\"id\":%u,\"rom_id\":%u,\"file_name\":\"save [2026-10-11_18-19-00].zip\","
             "\"slot\":\"autosave\",\"emulator\":\"ppsspp\",\"content_hash\":\"%s\","
             "\"updated_at\":\"" SERVER_TIME "\",\"file_size_bytes\":100,"
             "\"origin_device_id\":\"other-device\",\"device_syncs\":[{\"device_id\":"
             "\"other-device\",\"device_name\":\"%s\"}]}",
             id, rom, hash, device_name);
}

static fake_route *serve_save(const char *path, unsigned id, unsigned rom, const char *hash) {
    char json[SAVE_JSON_MAX];
    save_json(json, sizeof json, id, rom, hash, OTHER_DEVICE_NAME);
    char raw[SAVE_JSON_MAX + 64];
    snprintf(raw, sizeof raw, JSON_OK "%s", json);
    return serve_raw(path, raw);
}

static void upload_path(unsigned rom, const char *hash, const char *overwrite, char *out,
                        size_t size) {
    snprintf(out, size,
             "/api/saves?rom_id=%u&emulator=psp_console&slot=autosave&device_id=" SAVE_DEVICE
             "&content_hash=%s&overwrite=%s&autocleanup=true&autocleanup_limit=10",
             rom, hash, overwrite);
}

static fake_route *serve_upload(unsigned rom, const char *hash, const char *overwrite,
                                unsigned save) {
    char path[512];
    upload_path(rom, hash, overwrite, path, sizeof path);
    return serve_save(path, save, rom, hash);
}

static void serve_content_of(unsigned save, const unsigned char *bytes, size_t size) {
    static char raw[UNIT_BYTES_MAX + 128];
    const int used =
        snprintf(raw, sizeof raw, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", size);
    memcpy(raw + used, bytes, size);
    char path[160];
    snprintf(path, sizeof path, "/api/saves/%u/content?device_id=" SAVE_DEVICE "&optimistic=false",
             save);
    TEST_ASSERT_NOT_NULL(fake_transport_add_raw(&transport, path, raw, (size_t)used + size));
}

static void serve_downloaded(unsigned save, unsigned rom, const char *hash) {
    char path[64];
    snprintf(path, sizeof path, "/api/saves/%u/downloaded", save);
    serve_save(path, save, rom, hash);
}

static void serve_device_of(unsigned save, unsigned rom, const char *hash) {
    char path[128];
    snprintf(path, sizeof path, "/api/saves/%u?device_id=" SAVE_DEVICE, save);
    serve_save(path, save, rom, hash);
}

static void serve_complete(void) { serve_raw(COMPLETE_PATH, JSON_OK "{\"session\":{\"id\":7}}"); }

/* A baseline in saves.json before the app starts: the game synced as server_hash (save) and
 * local_hash, with its folders' summary as they are now. */
static void record_saves_baseline(const char *save_target, unsigned rom, unsigned save,
                                  const char *server_hash, const char *local) {
    skiff_saves_baseline *baseline = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_create(&baseline));
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_storage_resolve(&roots, SKIFF_SAVES_BASELINE_PATH, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_load(baseline, posix, path, NULL));
    skiff_saves_baseline_entry entry;
    memset(&entry, 0, sizeof entry);
    snprintf(entry.save_target, sizeof entry.save_target, "%s", save_target);
    entry.rom_id = rom;
    entry.server_save_id = save;
    snprintf(entry.server_hash, sizeof entry.server_hash, "%s", server_hash);
    snprintf(entry.local_hash, sizeof entry.local_hash, "%s", local);
    entry.synced_ms = UTC_BASE_MS;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_saves_tree_summarize(posix, &roots, SKIFF_STORAGE_ROOT_SAVES,
                                                     save_target, &entry.tree));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_record(baseline, &entry));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_save(baseline, posix, path));
    skiff_saves_baseline_destroy(baseline);
}

static void read_baseline(skiff_saves_baseline **out) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_create(out));
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_storage_resolve(&roots, SKIFF_SAVES_BASELINE_PATH, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_baseline_load(*out, posix, path, NULL));
}

/* Frames until save sync has nothing left to run. */
static void run_saves(void) {
    for (int i = 0; i < FRAMES_MAX && (app_saves_busy(app) || app_call_busy(app)); i++) {
        frame(0);
    }
    frame(0);
    TEST_ASSERT_FALSE(app_saves_busy(app));
    print_view();
}

/* A paired library on RomM version with three games of its own, its token holding scopes. */
static void open_saves_library(const char *scopes, const char *version) {
    write_saves_config(scopes);
    serve_heartbeat(version);
    serve_platforms();
    serve_page(0, 3, 3);
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
}

/* Settings, its third row: the Saves screen. */
static void open_saves_screen(void) {
    frame(SKIFF_UI_ACTION_MENU);
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    print_view();
}

/* Moves the Saves list's selection to the row labelled label. */
static void select_save_row(const char *label) {
    for (size_t i = 0; i < APP_SAVES_GAMES_MAX; i++) {
        const skiff_app_view *v = view();
        const size_t at = v->list.selected - v->list.first;
        if (at < v->row_count && strstr(v->rows[at].label, label) != NULL) {
            return;
        }
        frame(SKIFF_UI_ACTION_DOWN);
    }
    TEST_FAIL_MESSAGE(label);
}

static int requested_path(const char *path) {
    for (size_t i = 0; i < transport.log_count; i++) {
        if (strcmp(transport.log[i].url + strlen(SERVER), path) == 0) {
            return 1;
        }
    }
    return 0;
}

static int requested_prefix(const char *prefix) {
    for (size_t i = 0; i < transport.log_count; i++) {
        if (strncmp(transport.log[i].url + strlen(SERVER), prefix, strlen(prefix)) == 0) {
            return 1;
        }
    }
    return 0;
}

static const fake_request *request_to(const char *path) {
    for (size_t i = 0; i < transport.log_count; i++) {
        if (strcmp(transport.log[i].url + strlen(SERVER), path) == 0) {
            return &transport.log[i];
        }
    }
    return NULL;
}

/* The launch check's three games and what RomM answers for them. */
typedef struct saves_scene {
    char up_hash[HASH_TEXT_MAX];
    char ask_hash[HASH_TEXT_MAX];
    char down_hash[HASH_TEXT_MAX];
    char ask_server_hash[HASH_TEXT_MAX];
    unsigned char down_unit[UNIT_BYTES_MAX];
    size_t down_size;
    unsigned char ask_unit[UNIT_BYTES_MAX];
    size_t ask_size;
} saves_scene;

static saves_scene scene;

/* The scene with ask_title for the game whose saves differ. */
static void set_saves_scene_titled(const char *ask_title) {
    memset(&scene, 0, sizeof scene);
    put_save(GAME_UP, UP_LOCAL);
    put_save(GAME_ASK, ASK_LOCAL);
    local_hash(GAME_UP, scene.up_hash);
    local_hash(GAME_ASK, scene.ask_hash);
    scene.down_size = server_unit(GAME_DOWN, DOWN_SERVER, scene.down_unit, sizeof scene.down_unit,
                                  scene.down_hash);
    scene.ask_size = server_unit(GAME_ASK, ASK_SERVER, scene.ask_unit, sizeof scene.ask_unit,
                                 scene.ask_server_hash);
    record_installed(ROM_UP, body_crc);
    serve_rom_with_save(ROM_UP, "Up Game", GAME_UP);
    char items[RAW_MAX];
    int used = saves_item(items, sizeof items, ROM_DOWN, "Down Game", GAME_DOWN);
    items[used++] = ',';
    saves_item(items + used, sizeof items - (size_t)used, ROM_ASK, ask_title, GAME_ASK);
    serve_saves_list(items, 2);
    char ops[OPS_MAX];
    used = sync_op(ops, sizeof ops, "upload", ROM_UP, 0, NULL, APP_SAVES_SLOT);
    ops[used++] = ',';
    used += sync_op(ops + used, sizeof ops - (size_t)used, "download", ROM_DOWN, SAVE_DOWN,
                    scene.down_hash, APP_SAVES_SLOT);
    ops[used++] = ',';
    used += sync_op(ops + used, sizeof ops - (size_t)used, "download", ROM_DOWN, SAVE_DOWN + 90,
                    scene.ask_hash, "quicksave");
    ops[used++] = ',';
    sync_op(ops + used, sizeof ops - (size_t)used, "conflict", ROM_ASK, SAVE_ASK,
            scene.ask_server_hash, APP_SAVES_SLOT);
    serve_negotiate(ops)->max_uses = 1;
    TEST_PRINTF("a check of one game negotiates for its ROM alone");
    sync_op(ops, sizeof ops, "conflict", ROM_ASK, SAVE_ASK, scene.ask_server_hash, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_upload(ROM_UP, scene.up_hash, "false", SAVE_UP);
    serve_content_of(SAVE_DOWN, scene.down_unit, scene.down_size);
    serve_downloaded(SAVE_DOWN, ROM_DOWN, scene.down_hash);
    serve_device_of(SAVE_ASK, ROM_ASK, scene.ask_server_hash);
    serve_complete();
}

static void set_saves_scene(void) { set_saves_scene_titled("Ask Game"); }

static void test_the_launch_check_syncs_what_needs_no_choice_and_counts_the_rest(void) {
    set_saves_scene();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    TEST_PRINTF("after the library's first page: the check, then every transfer needing no choice");
    run_saves();
    TEST_ASSERT_EQUAL_STRING("Saves: 2 synced, 1 need you", view()->status);

    TEST_PRINTF("negotiate lists this PSP's saves, asks back unlisted ones, filters no emulator");
    const fake_request *negotiate = request_to(NEGOTIATE_PATH);
    TEST_ASSERT_NOT_NULL(negotiate);
    TEST_PRINTF("negotiate body: %s", negotiate->body);
    TEST_ASSERT_NOT_NULL(strstr(negotiate->body, "\"device_id\":\"" SAVE_DEVICE "\""));
    TEST_ASSERT_NOT_NULL(strstr(negotiate->body, "\"restore_unlisted\":true"));
    TEST_ASSERT_NOT_NULL(strstr(negotiate->body, scene.up_hash));
    TEST_ASSERT_NOT_NULL(strstr(negotiate->body, scene.ask_hash));
    TEST_ASSERT_NULL(strstr(negotiate->body, "emulators"));
    TEST_PRINTF("the installed game's save ID came from its details");
    TEST_ASSERT_TRUE(requested_path("/api/roms/101"));

    TEST_PRINTF("Up Game: only on this PSP, uploaded without overwrite, as a long transfer");
    char path[512];
    upload_path(ROM_UP, scene.up_hash, "false", path, sizeof path);
    const fake_request *upload = request_to(path);
    TEST_ASSERT_NOT_NULL(upload);
    TEST_ASSERT_TRUE(upload->long_transfer);
    TEST_ASSERT_NOT_NULL(strstr(upload->body, "filename=\"" GAME_UP ".zip\""));

    TEST_PRINTF("Down Game: only in RomM, downloaded, checked, restored and confirmed");
    char data[64];
    read_save(GAME_DOWN, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING(DOWN_SERVER, data);
    TEST_ASSERT_TRUE(requested_path("/api/saves/202/downloaded"));
    TEST_ASSERT_FALSE_MESSAGE(requested_prefix("/api/saves/203/content"),
                              "a conflict is never transferred on its own");
    TEST_PRINTF("Ask Game: both differ and no baseline; its RomM side's device was read");
    TEST_ASSERT_TRUE(requested_path("/api/saves/203?device_id=" SAVE_DEVICE));
    TEST_ASSERT_TRUE_MESSAGE(requested_path(COMPLETE_PATH), "the session is closed");
    read_save(GAME_ASK, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING(ASK_LOCAL, data);

    skiff_saves_baseline *baseline = NULL;
    read_baseline(&baseline);
    TEST_PRINTF("saves.json: %zu game(s)", baseline->count);
    const skiff_saves_baseline_entry *up = skiff_saves_baseline_find(baseline, GAME_UP);
    const skiff_saves_baseline_entry *down = skiff_saves_baseline_find(baseline, GAME_DOWN);
    TEST_ASSERT_NOT_NULL(up);
    TEST_ASSERT_NOT_NULL(down);
    TEST_ASSERT_NULL(skiff_saves_baseline_find(baseline, GAME_ASK));
    TEST_ASSERT_EQUAL_UINT64(SAVE_UP, up->server_save_id);
    TEST_ASSERT_EQUAL_STRING(scene.up_hash, up->local_hash);
    TEST_ASSERT_EQUAL_UINT64(SAVE_DOWN, down->server_save_id);
    TEST_ASSERT_EQUAL_STRING(scene.down_hash, down->server_hash);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(scene.down_hash, down->local_hash,
                                     "what was restored is what RomM holds");
    TEST_ASSERT_EQUAL_STRING(GAME_UP, skiff_saves_baseline_rom_target(baseline, ROM_UP));
    skiff_saves_baseline_destroy(baseline);

    TEST_PRINTF("the Saves screen, by name");
    wait_ms(SKIFF_APP_NOTE_MS);
    open_saves_screen();
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_SAVES), view()->title);
    TEST_ASSERT_EQUAL_STRING("3 games", view()->status);
    TEST_ASSERT_EQUAL_size_t(3, view()->row_count);
    TEST_ASSERT_EQUAL_STRING("Ask Game", view()->rows[0].label);
    TEST_ASSERT_EQUAL_STRING("Differs from Ro...", view()->rows[0].detail);
    TEST_ASSERT_EQUAL_STRING("Down Game", view()->rows[1].label);
    TEST_ASSERT_EQUAL_STRING("Synced 21 Sep", view()->rows[1].detail);
    TEST_PRINTF("a game RomM lists no saves for yet shows by its save ID");
    TEST_ASSERT_EQUAL_STRING(GAME_UP, view()->rows[2].label);
    TEST_ASSERT_TRUE(shows("Differs from RomM's"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_SYNC)));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_EXTRA, english(SKIFF_TEXT_SYNC_ALL)));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_MENU, english(SKIFF_TEXT_BACKUPS)));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_BACK, english(SKIFF_TEXT_BACK)));
    frame(SKIFF_UI_ACTION_DOWN);
    TEST_ASSERT_TRUE(shows("Last synced 21 Sep"));

    TEST_PRINTF("skiff.log: save lines with ids and hashes, never the token");
    static char log_text[64 * 1024];
    skiff_log_flush(app->log);
    read_app_file(SKIFF_APP_LOG_FILE_NAME, log_text, sizeof log_text);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "save: check: 3 game(s)"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "save: " GAME_DOWN ": download of save 202"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, scene.up_hash));
    TEST_ASSERT_NULL(strstr(log_text, TOKEN));
}

/* The choice screen's two tables: both sides, then the buttons. */
enum { SIDES_TABLE, OPTIONS_TABLE, CHOICE_TABLES };
enum { SIDE_NAME, SIDE_DAY, SIDE_TIME, SIDE_SOURCE };
enum { OPTION_LABEL, OPTION_DETAIL };
#define CHOICE_OPTIONS 3

static const skiff_app_table_row *side_row(size_t row) {
    return &view()->tables[SIDES_TABLE].rows[row];
}

/* "18:19": two digits, a colon, two digits. */
static void check_time(const char *text) {
    TEST_ASSERT_EQUAL_size_t_MESSAGE(5, strlen(text), text);
    TEST_ASSERT_EQUAL_CHAR(':', text[2]);
}

/*
 * The choice screen (Which save?): the game in the header, one line saying what happened, both
 * sides aligned in columns, and the three buttons in the body with their symbols' actions and what
 * each does, named in the footer too.
 */
static void check_choice(const char *game, skiff_text_id situation, const skiff_text_id *labels,
                         const skiff_text_id *details) {
    static const unsigned ACTIONS[CHOICE_OPTIONS] = {SKIFF_UI_ACTION_CONFIRM, SKIFF_UI_ACTION_EXTRA,
                                                     SKIFF_UI_ACTION_BACK};
    print_view();
    const skiff_app_view *v = view();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVE_CHOICE, v->screen);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_SAVE_CHOICE), v->title);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(game, v->status, "the game is named in the header only");
    TEST_ASSERT_EQUAL_size_t(1, v->line_count);
    TEST_ASSERT_EQUAL_STRING(english(situation), v->lines[0]);
    TEST_ASSERT_EQUAL_size_t(CHOICE_TABLES, v->table_count);

    const skiff_app_table *sides = &v->tables[SIDES_TABLE];
    TEST_ASSERT_EQUAL_size_t(2, sides->row_count);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_SIDE_THIS_PSP), side_row(0)->cells[SIDE_NAME]);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_SIDE_ROMM), side_row(1)->cells[SIDE_NAME]);
    TEST_ASSERT_EQUAL_UINT(0, side_row(0)->action);
    TEST_PRINTF("both sides' dates start in one column, after the wider side's name");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sides->column_x[SIDE_NAME]);
    TEST_ASSERT_EQUAL_FLOAT(CHAR_WIDTH * (float)strlen(english(SKIFF_TEXT_SIDE_THIS_PSP)) +
                                SKIFF_APP_COLUMN_GAP,
                            sides->column_x[SIDE_DAY]);
    TEST_ASSERT_TRUE(sides->column_x[SIDE_TIME] > sides->column_x[SIDE_DAY]);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(SIDE_SOURCE, sides->dim_from, "only the source is dimmed");

    const skiff_app_table *options = &v->tables[OPTIONS_TABLE];
    TEST_ASSERT_EQUAL_size_t(CHOICE_OPTIONS, options->row_count);
    TEST_PRINTF("each button's symbol leads its row, labels and details in two columns");
    TEST_ASSERT_EQUAL_FLOAT((float)SKIFF_APP_SYMBOL_WIDTH, options->column_x[OPTION_LABEL]);
    TEST_ASSERT_EQUAL_size_t(OPTION_DETAIL, options->dim_from);
    for (size_t i = 0; i < CHOICE_OPTIONS; i++) {
        const skiff_app_table_row *row = &options->rows[i];
        TEST_ASSERT_EQUAL_UINT(ACTIONS[i], row->action);
        TEST_ASSERT_EQUAL_STRING(english(labels[i]), row->cells[OPTION_LABEL]);
        TEST_ASSERT_EQUAL_STRING(english(details[i]), row->cells[OPTION_DETAIL]);
        TEST_ASSERT_TRUE(options->column_x[OPTION_DETAIL] >=
                         options->column_x[OPTION_LABEL] +
                             CHAR_WIDTH * (float)strlen(row->cells[OPTION_LABEL]) +
                             SKIFF_APP_COLUMN_GAP);
        TEST_ASSERT_TRUE(hints(ACTIONS[i], english(labels[i])));
    }
}

/* The launch check, then the Saves screen at Ask Game and its conflict. */
static void open_the_conflict(void) {
    set_saves_scene();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    open_saves_screen();
    select_save_row("Ask Game");
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVE_CHOICE, view()->screen);
}

static void test_a_conflict_shows_both_saves_and_keep_this_psps_overwrites(void) {
    open_the_conflict();
    TEST_PRINTF("never synced here: both dates, and the device RomM's save came from");
    const skiff_text_id labels[] = {SKIFF_TEXT_KEEP_THIS_PSP, SKIFF_TEXT_USE_ROMM,
                                    SKIFF_TEXT_LATER};
    const skiff_text_id details[] = {SKIFF_TEXT_KEEP_THIS_PSP_DETAIL, SKIFF_TEXT_USE_ROMM_DETAIL,
                                     SKIFF_TEXT_LATER_DETAIL};
    check_choice("Ask Game", SKIFF_TEXT_SAVE_CONFLICT_FIRST, labels, details);
    TEST_ASSERT_TRUE(side_row(0)->cells[SIDE_DAY][0] != '\0');
    check_time(side_row(0)->cells[SIDE_TIME]);
    TEST_ASSERT_EQUAL_STRING("", side_row(0)->cells[SIDE_SOURCE]);
    TEST_ASSERT_EQUAL_STRING("11 Oct", side_row(1)->cells[SIDE_DAY]);
    TEST_ASSERT_EQUAL_STRING("18:19", side_row(1)->cells[SIDE_TIME]);
    TEST_ASSERT_EQUAL_STRING(OTHER_DEVICE_NAME, side_row(1)->cells[SIDE_SOURCE]);
    TEST_PRINTF("Later: nothing changes");
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    run_saves();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVE_CHOICE, view()->screen);
    TEST_PRINTF("Keep this PSP's: the one upload that replaces RomM's save");
    serve_upload(ROM_ASK, scene.ask_hash, "true", SAVE_KEPT);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    char path[512];
    upload_path(ROM_ASK, scene.ask_hash, "true", path, sizeof path);
    TEST_ASSERT_TRUE(requested_path(path));
    upload_path(ROM_ASK, scene.ask_hash, "false", path, sizeof path);
    TEST_ASSERT_FALSE(requested_path(path));
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    TEST_ASSERT_EQUAL_STRING("Synced: Ask Game", view()->status);
    skiff_saves_baseline *baseline = NULL;
    read_baseline(&baseline);
    const skiff_saves_baseline_entry *ask = skiff_saves_baseline_find(baseline, GAME_ASK);
    TEST_ASSERT_NOT_NULL(ask);
    TEST_ASSERT_EQUAL_UINT64(SAVE_KEPT, ask->server_save_id);
    TEST_ASSERT_EQUAL_STRING(scene.ask_hash, ask->local_hash);
    skiff_saves_baseline_destroy(baseline);
}

static void test_use_romms_restores_it_after_a_backup(void) {
    open_the_conflict();
    serve_content_of(SAVE_ASK, scene.ask_unit, scene.ask_size);
    serve_downloaded(SAVE_ASK, ROM_ASK, scene.ask_server_hash);
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    char data[64];
    read_save(GAME_ASK, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING(ASK_SERVER, data);
    TEST_ASSERT_EQUAL_STRING("Synced: Ask Game", view()->status);
    TEST_PRINTF("this PSP's save went to a backup first, listed on the Backups screen");
    char backup[TEMP_DIR_PATH_MAX];
    app_file("saves-backup/" GAME_ASK "/1.zip", backup, sizeof backup);
    FILE *file = fopen(backup, "rb");
    TEST_ASSERT_NOT_NULL(file);
    fclose(file);
    wait_ms(SKIFF_APP_NOTE_MS);
    select_save_row("Ask Game");
    frame(SKIFF_UI_ACTION_MENU);
    run_saves();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVE_BACKUPS, view()->screen);
    TEST_ASSERT_EQUAL_STRING("Ask Game", view()->status);
    TEST_ASSERT_EQUAL_size_t(1, view()->row_count);
    TEST_ASSERT_EQUAL_STRING("Backup 1", view()->rows[0].label);
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_RESTORE)));
    TEST_PRINTF("restoring it asks first, then puts this PSP's run back");
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_CONFIRM, view()->screen);
    TEST_ASSERT_TRUE(shows("Restore this backup?"));
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVE_BACKUPS, view()->screen);
    frame(SKIFF_UI_ACTION_CONFIRM);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    read_save(GAME_ASK, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING(ASK_LOCAL, data);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    TEST_PRINTF("checked again since: changed on this PSP, which the next sync uploads");
    select_save_row("Ask Game");
    TEST_ASSERT_TRUE(shows("Changed on this PSP"));
}

static void test_romms_409_turns_an_upload_into_a_question(void) {
    put_save(GAME_UP, "an older run");
    char old_hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, old_hash);
    record_saves_baseline(GAME_UP, ROM_UP, SAVE_UP, old_hash, old_hash);
    put_save(GAME_UP, UP_LOCAL);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, hash);
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, ROM_UP, "Up Game", GAME_UP);
    serve_saves_list(item, 1);
    char ops[OPS_MAX];
    TEST_PRINTF("negotiate shows RomM's save as it was synced: an upload of the local change");
    sync_op(ops, sizeof ops, "upload", ROM_UP, SAVE_UP, old_hash, APP_SAVES_SLOT);
    serve_negotiate(ops)->max_uses = 1;
    char path[512];
    upload_path(ROM_UP, hash, "false", path, sizeof path);
    serve_raw(path, "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\n\r\n"
                    "{\"detail\":\"Slot has a newer save since your last sync\"}");
    TEST_PRINTF("but another device saved meanwhile: RomM refuses, and negotiating again shows it");
    sync_op(ops, sizeof ops, "conflict", ROM_UP, SAVE_KEPT, OTHER_HASH, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_device_of(SAVE_KEPT, ROM_UP, OTHER_HASH);
    serve_complete();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    TEST_ASSERT_TRUE(requested_path(path));
    open_saves_screen();
    TEST_ASSERT_TRUE(shows("Changed on both"));
    char data[64];
    read_save(GAME_UP, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(UP_LOCAL, data, "nothing was overwritten either way");
    TEST_PRINTF("Sync asks: both changed since the last sync");
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    const skiff_text_id labels[] = {SKIFF_TEXT_KEEP_THIS_PSP, SKIFF_TEXT_USE_ROMM,
                                    SKIFF_TEXT_LATER};
    const skiff_text_id details[] = {SKIFF_TEXT_KEEP_THIS_PSP_DETAIL, SKIFF_TEXT_USE_ROMM_DETAIL,
                                     SKIFF_TEXT_LATER_DETAIL};
    check_choice("Up Game", SKIFF_TEXT_SAVE_CONFLICT, labels, details);
    skiff_saves_baseline *baseline = NULL;
    read_baseline(&baseline);
    TEST_ASSERT_EQUAL_STRING(old_hash, skiff_saves_baseline_find(baseline, GAME_UP)->server_hash);
    skiff_saves_baseline_destroy(baseline);
}

static void test_a_save_deleted_on_this_psp_asks_and_can_be_left_deleted(void) {
    put_save(GAME_UP, UP_LOCAL);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, hash);
    record_saves_baseline(GAME_UP, ROM_UP, SAVE_UP, hash, hash);
    char folder[SAVE_PATH_MAX];
    snprintf(folder, sizeof folder, "%s/" GAME_UP DATA_FOLDER "/DATA.BIN", roots.saves);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(posix, folder));
    snprintf(folder, sizeof folder, "%s/" GAME_UP DATA_FOLDER, roots.saves);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_rmdir(posix, folder));
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, ROM_UP, "Up Game", GAME_UP);
    serve_saves_list(item, 1);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "download", ROM_UP, SAVE_UP, hash, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_complete();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    TEST_PRINTF("deleted here since the last sync: never downloaded back on its own");
    TEST_ASSERT_EQUAL_STRING("Saves: 0 synced, 1 need you", view()->status);
    TEST_ASSERT_FALSE(requested_prefix("/api/saves/201/content"));
    open_saves_screen();
    TEST_ASSERT_TRUE(shows("Deleted on this PSP"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    const skiff_text_id labels[] = {SKIFF_TEXT_DOWNLOAD_AGAIN, SKIFF_TEXT_LEAVE_IT,
                                    SKIFF_TEXT_LATER};
    const skiff_text_id details[] = {SKIFF_TEXT_DOWNLOAD_AGAIN_DETAIL, SKIFF_TEXT_LEAVE_IT_DETAIL,
                                     SKIFF_TEXT_LATER_DETAIL};
    check_choice("Up Game", SKIFF_TEXT_SAVE_DELETED_LOCAL_CHOICE, labels, details);
    TEST_PRINTF("this PSP's side has no save: said in its date's place");
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_SAVE_GONE), side_row(0)->cells[SIDE_DAY]);
    TEST_ASSERT_EQUAL_STRING("", side_row(0)->cells[SIDE_TIME]);
    TEST_ASSERT_TRUE(side_row(1)->cells[SIDE_DAY][0] != '\0');
    TEST_PRINTF("Leave it: remembered, so the next sync leaves it too");
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    TEST_ASSERT_TRUE(shows("Synced"));
    skiff_saves_baseline *baseline = NULL;
    read_baseline(&baseline);
    TEST_ASSERT_EQUAL_STRING(SKIFF_SAVES_EMPTY_HASH,
                             skiff_saves_baseline_find(baseline, GAME_UP)->local_hash);
    skiff_saves_baseline_destroy(baseline);
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    TEST_ASSERT_FALSE(requested_prefix("/api/saves/201/content"));
    TEST_ASSERT_TRUE(shows("Synced"));
}

static void test_without_the_save_scopes_saves_offer_pair_again(void) {
    put_save(GAME_DOWN, DOWN_OLD);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_DOWN, hash);
    record_saves_baseline(GAME_DOWN, ROM_DOWN, SAVE_DOWN, hash, hash);
    open_saves_library(BROWSE_SCOPES, SAVES_ROMM);
    TEST_PRINTF("an older token: the launch says so once, and RomM is not asked");
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_SAVES_PAIR_NOTE), view()->status);
    TEST_ASSERT_FALSE(requested_prefix("/api/sync"));
    TEST_ASSERT_FALSE(requested_prefix(SAVES_LIST_PATH));
    open_saves_screen();
    TEST_ASSERT_TRUE(shows("Pair again to sync saves with RomM"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_PAIR_AGAIN)));
    TEST_ASSERT_TRUE(view()->has_list);
    TEST_ASSERT_EQUAL_size_t(0, view()->row_count);
    TEST_PRINTF("Pair again forgets the baselines (perhaps another RomM user), not the saves");
    skiff_saves_baseline *baseline = NULL;
    read_baseline(&baseline);
    TEST_ASSERT_EQUAL_size_t(1, baseline->count);
    skiff_saves_baseline_destroy(baseline);
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_TOKEN, "romm/device-token.http"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_PAIR);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    read_baseline(&baseline);
    TEST_ASSERT_EQUAL_size_t(0, baseline->count);
    skiff_saves_baseline_destroy(baseline);
    char data[64];
    read_save(GAME_DOWN, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING(DOWN_OLD, data);
}

static void test_a_forbidden_save_call_offers_pair_again(void) {
    put_save(GAME_UP, UP_LOCAL);
    record_installed(ROM_UP, body_crc);
    serve_rom_with_save(ROM_UP, "Up Game", GAME_UP);
    serve_saves_list("", 0);
    serve_raw(NEGOTIATE_PATH, "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n\r\n"
                              "{\"detail\":\"Forbidden\"}");
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    TEST_ASSERT_TRUE(requested_path(NEGOTIATE_PATH));
    open_saves_screen();
    TEST_PRINTF("RomM refused the token: read as a missing scope, no error screen");
    TEST_ASSERT_TRUE(shows("Pair again to sync saves with RomM"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_PAIR_AGAIN)));
}

static void test_romm_5_3_browses_but_says_save_sync_needs_5_4(void) {
    put_save(GAME_UP, UP_LOCAL);
    open_saves_library(SAVE_SCOPES, "5.3.1");
    run_saves();
    TEST_ASSERT_FALSE(requested_prefix("/api/sync"));
    TEST_ASSERT_FALSE(requested_prefix(SAVES_LIST_PATH));
    open_saves_screen();
    TEST_ASSERT_TRUE(shows("Save sync needs RomM 5.4 or newer [215]"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_BACK, english(SKIFF_TEXT_BACK)));
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_EXTRA, english(SKIFF_TEXT_SYNC_ALL)));
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SETTINGS, view()->screen);
}

static void test_a_check_that_times_out_says_so_and_retries(void) {
    fake_route *list = serve_raw(SAVES_LIST_PATH, JSON_OK "{}");
    list->fail_before_response = SKIFF_ERR_NET_TIMEOUT;
    list->max_uses = 1;
    serve_saves_list("", 0);
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    open_saves_screen();
    TEST_ASSERT_TRUE(shows("[103]"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_RETRY)));
    TEST_PRINTF("Retry: the Wi-Fi is joined again first, then the check, back on the Saves screen");
    const int joins = env_state.net_starts;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    TEST_ASSERT_GREATER_THAN_INT(joins, env_state.net_starts);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    TEST_ASSERT_TRUE(shows("No saves on this PSP or in RomM yet."));
    TEST_ASSERT_TRUE(view()->has_list);
    TEST_ASSERT_EQUAL_size_t(0, view()->row_count);
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_SYNC_ALL)));
}

static void test_a_restore_cut_short_is_settled_before_anything_with_a_notice(void) {
    char work[TEMP_DIR_PATH_MAX];
    app_file("saves-work", work, sizeof work);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, work));
    char journal[TEMP_DIR_PATH_MAX];
    app_file("saves-work/journal", journal, sizeof journal);
    write_file(journal, "not a journal", strlen("not a journal"));
    write_saves_config(BROWSE_SCOPES);
    serve_library(1);
    create_app();
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_NOTICE), view()->title);
    TEST_ASSERT_TRUE(shows("[309]"));
    TEST_ASSERT_EQUAL_size_t(0, transport.request_count);
    TEST_PRINTF("OK goes on to RomM as usual");
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    FILE *file = fopen(journal, "rb");
    TEST_ASSERT_NULL_MESSAGE(file, "the journal is gone");
}

/* The library lists ROM_UP among its games: id 101 is "Game 101". */
static void open_saves_library_with_rom_up(void) {
    write_saves_config(SAVE_SCOPES);
    serve_heartbeat(SAVES_ROMM);
    serve_platforms();
    serve_list_page("", ROM_UP - 1, 0, 1, 1);
    serve_saves_list("", 0);
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    run_saves();
}

static void test_a_games_details_show_its_save_and_sync_it(void) {
    put_save(GAME_UP, UP_LOCAL);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, hash);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "upload", ROM_UP, 0, NULL, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_upload(ROM_UP, hash, "false", SAVE_UP);
    serve_complete();
    serve_rom_with_save(ROM_UP, "Game 101", GAME_UP);
    open_saves_library_with_rom_up();
    TEST_PRINTF("no ROM claims the folder yet: RomM was not asked to negotiate");
    TEST_ASSERT_FALSE(requested_path(NEGOTIATE_PATH));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_EXTRA, english(SKIFF_TEXT_SYNC_SAVE)));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_DOWNLOAD)));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_BACK, english(SKIFF_TEXT_BACK)));
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_DETAILS, view()->screen);
    TEST_ASSERT_EQUAL_STRING("Synced: Game 101", view()->status);
    TEST_ASSERT_TRUE(shows("Save: Synced 21 Sep"));
    char path[512];
    upload_path(ROM_UP, hash, "false", path, sizeof path);
    TEST_ASSERT_TRUE(requested_path(path));
}

static void test_a_details_sync_that_lands_after_leaving_does_nothing_more(void) {
    put_save(GAME_UP, UP_LOCAL);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "upload", ROM_UP, 0, NULL, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_complete();
    serve_rom_with_save(ROM_UP, "Game 101", GAME_UP);
    open_saves_library_with_rom_up();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_EXTRA);
    frame(0);
    TEST_ASSERT_TRUE(app_call_busy(app));
    TEST_PRINTF("Back while the save is checked: the check finishes, nothing is transferred");
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    env_state.hold_calls = 0;
    run_saves();
    TEST_ASSERT_TRUE(requested_path(NEGOTIATE_PATH));
    TEST_ASSERT_FALSE(requested_prefix("/api/saves?"));
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
}

static void test_a_new_server_forgets_every_baseline_and_save_id(void) {
    put_save(GAME_UP, UP_LOCAL);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, hash);
    record_saves_baseline(GAME_UP, ROM_UP, SAVE_UP, hash, hash);
    open_saves_library(BROWSE_SCOPES, "5.3.1");
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_saves_baseline_remember_rom(app->saves->baseline, ROM_UP, GAME_UP, NULL));
    serve_heartbeat("5.3.1");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    frame(SKIFF_UI_ACTION_MENU);
    frame(SKIFF_UI_ACTION_CONFIRM);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, "https://other.test");
    run_until(SKIFF_APP_SCREEN_PAIR);
    skiff_saves_baseline *baseline = NULL;
    read_baseline(&baseline);
    TEST_ASSERT_EQUAL_size_t(0, baseline->count);
    TEST_ASSERT_EQUAL_size_t(0, baseline->rom_count);
    skiff_saves_baseline_destroy(baseline);
    char data[64];
    read_save(GAME_UP, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(UP_LOCAL, data, "the saves themselves stay");
}

static void test_the_saves_row_waits_for_the_connection(void) {
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat(SAVES_ROMM);
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    create_app();
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_PRINTF("pairing, from which Settings opens before Skiff ever reached the library");
    frame(SKIFF_UI_ACTION_MENU);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SETTINGS, view()->screen);
    TEST_ASSERT_EQUAL_size_t(APP_SETTINGS_ROW_COUNT, view()->row_count);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_SAVES), view()->rows[2].label);
    TEST_ASSERT_TRUE(view()->rows[2].dim);
    TEST_PRINTF("Games from RomM: this RomM's heartbeat holds no DEVICE_INSTALL");
    TEST_ASSERT_EQUAL_STRING("Games from RomM: Turned off in RomM", view()->rows[3].label);
    TEST_ASSERT_TRUE(view()->rows[3].dim);
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SETTINGS, view()->screen);
    TEST_PRINTF("opened anyway (the platform's smoke test can): no token yet, so Pair again");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_app_open_saves(app));
    frame(0);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    TEST_ASSERT_TRUE(shows("Pair again to sync saves with RomM"));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_app_open_saves(NULL));
}

/* What the app smoke test draws: no server at all, so an empty list and why. */
static void test_the_saves_screen_before_any_server_says_why_it_is_empty(void) {
    create_app();
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_INVALID_ARG, skiff_app_open_saves(app),
                                  "not before startup");
    run_until(SKIFF_APP_SCREEN_SERVER);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_app_open_saves(app));
    frame(0);
    print_view();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    TEST_ASSERT_TRUE(shows("Saves sync once Skiff is connected to RomM."));
    TEST_ASSERT_TRUE(view()->has_list);
    TEST_ASSERT_EQUAL_size_t(0, view()->row_count);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_DIALOG_NONE, view()->dialog);
}

/* ROMs whose save IDs leave a game unsyncable, and the ISO and CSO of one game. */
#define ROM_OLD_SCAN 110U
#define ROM_TWIN_ISO 111U
#define ROM_TWIN_CSO 112U
#define ROM_PAIR_ISO 113U
#define ROM_PAIR_CSO 114U
#define SAVE_TWIN 212U
#define GAME_TWIN "SKIF00004"
#define GAME_PAIR "SKIF00005"
#define GAME_UNKNOWN "ULJM05000"

static void test_rows_say_why_a_game_cannot_sync_and_one_twin_is_chosen(void) {
    put_save(GAME_TWIN, "the twin's save");
    put_save(GAME_PAIR, "the pair's save");
    put_save(GAME_UNKNOWN, "a game RomM does not know");
    char twin_hash[HASH_TEXT_MAX];
    local_hash(GAME_TWIN, twin_hash);
    char items[RAW_MAX];
    int used = snprintf(items, sizeof items,
                        "{\"id\":%u,\"platform_id\":1,\"name\":\"Old Scan\",\"fs_name\":"
                        "\"Old Scan.pbp\",\"fs_size_bytes\":10,\"save_target\":null,"
                        "\"save_target_layout\":null},",
                        ROM_OLD_SCAN);
    const unsigned ids[] = {ROM_TWIN_ISO, ROM_TWIN_CSO, ROM_PAIR_ISO, ROM_PAIR_CSO};
    const char *const titles[] = {"Twin ISO", "Twin CSO", "Pair ISO", "Pair CSO"};
    const char *const targets[] = {GAME_TWIN, GAME_TWIN, GAME_PAIR, GAME_PAIR};
    for (size_t i = 0; i < 4; i++) {
        used +=
            saves_item(items + used, sizeof items - (size_t)used, ids[i], titles[i], targets[i]);
        items[used++] = i < 3 ? ',' : '\0';
    }
    serve_saves_list(items, 5);
    TEST_PRINTF("of the twins, only the CSO's slot holds a save: the same as this PSP's");
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "download", ROM_TWIN_CSO, SAVE_TWIN, twin_hash, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_downloaded(SAVE_TWIN, ROM_TWIN_CSO, twin_hash);
    serve_complete();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    TEST_ASSERT_TRUE_MESSAGE(requested_path("/api/saves/212/downloaded"),
                             "RomM is told this PSP holds the same save");
    skiff_saves_baseline *baseline = NULL;
    read_baseline(&baseline);
    const skiff_saves_baseline_entry *twin = skiff_saves_baseline_find(baseline, GAME_TWIN);
    TEST_ASSERT_NOT_NULL(twin);
    TEST_ASSERT_EQUAL_UINT64(ROM_TWIN_CSO, twin->rom_id);
    skiff_saves_baseline_destroy(baseline);
    open_saves_screen();
    TEST_ASSERT_EQUAL_size_t(4, view()->row_count);
    TEST_ASSERT_EQUAL_STRING("Old Scan", view()->rows[0].label);
    TEST_ASSERT_EQUAL_STRING("Can't sync", view()->rows[0].detail);
    TEST_ASSERT_TRUE(view()->rows[0].dim);
    TEST_ASSERT_EQUAL_STRING("SKIF00005", view()->rows[1].label);
    TEST_ASSERT_EQUAL_STRING("Twin CSO", view()->rows[2].label);
    TEST_ASSERT_EQUAL_STRING("Synced 21 Sep", view()->rows[2].detail);
    TEST_ASSERT_EQUAL_STRING(GAME_UNKNOWN, view()->rows[3].label);
    TEST_PRINTF("RomM scanned it before 5.4: rescan");
    TEST_ASSERT_TRUE(shows("Can't sync: RomM has no save ID for this game."));
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_MESSAGE, view()->screen);
    TEST_ASSERT_TRUE(shows("rescan the platform"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_PRINTF("two games with this save ID and saves in neither: picked from the details");
    frame(SKIFF_UI_ACTION_DOWN);
    TEST_ASSERT_TRUE(shows("Several games in RomM use this save."));
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_TRUE(shows("Open the one you play in the library"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_PRINTF("a save folder no game in RomM claims");
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_DOWN);
    TEST_ASSERT_TRUE(shows("Skiff does not know which game in RomM uses this save."));
    TEST_PRINTF("and it has no backups to list");
    frame(SKIFF_UI_ACTION_MENU);
    run_saves();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVE_BACKUPS, view()->screen);
    TEST_ASSERT_TRUE(shows("No backups of this game's saves yet."));
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_RESTORE)));
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
}

static void test_a_game_without_a_save_id_says_so_on_its_details(void) {
    serve_saves_list("", 0);
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    open_details(1);
    TEST_ASSERT_TRUE(shows("Save: no save ID in RomM"));
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_EXTRA, english(SKIFF_TEXT_SYNC_SAVE)));
    frame(SKIFF_UI_ACTION_EXTRA);
    TEST_ASSERT_FALSE(app_saves_busy(app));
}

static void test_a_save_that_fails_romms_checksum_is_never_restored(void) {
    unsigned char unit[UNIT_BYTES_MAX];
    char real_hash[HASH_TEXT_MAX];
    const size_t size = server_unit(GAME_DOWN, DOWN_SERVER, unit, sizeof unit, real_hash);
    put_save(GAME_DOWN, DOWN_OLD);
    char old_hash[HASH_TEXT_MAX];
    local_hash(GAME_DOWN, old_hash);
    record_saves_baseline(GAME_DOWN, ROM_DOWN, SAVE_UP, old_hash, old_hash);
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, ROM_DOWN, "Down Game", GAME_DOWN);
    serve_saves_list(item, 1);
    TEST_PRINTF("RomM says its save hashes to one thing; the file it sends hashes to another");
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "download", ROM_DOWN, SAVE_DOWN, OTHER_HASH, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_content_of(SAVE_DOWN, unit, size);
    serve_complete();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    TEST_ASSERT_EQUAL_STRING("Saves: 0 synced, 1 need you", view()->status);
    char data[64];
    read_save(GAME_DOWN, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING(DOWN_OLD, data);
    TEST_ASSERT_FALSE(requested_path("/api/saves/202/downloaded"));
    char work[TEMP_DIR_PATH_MAX];
    app_file("saves-work/" GAME_DOWN ".zip", work, sizeof work);
    TEST_ASSERT_NULL_MESSAGE(fopen(work, "rb"), "the download is not kept");
    open_saves_screen();
    TEST_ASSERT_TRUE(shows("[217]"));
    TEST_PRINTF("syncing it by hand says so too");
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_MESSAGE, view()->screen);
    TEST_ASSERT_TRUE(shows("does not match RomM's checksum; sync it again [217]"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
}

static void test_a_lost_connection_stops_the_batch_for_the_next_check(void) {
    put_save(GAME_UP, UP_LOCAL);
    put_save(GAME_ASK, ASK_LOCAL);
    char up[HASH_TEXT_MAX];
    char ask[HASH_TEXT_MAX];
    local_hash(GAME_UP, up);
    local_hash(GAME_ASK, ask);
    char items[RAW_MAX];
    int used = saves_item(items, sizeof items, ROM_UP, "Alpha", GAME_UP);
    items[used++] = ',';
    saves_item(items + used, sizeof items - (size_t)used, ROM_ASK, "Beta", GAME_ASK);
    serve_saves_list(items, 2);
    char ops[OPS_MAX];
    used = sync_op(ops, sizeof ops, "upload", ROM_UP, 0, NULL, APP_SAVES_SLOT);
    ops[used++] = ',';
    sync_op(ops + used, sizeof ops - (size_t)used, "upload", ROM_ASK, 0, NULL, APP_SAVES_SLOT);
    serve_negotiate(ops);
    fake_route *first = serve_upload(ROM_UP, up, "false", SAVE_UP);
    first->fail_before_response = SKIFF_ERR_NET_CONNECTION_LOST;
    serve_upload(ROM_ASK, ask, "false", SAVE_ASK);
    serve_complete();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    char path[512];
    upload_path(ROM_ASK, ask, "false", path, sizeof path);
    TEST_ASSERT_FALSE_MESSAGE(requested_path(path), "the batch stopped at the lost connection");
    TEST_ASSERT_EQUAL_STRING("Saves: 0 synced, 1 need you", view()->status);
    open_saves_screen();
    TEST_ASSERT_TRUE(shows("[111]"));
    frame(SKIFF_UI_ACTION_DOWN);
    TEST_ASSERT_TRUE(shows("Changed on this PSP"));
}

/* ---- A suspend under save sync (S1.7) ---- */

/* The PSP sleeps once a save's .part file is open: on waking, its writes fail (the Memory Stick's
 * handles are gone), as on hardware; the next .part works again. */
static int part_opens;

static int is_part(const char *path) {
    const size_t length = strlen(path);
    const size_t suffix = strlen(SKIFF_ROMM_DOWNLOAD_PART_SUFFIX);
    return length >= suffix && strcmp(path + length - suffix, SKIFF_ROMM_DOWNLOAD_PART_SUFFIX) == 0;
}

static void sleep_at_first_part(void *ctx, const char *call, const char *path) {
    (void)ctx;
    if (strcmp(call, "open") != 0 || !is_part(path)) {
        return;
    }
    part_opens++;
    if (part_opens == 1) {
        TEST_PRINTF("the PSP sleeps with %s open", path);
        env_state.suspends++;
        storage.fail_suffix = SKIFF_ROMM_DOWNLOAD_PART_SUFFIX;
        storage.write_budget = storage.bytes_written;
        storage.write_error = SKIFF_ERR_STORAGE_IO;
    } else {
        storage.write_error = SKIFF_OK;
    }
}

/* The requests made since the log held `from`, in order, each matched against path (exact) or
 * prefix: the index of the first one that is. */
static size_t first_request_from(size_t from, const char *prefix) {
    for (size_t i = from; i < transport.log_count; i++) {
        if (strncmp(transport.log[i].url + strlen(SERVER), prefix, strlen(prefix)) == 0) {
            return i;
        }
    }
    return transport.log_count;
}

static size_t count_requests(const char *prefix) {
    size_t count = 0;
    for (size_t i = 0; i < transport.log_count; i++) {
        count += strncmp(transport.log[i].url + strlen(SERVER), prefix, strlen(prefix)) == 0;
    }
    return count;
}

/* Up Game's save deleted on this PSP since it synced, RomM holding it: the launch check asks. */
static void delete_synced_save(char *hash) {
    put_save(GAME_UP, UP_LOCAL);
    local_hash(GAME_UP, hash);
    record_saves_baseline(GAME_UP, ROM_UP, SAVE_UP, hash, hash);
    char path[SAVE_PATH_MAX];
    snprintf(path, sizeof path, "%s/" GAME_UP DATA_FOLDER "/DATA.BIN", roots.saves);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(posix, path));
    snprintf(path, sizeof path, "%s/" GAME_UP DATA_FOLDER, roots.saves);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_rmdir(posix, path));
}

static void test_a_suspend_joins_the_wifi_again_before_the_next_save_call(void) {
    char hash[HASH_TEXT_MAX];
    delete_synced_save(hash);
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, ROM_UP, "Up Game", GAME_UP);
    serve_saves_list(item, 1);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "download", ROM_UP, SAVE_UP, hash, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_complete();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    open_saves_screen();
    TEST_ASSERT_TRUE(app->net_joined);

    TEST_PRINTF("the PSP sleeps on the Saves screen: the connection is given up at once");
    env_state.suspends++;
    frame(0);
    TEST_ASSERT_FALSE(app->net_joined);
    TEST_ASSERT_NULL(app->transport);
    const int joins = env_state.net_starts;
    const size_t before = transport.log_count;
    TEST_PRINTF("the player's sync joins the Wi-Fi first, then checks, then asks");
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_SAVE_CHOICE);
    TEST_ASSERT_EQUAL_INT(joins + 1, env_state.net_starts);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(before, first_request_from(before, "/api/heartbeat"),
                                     "no save call before the connection is back");
    TEST_ASSERT_TRUE(first_request_from(before, NEGOTIATE_PATH) < transport.log_count);
    print_view();
}

static void test_a_save_download_cut_by_a_suspend_runs_again_after_joining(void) {
    char hash[HASH_TEXT_MAX];
    delete_synced_save(hash);
    unsigned char unit[UNIT_BYTES_MAX];
    char server_hash[HASH_TEXT_MAX];
    const size_t unit_size = server_unit(GAME_UP, UP_LOCAL, unit, sizeof unit, server_hash);
    TEST_ASSERT_EQUAL_STRING(hash, server_hash);
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, ROM_UP, "Up Game", GAME_UP);
    serve_saves_list(item, 1);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "download", ROM_UP, SAVE_UP, hash, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_complete();
    serve_content_of(SAVE_UP, unit, unit_size);
    serve_downloaded(SAVE_UP, ROM_UP, hash);
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    open_saves_screen();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_SAVE_CHOICE);

    TEST_PRINTF("Download again; the PSP sleeps under it and its .part write fails on waking");
    part_opens = 0;
    storage.on_call = sleep_at_first_part;
    const int joins = env_state.net_starts;
    const size_t completes = count_requests(COMPLETE_PATH);
    const size_t before = transport.log_count;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_SAVES);
    run_saves();
    storage.on_call = NULL;

    TEST_PRINTF("no Memory Stick error: the Wi-Fi was joined again and the download ran again");
    TEST_ASSERT_FALSE(shows("[302]"));
    TEST_ASSERT_FALSE(shows("[402]"));
    TEST_ASSERT_EQUAL_INT(2, part_opens);
    TEST_ASSERT_EQUAL_INT(joins + 1, env_state.net_starts);
    char content[160];
    snprintf(content, sizeof content, "/api/saves/%u/content", SAVE_UP);
    const size_t cut = first_request_from(before, content);
    TEST_ASSERT_TRUE(cut < transport.log_count);
    TEST_PRINTF("after the cut transfer, the next request is the rejoin's heartbeat (S1.7 sent the "
                "session's close first and failed with 402)");
    TEST_ASSERT_EQUAL_size_t(cut + 1, first_request_from(cut + 1, "/api"));
    TEST_ASSERT_EQUAL_size_t(cut + 1, first_request_from(cut + 1, "/api/heartbeat"));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(completes, count_requests(COMPLETE_PATH),
                                     "the session the suspend cut is left for RomM to expire");
    TEST_ASSERT_TRUE(first_request_from(cut + 1, content) < transport.log_count);
    char data[64];
    read_save(GAME_UP, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING(UP_LOCAL, data);
    TEST_ASSERT_TRUE(requested_path("/api/saves/201/downloaded"));
    TEST_ASSERT_TRUE(shows("Synced"));

    static char log_text[64 * 1024];
    skiff_log_flush(app->log);
    read_app_file(SKIFF_APP_LOG_FILE_NAME, log_text, sizeof log_text);
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "across a suspend, taken as SKIFF_ERR_NET_CONNECTION_LOST"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, GAME_UP " lost to a suspend, again after joining"));
}

/* The PSP sleeps whenever a file whose path ends in sleep_suffix is opened. */
static const char *sleep_suffix;
static int sleeps;

static void sleep_at_suffix(void *ctx, const char *call, const char *path) {
    (void)ctx;
    const size_t length = strlen(path);
    const size_t suffix = strlen(sleep_suffix);
    if (strcmp(call, "open") == 0 && length >= suffix &&
        strcmp(path + length - suffix, sleep_suffix) == 0) {
        sleeps++;
        env_state.suspends++;
    }
}

static void test_a_save_download_cut_by_a_second_suspend_says_the_connection_was_lost(void) {
    char hash[HASH_TEXT_MAX];
    delete_synced_save(hash);
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, ROM_UP, "Up Game", GAME_UP);
    serve_saves_list(item, 1);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "download", ROM_UP, SAVE_UP, hash, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_complete();
    char content[160];
    snprintf(content, sizeof content,
             "/api/saves/%u/content?device_id=" SAVE_DEVICE "&optimistic=false", SAVE_UP);
    fake_route *cut = serve_raw(content, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nPK\x03\x04");
    cut->fail_mid_body = SKIFF_ERR_STORAGE_IO;
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    open_saves_screen();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_SAVE_CHOICE);
    TEST_PRINTF("every try is cut by a suspend: one more after joining, then the player is told");
    const int joins = env_state.net_starts;
    sleeps = 0;
    sleep_suffix = SKIFF_ROMM_DOWNLOAD_PART_SUFFIX;
    storage.on_call = sleep_at_suffix;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    storage.on_call = NULL;
    TEST_ASSERT_EQUAL_INT(2, sleeps);
    TEST_ASSERT_EQUAL_INT(2, cut->uses);
    TEST_ASSERT_EQUAL_INT(joins + 1, env_state.net_starts);
    TEST_ASSERT_TRUE_MESSAGE(shows("[111]"), "a lost connection, not a Memory Stick fault");
    TEST_ASSERT_FALSE(shows("[302]"));
    TEST_PRINTF("OK goes back to the Saves screen, where the game says so too");
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    TEST_ASSERT_TRUE(shows("[111]"));
}

static void test_without_a_suspend_a_memory_stick_fault_stays_one(void) {
    char hash[HASH_TEXT_MAX];
    delete_synced_save(hash);
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, ROM_UP, "Up Game", GAME_UP);
    serve_saves_list(item, 1);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "download", ROM_UP, SAVE_UP, hash, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_complete();
    char content[160];
    snprintf(content, sizeof content,
             "/api/saves/%u/content?device_id=" SAVE_DEVICE "&optimistic=false", SAVE_UP);
    fake_route *cut = serve_raw(content, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nPK\x03\x04");
    cut->fail_mid_body = SKIFF_ERR_STORAGE_IO;
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    open_saves_screen();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_SAVE_CHOICE);
    const int joins = env_state.net_starts;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows("[302]"));
    TEST_ASSERT_EQUAL_INT(1, cut->uses);
    TEST_ASSERT_EQUAL_INT(joins, env_state.net_starts);
    TEST_ASSERT_TRUE(app->net_joined);
}

static void test_a_batch_upload_cut_by_a_suspend_counts_as_a_lost_connection(void) {
    put_save(GAME_UP, UP_LOCAL);
    char up[HASH_TEXT_MAX];
    local_hash(GAME_UP, up);
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, ROM_UP, "Alpha", GAME_UP);
    serve_saves_list(item, 1);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "upload", ROM_UP, 0, NULL, APP_SAVES_SLOT);
    serve_negotiate(ops);
    fake_route *upload = serve_upload(ROM_UP, up, "false", SAVE_UP);
    TEST_PRINTF(
        "the PSP sleeps under the launch check's upload; the unit cannot be read on waking");
    upload->fail_mid_upload = SKIFF_ERR_STORAGE_IO;
    upload->fail_after_upload_bytes = 1;
    serve_complete();
    sleeps = 0;
    sleep_suffix = SKIFF_SAVES_UNIT_EXTENSION;
    storage.on_call = sleep_at_suffix;
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    storage.on_call = NULL;
    TEST_ASSERT_TRUE(sleeps > 0);
    TEST_ASSERT_EQUAL_INT(1, upload->uses);
    TEST_ASSERT_FALSE(app->net_joined);
    TEST_ASSERT_FALSE_MESSAGE(requested_path(COMPLETE_PATH),
                              "no request before the Wi-Fi is joined again");
    TEST_PRINTF("the batch never runs it again on its own: the row says the connection was lost");
    open_saves_screen();
    TEST_ASSERT_TRUE(shows("[111]"));
    TEST_ASSERT_FALSE(shows("[302]"));
}

static void test_a_table_added_to_the_view_starts_empty(void) {
    open_paired_library(1);
    app->view.table_count = 0;
    skiff_app_table *stale = &app->view.tables[0];
    stale->row_count = 2;
    stale->column_x[1] = 99.0f;
    snprintf(stale->rows[0].cells[0], sizeof stale->rows[0].cells[0], "old");
    skiff_app_table *table = app_view_add_table(app, 1);
    TEST_ASSERT_EQUAL_PTR(stale, table);
    TEST_ASSERT_EQUAL_size_t(0, table->row_count);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, table->column_x[1]);
    TEST_ASSERT_EQUAL_STRING("", table->rows[0].cells[0]);
    TEST_ASSERT_EQUAL_size_t(1, table->dim_from);
}

static void test_a_restore_that_cannot_be_finished_at_launch_stops_save_sync(void) {
    char work[TEMP_DIR_PATH_MAX];
    app_file("saves-work", work, sizeof work);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, work));
    char journal[TEMP_DIR_PATH_MAX];
    app_file("saves-work/journal", journal, sizeof journal);
    write_file(journal, "not a journal", strlen("not a journal"));
    TEST_PRINTF("the Memory Stick refuses to let the journal go");
    storage.fail_suffix = "journal";
    storage.remove_error = SKIFF_ERR_STORAGE_IO;
    put_save(GAME_UP, UP_LOCAL);
    record_installed(ROM_UP, body_crc);
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    TEST_PRINTF("browsing works; nothing about saves is asked of RomM");
    TEST_ASSERT_FALSE(requested_prefix(SAVES_LIST_PATH));
    TEST_ASSERT_FALSE(requested_prefix("/api/sync"));
    open_saves_screen();
    TEST_ASSERT_TRUE(shows("[302]"));
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_EXTRA, english(SKIFF_TEXT_SYNC_ALL)));
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    TEST_ASSERT_FALSE(requested_prefix(SAVES_LIST_PATH));
}

static void test_a_new_server_waits_until_the_old_baselines_are_gone(void) {
    put_save(GAME_UP, UP_LOCAL);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, hash);
    record_saves_baseline(GAME_UP, ROM_UP, SAVE_UP, hash, hash);
    open_saves_library(BROWSE_SCOPES, "5.3.1");
    serve_heartbeat("5.3.1");
    storage.fail_suffix = "saves.json";
    storage.remove_error = SKIFF_ERR_STORAGE_IO;
    frame(SKIFF_UI_ACTION_MENU);
    frame(SKIFF_UI_ACTION_CONFIRM);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, "https://other.test");
    frame(0);
    print_view();
    TEST_PRINTF("saves.json could not be dropped: the old server stays, said with the error");
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_MESSAGE, view()->screen);
    TEST_ASSERT_TRUE(shows("[302]"));
    char config[TEXT_MAX];
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_ASSERT_NOT_NULL(strstr(config, "url = " SERVER));
    TEST_ASSERT_NOT_NULL(strstr(config, "token = " TOKEN));
}

static void test_a_transfer_that_ends_after_leaving_keeps_the_players_screen(void) {
    put_save(GAME_UP, UP_LOCAL);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, hash);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "upload", ROM_UP, 0, NULL, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_upload(ROM_UP, hash, "false", SAVE_UP);
    serve_complete();
    serve_rom_with_save(ROM_UP, "Game 101", GAME_UP);
    open_saves_library_with_rom_up();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    frame(SKIFF_UI_ACTION_EXTRA);
    for (int i = 0; i < FRAMES_MAX && app->call.kind != CALL_SAVES_APPLY; i++) {
        frame(0);
    }
    TEST_ASSERT_EQUAL_INT(CALL_SAVES_APPLY, app->call.kind);
    env_state.hold_calls = 1;
    frame(0);
    TEST_ASSERT_TRUE(app_call_busy(app));
    TEST_PRINTF("Back while the save uploads: the upload finishes, the library stays on screen");
    frame(SKIFF_UI_ACTION_BACK);
    env_state.hold_calls = 0;
    run_saves();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    TEST_ASSERT_EQUAL_STRING("Synced: Game 101", view()->status);
    char path[512];
    upload_path(ROM_UP, hash, "false", path, sizeof path);
    TEST_ASSERT_TRUE(requested_path(path));
}

static void test_a_transfer_cut_by_a_suspend_after_leaving_keeps_the_players_screen(void) {
    put_save(GAME_UP, UP_LOCAL);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, hash);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "upload", ROM_UP, 0, NULL, APP_SAVES_SLOT);
    serve_negotiate(ops);
    fake_route *upload = serve_upload(ROM_UP, hash, "false", SAVE_UP);
    upload->fail_mid_upload = SKIFF_ERR_STORAGE_IO;
    upload->fail_after_upload_bytes = 1;
    serve_complete();
    serve_rom_with_save(ROM_UP, "Game 101", GAME_UP);
    open_saves_library_with_rom_up();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    frame(SKIFF_UI_ACTION_EXTRA);
    for (int i = 0; i < FRAMES_MAX && app->call.kind != CALL_SAVES_APPLY; i++) {
        frame(0);
    }
    TEST_ASSERT_EQUAL_INT(CALL_SAVES_APPLY, app->call.kind);
    env_state.hold_calls = 1;
    frame(0);
    TEST_PRINTF("Back while the save uploads, then the PSP sleeps under it");
    frame(SKIFF_UI_ACTION_BACK);
    env_state.suspends++;
    const int joins = env_state.net_starts;
    env_state.hold_calls = 0;
    run_saves();
    TEST_PRINTF("not run again and not brought back: the library stays, the row says why");
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    TEST_ASSERT_EQUAL_INT(1, upload->uses);
    TEST_ASSERT_EQUAL_INT(joins, env_state.net_starts);
    TEST_ASSERT_FALSE(app->net_joined);
    open_saves_screen();
    TEST_ASSERT_TRUE(shows("[111]"));
}

static void test_a_pairing_approved_across_a_suspend_still_pairs(void) {
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_TOKEN, "romm/device-token.http"));
    serve_platforms();
    serve_page(0, 3, 3);
    create_app();
    run_until(SKIFF_APP_SCREEN_PAIR);
    env_state.hold_calls = 1;
    for (int i = 0;
         i < FRAMES_MAX && !(env_state.call_pending && app->call.kind == CALL_PAIRING_POLL); i++) {
        frame(0);
    }
    TEST_ASSERT_EQUAL_INT(CALL_PAIRING_POLL, app->call.kind);
    TEST_PRINTF("RomM approves; the PSP sleeps before the poll's answer is taken");
    env_state.suspends++;
    frame(0);
    TEST_ASSERT_FALSE(app->net_joined);
    TEST_ASSERT_NOT_NULL_MESSAGE(app->transport, "the client stays until the poll is taken");
    const int joins = env_state.net_starts;
    env_state.hold_calls = 0;
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_INT_MESSAGE(joins + 1, env_state.net_starts,
                                  "the Wi-Fi is joined again before the walk's next request");
    char config[TEXT_MAX];
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_ASSERT_NOT_NULL(strstr(config, "token = " PAIRED_TOKEN));
    TEST_ASSERT_EQUAL_STRING("Bearer " PAIRED_TOKEN, env_state.spec.romm->authorization);
}

static void test_a_suspend_mid_connect_joins_again_before_the_next_step(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_library(1);
    env_state.hold_calls = 1;
    create_app();
    for (int i = 0; i < FRAMES_MAX && !(env_state.call_pending && app->call.kind == CALL_HEARTBEAT);
         i++) {
        /* Only the heartbeat is held: the free-space count at startup comes back. */
        env_state.hold_calls = !app_call_busy(app) || app->call.kind == CALL_HEARTBEAT;
        frame(0);
    }
    env_state.hold_calls = 1;
    TEST_ASSERT_EQUAL_INT(CALL_HEARTBEAT, app->call.kind);
    TEST_ASSERT_EQUAL_INT(1, env_state.net_starts);
    TEST_PRINTF("the PSP sleeps while the heartbeat is out; RomM's answer is taken after waking");
    env_state.suspends++;
    frame(0);
    env_state.hold_calls = 0;
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, env_state.net_starts,
                                  "joined again before the platform request");
    const size_t platforms = first_request_from(0, "/api/platforms");
    const size_t second_heartbeat =
        first_request_from(first_request_from(0, "/api/heartbeat") + 1, "/api/heartbeat");
    TEST_PRINTF("requests: platforms at %zu, the second heartbeat at %zu of %zu", platforms,
                second_heartbeat, transport.log_count);
    TEST_ASSERT_TRUE_MESSAGE(second_heartbeat < platforms,
                             "the walk starts again from the join, then goes on");
}

static void test_a_retry_dropped_by_a_blocked_sync_leaves_the_next_transfer_its_own(void) {
    char hash[HASH_TEXT_MAX];
    delete_synced_save(hash);
    unsigned char unit[UNIT_BYTES_MAX];
    char server_hash[HASH_TEXT_MAX];
    const size_t unit_size = server_unit(GAME_UP, UP_LOCAL, unit, sizeof unit, server_hash);
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, ROM_UP, "Up Game", GAME_UP);
    serve_saves_list(item, 1);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "download", ROM_UP, SAVE_UP, hash, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_complete();
    serve_content_of(SAVE_UP, unit, unit_size);
    serve_downloaded(SAVE_UP, ROM_UP, hash);
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    open_saves_screen();
    TEST_PRINTF("a retry waits, then save sync is blocked (RomM refused the token) and drops it");
    app->saves->apply_retry_pending = 1;
    app->saves->want_apply = 1;
    app->saves->forbidden = 1;
    frame(0);
    TEST_ASSERT_FALSE(app->saves->want_apply);
    TEST_ASSERT_FALSE_MESSAGE(app->saves->apply_retry_pending, "dropped with its transfer");
    app->saves->forbidden = 0;
    TEST_PRINTF("the player's next transfer is cut by a suspend: it still gets its retry");
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_SAVE_CHOICE);
    part_opens = 0;
    storage.on_call = sleep_at_first_part;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_SAVES);
    run_saves();
    storage.on_call = NULL;
    TEST_ASSERT_EQUAL_INT(2, part_opens);
    char data[64];
    read_save(GAME_UP, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING(UP_LOCAL, data);
    TEST_ASSERT_FALSE(shows("[111]"));
}

/* S1.7 on hardware (2026-10-12): the retry's join was taken as done at once and its request failed
 * with 102. The retried transfer waits until the join has ended, however long it takes. */
static void test_a_retried_transfer_waits_until_the_wifi_is_joined(void) {
    char hash[HASH_TEXT_MAX];
    delete_synced_save(hash);
    unsigned char unit[UNIT_BYTES_MAX];
    char server_hash[HASH_TEXT_MAX];
    const size_t unit_size = server_unit(GAME_UP, UP_LOCAL, unit, sizeof unit, server_hash);
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, ROM_UP, "Up Game", GAME_UP);
    serve_saves_list(item, 1);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "download", ROM_UP, SAVE_UP, hash, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_complete();
    serve_content_of(SAVE_UP, unit, unit_size);
    serve_downloaded(SAVE_UP, ROM_UP, hash);
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    open_saves_screen();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_SAVE_CHOICE);
    part_opens = 0;
    storage.on_call = sleep_at_first_part;
    enum { JOIN_POLLS = 40 };
    env_state.join_polls = JOIN_POLLS;
    const int joins = env_state.net_starts;
    frame(SKIFF_UI_ACTION_CONFIRM);
    for (int i = 0; i < FRAMES_MAX && env_state.net_starts == joins; i++) {
        frame(0);
    }
    TEST_ASSERT_EQUAL_INT(joins + 1, env_state.net_starts);
    TEST_PRINTF("the join started; the access point answers only after %d polls", JOIN_POLLS);
    const size_t requests = transport.log_count;
    for (int i = 0; i < JOIN_POLLS - 1; i++) {
        frame(0);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(requests, transport.log_count,
                                         "no request while the Wi-Fi is still joining");
        TEST_ASSERT_FALSE(app->net_joined);
    }
    run_until(SKIFF_APP_SCREEN_SAVES);
    run_saves();
    storage.on_call = NULL;
    TEST_ASSERT_TRUE(app->net_joined);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, part_opens, "the transfer ran again once joined");
    TEST_ASSERT_FALSE(shows("[102]"));
    char data[64];
    read_save(GAME_UP, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING(UP_LOCAL, data);
}

/* S1.7 on hardware (2026-10-12): the PSP slept while the restore extracted into its stage. On
 * waking, the stage's writes failed, and so did the settle right after (the Memory Stick was not
 * back yet), which stopped save sync for the session. The Memory Stick here stays away until the
 * test brings it back. */
static int stage_opens;

static void sleep_at_first_stage_file(void *ctx, const char *call, const char *path) {
    (void)ctx;
    if (strcmp(call, "open") != 0 || strstr(path, "/saves-work/stage/") == NULL) {
        return;
    }
    stage_opens++;
    if (stage_opens == 1) {
        TEST_PRINTF("the PSP sleeps with %s open; the Memory Stick is away on waking", path);
        env_state.suspends++;
        storage.fail_suffix = NULL;
        storage.write_budget = storage.bytes_written;
        storage.write_error = SKIFF_ERR_STORAGE_IO;
        storage.list_error = SKIFF_ERR_STORAGE_IO;
    }
}

static void memory_stick_back(void) {
    storage.write_error = SKIFF_OK;
    storage.list_error = SKIFF_OK;
}

/* Up Game deleted here, RomM's save ready for Download again, the choice on screen. */
static void open_download_again(void) {
    char hash[HASH_TEXT_MAX];
    delete_synced_save(hash);
    unsigned char unit[UNIT_BYTES_MAX];
    char server_hash[HASH_TEXT_MAX];
    const size_t unit_size = server_unit(GAME_UP, UP_LOCAL, unit, sizeof unit, server_hash);
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, ROM_UP, "Up Game", GAME_UP);
    serve_saves_list(item, 1);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "download", ROM_UP, SAVE_UP, hash, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_complete();
    serve_content_of(SAVE_UP, unit, unit_size);
    serve_downloaded(SAVE_UP, ROM_UP, hash);
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    open_saves_screen();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_SAVE_CHOICE);
}

/* Frames until the app has tried attempts settles and none runs. */
static void run_until_settles(int attempts) {
    for (int i = 0;
         i < 4 * FRAMES_MAX && !(app->saves->settle_attempts >= attempts && !app_call_busy(app));
         i++) {
        frame(0);
    }
    TEST_ASSERT_EQUAL_INT(attempts, app->saves->settle_attempts);
}

static int work_file_exists(const char *name) {
    char path[TEMP_DIR_PATH_MAX];
    app_file(name, path, sizeof path);
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        fclose(file);
    }
    return file != NULL;
}

static void test_a_restore_cut_by_a_suspend_is_settled_once_the_stick_is_back(void) {
    open_download_again();
    stage_opens = 0;
    storage.on_call = sleep_at_first_stage_file;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until_settles(1);
    TEST_PRINTF("the settle after the wake failed and one more did: save sync waits, not stopped");
    TEST_ASSERT_TRUE(app->saves->settle_pending);
    TEST_ASSERT_EQUAL_INT(SAVES_OK, app_saves_blocked(app));
    TEST_ASSERT_FALSE(shows("[302]"));
    memory_stick_back();
    run_until(SKIFF_APP_SCREEN_SAVES);
    run_saves();
    storage.on_call = NULL;
    TEST_PRINTF("settled, then the transfer ran again after the join, and restored the save");
    TEST_ASSERT_FALSE(app->saves->settle_pending);
    char content[64];
    snprintf(content, sizeof content, "/api/saves/%u/content", SAVE_UP);
    TEST_ASSERT_EQUAL_size_t(2, count_requests(content));
    char data[64];
    read_save(GAME_UP, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING(UP_LOCAL, data);
    TEST_ASSERT_TRUE(shows("Synced"));
    TEST_ASSERT_FALSE(work_file_exists("saves-work/stage"));
    TEST_ASSERT_FALSE(work_file_exists("saves-work/" GAME_UP ".zip"));
}

static void test_a_settle_that_keeps_failing_stops_save_sync_until_one_works(void) {
    open_download_again();
    stage_opens = 0;
    storage.on_call = sleep_at_first_stage_file;
    frame(SKIFF_UI_ACTION_CONFIRM);
    for (int i = 0; i < 4 * FRAMES_MAX && app_saves_blocked(app) != SAVES_UNRECOVERED; i++) {
        frame(0);
    }
    frame(0);
    TEST_PRINTF("every settle failed: save sync stops and says why, with Retry");
    TEST_ASSERT_FALSE(app->saves->settle_pending);
    TEST_ASSERT_EQUAL_INT(SAVES_UNRECOVERED, app_saves_blocked(app));
    if (view()->screen != SKIFF_APP_SCREEN_SAVES) {
        frame(SKIFF_UI_ACTION_CONFIRM);
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    print_view();
    TEST_ASSERT_TRUE(shows("[302]"));
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_CONFIRM, english(SKIFF_TEXT_RETRY)));
    char content[64];
    snprintf(content, sizeof content, "/api/saves/%u/content", SAVE_UP);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, count_requests(content),
                                     "the transfer is not run again meanwhile");
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_STORAGE_IO),
                             skiff_err_name(app->saves->recovered));
    TEST_PRINTF("a Retry whose settle cannot even start settles nothing: still stopped");
    env_state.call_start_error = SKIFF_ERR_NO_MEMORY;
    memory_stick_back();
    frame(SKIFF_UI_ACTION_CONFIRM);
    for (int i = 0; i < 4 * FRAMES_MAX && app->saves->settle_pending; i++) {
        frame(0);
    }
    TEST_ASSERT_EQUAL_INT(SAVES_UNRECOVERED, app_saves_blocked(app));
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_NO_MEMORY),
                             skiff_err_name(app->saves->recovered));
    env_state.call_start_error = SKIFF_OK;
    TEST_PRINTF("the Memory Stick is back: Retry settles it and save sync goes on");
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    storage.on_call = NULL;
    TEST_ASSERT_EQUAL_INT(SAVES_OK, app_saves_blocked(app));
    TEST_ASSERT_FALSE(shows("[302]"));
    TEST_ASSERT_FALSE(work_file_exists("saves-work/stage"));
    TEST_ASSERT_FALSE(work_file_exists("saves-work/" GAME_UP ".zip"));
}

static void test_leaving_while_a_cut_restore_settles_drops_the_automatic_retry(void) {
    open_download_again();
    stage_opens = 0;
    storage.on_call = sleep_at_first_stage_file;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until_settles(1);
    TEST_PRINTF(
        "Back while the restore waits to be settled: the settle goes on, the retry does not");
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SETTINGS, view()->screen);
    memory_stick_back();
    const int joins = env_state.net_starts;
    for (int i = 0; i < 4 * FRAMES_MAX && (app->saves->settle_pending || app_call_busy(app)); i++) {
        frame(0);
    }
    run_saves();
    storage.on_call = NULL;
    TEST_ASSERT_FALSE(app->saves->settle_pending);
    TEST_ASSERT_EQUAL_INT(SAVES_OK, app_saves_blocked(app));
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SETTINGS, view()->screen);
    TEST_ASSERT_EQUAL_INT(joins, env_state.net_starts);
    char content[64];
    snprintf(content, sizeof content, "/api/saves/%u/content", SAVE_UP);
    TEST_ASSERT_EQUAL_size_t(1, count_requests(content));
    TEST_ASSERT_FALSE(work_file_exists("saves-work/stage"));
}

static void test_a_cut_restore_is_settled_while_the_wifi_is_still_joining(void) {
    open_download_again();
    stage_opens = 0;
    storage.on_call = sleep_at_first_stage_file;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until_settles(1);
    TEST_ASSERT_TRUE(app->saves->settle_pending);
    TEST_PRINTF("the rejoin starts before the settle (the access point takes a while), coming "
                "back to the Saves screen");
    enum { JOIN_POLLS = 400 };
    env_state.join_polls = JOIN_POLLS;
    app->failed_request = REQUEST_SAVES;
    app_connect_begin(app, CONNECT_NETWORK);
    memory_stick_back();
    for (int i = 0; i < 4 * FRAMES_MAX && app->saves->settle_pending; i++) {
        frame(0);
        TEST_ASSERT_FALSE_MESSAGE(app->net_joined && app->saves->settle_pending,
                                  "the join ended before the settle ran");
    }
    TEST_PRINTF("settled while joining: no need to wait for the connection");
    TEST_ASSERT_FALSE(app->saves->settle_pending);
    TEST_ASSERT_FALSE(app->net_joined);
    TEST_ASSERT_EQUAL_INT(CONNECT_JOINING, app->connect);
    TEST_ASSERT_FALSE(work_file_exists("saves-work/stage"));
    run_until(SKIFF_APP_SCREEN_SAVES);
    run_saves();
    storage.on_call = NULL;
    char data[64];
    read_save(GAME_UP, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING(UP_LOCAL, data);
}

static void test_a_suspend_on_the_pair_screen_joins_again_and_keeps_the_code(void) {
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    fake_route *init = fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http");
    TEST_ASSERT_NOT_NULL(init);
    fake_route *pending =
        fake_transport_add_fixture(&transport, PATH_TOKEN, "romm/device-pending.http");
    TEST_ASSERT_NOT_NULL(pending);
    create_app();
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_ASSERT_TRUE(app->pairing.active);
    const int joins = env_state.net_starts;
    const int64_t started = app->pairing.started_ms;
    TEST_PRINTF("the PSP sleeps on the pair screen: the next poll waits for a join");
    env_state.suspends++;
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_ASSERT_EQUAL_INT(joins + 1, env_state.net_starts);
    TEST_ASSERT_TRUE(app->net_joined);
    TEST_ASSERT_TRUE_MESSAGE(app->pairing.active, "the same pairing goes on");
    TEST_ASSERT_EQUAL_INT64(started, app->pairing.started_ms);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, init->uses, "no new code was asked for");
    TEST_ASSERT_TRUE(shows("Code " USER_CODE));
}

static void test_a_restore_cut_mid_way_is_settled_and_stops_save_sync(void) {
    char extra[SAVE_PATH_MAX];
    snprintf(extra, sizeof extra, "%s/" GAME_ASK "EXTRA", roots.saves);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, extra));
    char file[SAVE_PATH_MAX + 16];
    snprintf(file, sizeof file, "%s/DATA.BIN", extra);
    write_file(file, "a folder RomM's save lacks", strlen("a folder RomM's save lacks"));
    open_the_conflict();
    serve_content_of(SAVE_ASK, scene.ask_unit, scene.ask_size);
    serve_downloaded(SAVE_ASK, ROM_ASK, scene.ask_server_hash);
    TEST_PRINTF("Use RomM's: its save lacks a folder, which the Memory Stick will not remove");
    storage.fail_suffix = GAME_ASK "EXTRA";
    storage.rmdir_error = SKIFF_ERR_STORAGE_IO;
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_MESSAGE, view()->screen);
    TEST_ASSERT_TRUE(shows("[302]"));
    TEST_ASSERT_FALSE(requested_path("/api/saves/203/downloaded"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_PRINTF(
        "the journal could not be settled either: nothing syncs until a launch finishes it");
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    TEST_ASSERT_TRUE(shows("[302]"));
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_EXTRA, english(SKIFF_TEXT_SYNC_ALL)));
    char journal[TEMP_DIR_PATH_MAX];
    app_file("saves-work/journal", journal, sizeof journal);
    FILE *kept = fopen(journal, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(kept, "the journal stays for the next launch");
    fclose(kept);
}

static void test_leave_it_compares_again_when_the_save_came_back(void) {
    put_save(GAME_UP, UP_LOCAL);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, hash);
    record_saves_baseline(GAME_UP, ROM_UP, SAVE_UP, hash, hash);
    char folder[SAVE_PATH_MAX];
    snprintf(folder, sizeof folder, "%s/" GAME_UP DATA_FOLDER "/DATA.BIN", roots.saves);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(posix, folder));
    char item[SAVE_JSON_MAX];
    saves_item(item, sizeof item, ROM_UP, "Up Game", GAME_UP);
    serve_saves_list(item, 1);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "download", ROM_UP, SAVE_UP, hash, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_complete();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    open_saves_screen();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVE_CHOICE, view()->screen);
    TEST_PRINTF("the save comes back (over USB) before the player answers");
    put_save(GAME_UP, UP_LOCAL);
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    skiff_saves_baseline *baseline = NULL;
    read_baseline(&baseline);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(hash, skiff_saves_baseline_find(baseline, GAME_UP)->local_hash,
                                     "never recorded as deleted while it is there");
    skiff_saves_baseline_destroy(baseline);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    TEST_ASSERT_TRUE(shows("Synced"));
}

static void test_a_save_id_learnt_from_details_reaches_saves_json(void) {
    serve_rom_with_save(ROM_UP, "Game 101", GAME_UP);
    open_saves_library_with_rom_up();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    frame(SKIFF_UI_ACTION_BACK);
    skiff_saves_baseline *baseline = NULL;
    read_baseline(&baseline);
    TEST_ASSERT_NULL_MESSAGE(skiff_saves_baseline_rom_target(baseline, ROM_UP),
                             "the details screen writes nothing itself");
    skiff_saves_baseline_destroy(baseline);
    TEST_PRINTF("the next save call writes it, though it changes nothing else");
    open_saves_screen();
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    read_baseline(&baseline);
    TEST_ASSERT_EQUAL_STRING(GAME_UP, skiff_saves_baseline_rom_target(baseline, ROM_UP));
    skiff_saves_baseline_destroy(baseline);
}

static void test_a_baseline_the_memory_stick_refused_is_written_by_the_next_call(void) {
    put_save(GAME_UP, UP_LOCAL);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, hash);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "upload", ROM_UP, 0, NULL, APP_SAVES_SLOT);
    serve_negotiate(ops)->max_uses = 1;
    serve_upload(ROM_UP, hash, "false", SAVE_UP);
    sync_op(ops, sizeof ops, "no_op", ROM_UP, SAVE_UP, hash, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_downloaded(SAVE_UP, ROM_UP, hash);
    serve_complete();
    serve_rom_with_save(ROM_UP, "Game 101", GAME_UP);
    open_saves_library_with_rom_up();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    TEST_PRINTF("the upload works, writing saves.json does not");
    storage.fail_suffix = "saves.json" SKIFF_STORAGE_DRAFT_SUFFIX;
    storage.write_budget = 0;
    storage.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    TEST_ASSERT_EQUAL_STRING("Synced: Game 101", view()->status);
    skiff_saves_baseline *baseline = NULL;
    read_baseline(&baseline);
    TEST_ASSERT_NULL(skiff_saves_baseline_find(baseline, GAME_UP));
    skiff_saves_baseline_destroy(baseline);
    TEST_PRINTF("room again: the next sync writes what the last one could not");
    storage.write_error = SKIFF_OK;
    frame(SKIFF_UI_ACTION_BACK);
    open_saves_screen();
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    read_baseline(&baseline);
    const skiff_saves_baseline_entry *entry = skiff_saves_baseline_find(baseline, GAME_UP);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_UINT64(SAVE_UP, entry->server_save_id);
    skiff_saves_baseline_destroy(baseline);
}

static void test_a_roms_details_skiff_refuses_cost_only_that_game(void) {
    put_save(GAME_DOWN, DOWN_OLD);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_DOWN, hash);
    const unsigned ids[] = {ROM_UP, ROM_DOWN};
    const uint32_t crcs[] = {body_crc, body_crc};
    record_installs(ids, crcs, 2);
    TEST_PRINTF("RomM answers one installed game's details with something Skiff refuses");
    serve_raw("/api/roms/101", JSON_OK "{\"id\":\"not a number\"}");
    serve_rom_with_save(ROM_DOWN, "Down Game", GAME_DOWN);
    serve_saves_list("", 0);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "upload", ROM_DOWN, 0, NULL, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_upload(ROM_DOWN, hash, "false", SAVE_DOWN);
    serve_complete();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    TEST_PRINTF(
        "the other game still syncs; the refused one waits, without a save ID, for Sync all");
    TEST_ASSERT_EQUAL_STRING("Saves: 1 synced, 0 need you", view()->status);
    skiff_saves_baseline *baseline = NULL;
    read_baseline(&baseline);
    TEST_ASSERT_EQUAL_STRING("", skiff_saves_baseline_rom_target(baseline, ROM_UP));
    TEST_ASSERT_EQUAL_STRING(GAME_DOWN, skiff_saves_baseline_rom_target(baseline, ROM_DOWN));
    TEST_ASSERT_NOT_NULL(skiff_saves_baseline_find(baseline, GAME_DOWN));
    skiff_saves_baseline_destroy(baseline);
}

/* A unit without files (an end record alone): what RomM holds once a save was deleted elsewhere. */
static const unsigned char EMPTY_UNIT[] = {0x50, 0x4B, 0x05, 0x06, 0, 0, 0, 0, 0, 0, 0,
                                           0,    0,    0,    0,    0, 0, 0, 0, 0, 0, 0};

static void test_a_save_deleted_in_romm_asks_and_says_removing_deletes_it_here(void) {
    put_save(GAME_UP, UP_LOCAL);
    record_installed(ROM_UP, body_crc);
    serve_rom_with_save(ROM_UP, "Up Game", GAME_UP);
    serve_saves_list("", 0);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "conflict", ROM_UP, SAVE_UP, SKIFF_SAVES_EMPTY_HASH, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_device_of(SAVE_UP, ROM_UP, SKIFF_SAVES_EMPTY_HASH);
    serve_content_of(SAVE_UP, EMPTY_UNIT, sizeof EMPTY_UNIT);
    serve_downloaded(SAVE_UP, ROM_UP, SKIFF_SAVES_EMPTY_HASH);
    serve_complete();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    TEST_PRINTF("RomM's save is an empty unit: never applied on its own");
    TEST_ASSERT_EQUAL_STRING("Saves: 0 synced, 1 need you", view()->status);
    TEST_ASSERT_FALSE(requested_prefix("/api/saves/201/content"));
    open_saves_screen();
    TEST_ASSERT_TRUE(shows("Deleted in RomM"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    TEST_PRINTF("taking RomM's side is named for what it does: this PSP's goes, after a backup");
    const skiff_text_id labels[] = {SKIFF_TEXT_KEEP_THIS_PSP, SKIFF_TEXT_REMOVE_THIS_PSP,
                                    SKIFF_TEXT_LATER};
    const skiff_text_id details[] = {SKIFF_TEXT_UPLOAD_AGAIN_DETAIL,
                                     SKIFF_TEXT_REMOVE_THIS_PSP_DETAIL, SKIFF_TEXT_LATER_DETAIL};
    check_choice(GAME_UP, SKIFF_TEXT_SAVE_DELETED_SERVER_CHOICE, labels, details);
    TEST_ASSERT_TRUE(shows("after a backup"));
    TEST_ASSERT_FALSE(hints(SKIFF_UI_ACTION_EXTRA, english(SKIFF_TEXT_USE_ROMM)));
    TEST_ASSERT_TRUE(side_row(0)->cells[SIDE_DAY][0] != '\0');
    check_time(side_row(0)->cells[SIDE_TIME]);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_SAVE_GONE), side_row(1)->cells[SIDE_DAY]);
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    char data[64];
    read_save(GAME_UP, data, sizeof data);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", data, "the save is removed here");
    char backup[TEMP_DIR_PATH_MAX];
    app_file("saves-backup/" GAME_UP "/1.zip", backup, sizeof backup);
    FILE *file = fopen(backup, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, "after a backup");
    fclose(file);
    TEST_ASSERT_EQUAL_STRING("Synced: " GAME_UP, view()->status);
}

/* PSP ROMs with saves, ids from first_id, on the list's page at offset of a list of total. */
static void serve_saves_page(unsigned offset, unsigned first_id, unsigned count, unsigned total) {
    static char raw[RAW_MAX];
    int used = snprintf(raw, sizeof raw, JSON_OK "{\"items\":[");
    for (unsigned i = 0; i < count; i++) {
        char title[32];
        char target[SKIFF_SAVES_TARGET_SIZE];
        snprintf(title, sizeof title, "Many %03u", first_id + i);
        snprintf(target, sizeof target, "SKIM%05u", first_id + i);
        used += snprintf(raw + used, sizeof raw - (size_t)used, i > 0 ? "," : "");
        used += saves_item(raw + used, sizeof raw - (size_t)used, first_id + i, title, target);
    }
    snprintf(raw + used, sizeof raw - (size_t)used, "],\"total\":%u,\"limit\":%d,\"offset\":%u}",
             total, SKIFF_ROMM_PAGE_SIZE, offset);
    char path[256];
    snprintf(path, sizeof path,
             "/api/roms?platform_ids=1&limit=25&offset=%u" LIST_QUERY "&has_saves=true", offset);
    serve_raw(path, raw);
}

#define MANY_FIRST_ID 300U
#define MANY_GAMES 33U

static void test_many_games_are_negotiated_in_rounds(void) {
    serve_saves_page(0, MANY_FIRST_ID, SKIFF_ROMM_PAGE_SIZE, MANY_GAMES);
    serve_saves_page(SKIFF_ROMM_PAGE_SIZE, MANY_FIRST_ID + SKIFF_ROMM_PAGE_SIZE,
                     MANY_GAMES - SKIFF_ROMM_PAGE_SIZE, MANY_GAMES);
    serve_negotiate("");
    serve_complete();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    size_t negotiations = 0;
    size_t first_round = 0;
    for (size_t i = 0; i < transport.log_count; i++) {
        const fake_request *request = &transport.log[i];
        if (strcmp(request->url + strlen(SERVER), NEGOTIATE_PATH) != 0) {
            continue;
        }
        size_t ids = 0;
        const char *list = strstr(request->body, "\"rom_ids\":[");
        for (const char *c = list; c != NULL && *c != ']'; c++) {
            ids += *c == ',';
        }
        TEST_PRINTF("negotiate %zu: %zu ROM(s)", negotiations + 1, list != NULL ? ids + 1 : 0);
        if (negotiations == 0) {
            first_round = ids + 1;
        }
        negotiations++;
    }
    TEST_PRINTF(
        "33 games with saves: two rounds of at most 32 ROMs, the first one's session closed");
    TEST_ASSERT_EQUAL_size_t(2, negotiations);
    TEST_ASSERT_EQUAL_size_t(APP_SAVES_NEGOTIATE_ROMS, first_round);
    TEST_ASSERT_TRUE(requested_path(COMPLETE_PATH));
}

static void test_sync_all_asks_again_about_a_game_once_without_a_save_id(void) {
    put_save(GAME_UP, UP_LOCAL);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, hash);
    record_installed(ROM_UP, body_crc);
    TEST_PRINTF("RomM has no save ID for the installed game yet");
    char raw[SAVE_JSON_MAX];
    snprintf(raw, sizeof raw,
             JSON_OK
             "{\"id\":101,\"platform_id\":1,\"name\":\"Up Game\",\"fs_name\":\"Up Game.iso\","
             "\"fs_size_bytes\":%u,\"save_target\":null,\"save_target_layout\":null,"
             "\"files\":[{\"rom_id\":101,\"file_name\":\"Up Game.iso\",\"file_size_bytes\":%u}]}",
             BODY_BYTES, BODY_BYTES);
    serve_raw("/api/roms/101", raw)->max_uses = 1;
    serve_rom_with_save(ROM_UP, "Up Game", GAME_UP);
    serve_saves_list("", 0);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "upload", ROM_UP, 0, NULL, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_upload(ROM_UP, hash, "false", SAVE_UP);
    serve_complete();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    TEST_ASSERT_FALSE(requested_path(NEGOTIATE_PATH));
    open_saves_screen();
    TEST_ASSERT_TRUE(shows("Can't sync"));
    TEST_PRINTF("rescanned in RomM since: Sync all asks again and syncs it");
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    char path[512];
    upload_path(ROM_UP, hash, "false", path, sizeof path);
    TEST_ASSERT_TRUE(requested_path(path));
    TEST_ASSERT_TRUE(shows("Synced"));
}

#define MANY_INSTALLED_FIRST 400U
#define MANY_INSTALLED (APP_SAVES_LOOKUPS_MAX + 1)

static void test_installed_games_past_the_lookup_budget_are_reached_next_time(void) {
    unsigned ids[MANY_INSTALLED];
    uint32_t crcs[MANY_INSTALLED];
    for (unsigned i = 0; i < MANY_INSTALLED; i++) {
        ids[i] = MANY_INSTALLED_FIRST + i;
        crcs[i] = body_crc;
    }
    record_installs(ids, crcs, MANY_INSTALLED);
    serve_saves_list("", 0);
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    TEST_PRINTF("RomM has none of them: a check asks about %d, remembering each",
                APP_SAVES_LOOKUPS_MAX);
    skiff_saves_baseline *baseline = NULL;
    read_baseline(&baseline);
    TEST_ASSERT_EQUAL_STRING("", skiff_saves_baseline_rom_target(baseline, MANY_INSTALLED_FIRST));
    TEST_ASSERT_NULL(
        skiff_saves_baseline_rom_target(baseline, MANY_INSTALLED_FIRST + APP_SAVES_LOOKUPS_MAX));
    skiff_saves_baseline_destroy(baseline);
    TEST_PRINTF("the next check asks about the one never asked first");
    open_saves_screen();
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    read_baseline(&baseline);
    TEST_ASSERT_EQUAL_STRING("", skiff_saves_baseline_rom_target(
                                     baseline, MANY_INSTALLED_FIRST + APP_SAVES_LOOKUPS_MAX));
    skiff_saves_baseline_destroy(baseline);
    TEST_ASSERT_EQUAL_size_t(APP_SAVES_LOOKUPS_MAX - 1, app->saves->lookup_cursor);
}

static void test_another_rom_with_the_same_save_id_starts_as_a_first_sync(void) {
    put_save(GAME_UP, UP_LOCAL);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, hash);
    TEST_PRINTF("this PSP's save last synced with the other copy of the game (ROM 102)");
    record_saves_baseline(GAME_UP, ROM_DOWN, SAVE_DOWN, hash, hash);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "no_op", ROM_DOWN, SAVE_DOWN, hash, APP_SAVES_SLOT);
    serve_negotiate(ops)->max_uses = 1;
    sync_op(ops, sizeof ops, "conflict", ROM_UP, SAVE_UP, OTHER_HASH, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_device_of(SAVE_UP, ROM_UP, OTHER_HASH);
    serve_complete();
    serve_rom_with_save(ROM_UP, "Game 101", GAME_UP);
    open_saves_library_with_rom_up();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    TEST_PRINTF("synced from ROM 101, whose RomM save differs: asked, never downloaded");
    frame(SKIFF_UI_ACTION_EXTRA);
    run_saves();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVE_CHOICE, view()->screen);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_SAVE_CONFLICT_FIRST)));
    TEST_ASSERT_FALSE(requested_prefix("/api/saves/201/content"));
}

/* ---- The header, and what needs the player ---- */

#define LONG_GAME "Pinball Hall of Fame - The Gottlieb Collection"
#define ZED_GAME "Zed Game"

/* The width a status may take beside title with the fake measure, as skiff/app.h lays the header.
 */
static float expected_status_width(const char *title) {
    const float title_width = CHAR_WIDTH * (float)strlen(title) * SKIFF_APP_TITLE_SCALE;
    return (SKIFF_APP_TEXT_WIDTH - title_width - SKIFF_APP_HEADER_GAP) / SKIFF_APP_STATUS_SCALE;
}

static void test_the_status_takes_the_width_the_title_leaves(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_library(3);
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    const float width = app_view_status_width(app);
    TEST_PRINTF("beside \"%s\": %d units for the status", view()->title, (int)width);
    TEST_ASSERT_EQUAL_FLOAT(expected_status_width(english(SKIFF_TEXT_TITLE_LIBRARY)), width);
    TEST_ASSERT_TRUE_MESSAGE(width > 400.0f, "a short title leaves the status most of the line");
    TEST_PRINTF("a note with a long name: the whole of it, past the old 230 units");
    app_note_named(app, SKIFF_TEXT_QUEUE_ADDED, LONG_GAME);
    frame(0);
    print_view();
    TEST_ASSERT_EQUAL_STRING("Added to downloads: " LONG_GAME, view()->status);
    TEST_PRINTF("a title too long for the line still leaves the status its minimum");
    snprintf(app->view.title, sizeof app->view.title, "%s", LONG_GAME);
    TEST_ASSERT_EQUAL_FLOAT(SKIFF_APP_STATUS_MIN_WIDTH, app_view_status_width(app));
}

static void test_a_long_name_is_shortened_so_the_action_stays(void) {
    write_config("[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN "\n[network]\nprofile = 1\n");
    serve_library(3);
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    const float width = 20.0f * CHAR_WIDTH;
    char out[SKIFF_APP_STATUS_MAX];
    app_format_fit_name(app, SKIFF_TEXT_SAVE_SYNCED_NOTE, LONG_GAME, width, out, sizeof out);
    TEST_PRINTF("in %d units: \"%s\"", (int)width, out);
    TEST_ASSERT_EQUAL_STRING("Synced: Pinball H...", out);
    app_format_fit_name(app, SKIFF_TEXT_SAVES_SYNCING, LONG_GAME, width, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("Syncing: Pinball...", out);
    TEST_PRINTF("a name that fits stays whole");
    app_format_fit_name(app, SKIFF_TEXT_SAVE_IN_SYNC_NOTE, "Lumines", 200.0f, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("Already in sync: Lumines", out);
    TEST_PRINTF("no room for even part of the name: the text is cut as a whole");
    app_format_fit_name(app, SKIFF_TEXT_SAVE_IN_SYNC_NOTE, LONG_GAME, 10.0f * CHAR_WIDTH, out,
                        sizeof out);
    TEST_ASSERT_EQUAL_STRING("Already...", out);
    TEST_PRINTF("in Spanish, the action first too");
    app->config.language = SKIFF_LANGUAGE_SPANISH;
    app_format_fit_name(app, SKIFF_TEXT_SAVE_SYNCED_NOTE, LONG_GAME, 30.0f * CHAR_WIDTH, out,
                        sizeof out);
    TEST_PRINTF("es: \"%s\"", out);
    TEST_ASSERT_EQUAL_STRING("Sincronizada: Pinball Hall...", out);
    app_format_fit_name(app, SKIFF_TEXT_SAVES_SYNCING, LONG_GAME, 30.0f * CHAR_WIDTH, out,
                        sizeof out);
    TEST_ASSERT_EQUAL_STRING("Sincronizando: Pinball Hall...", out);
    app->config.language = SKIFF_LANGUAGE_ENGLISH;
}

/* The header's text is the start of the action and name given, cut to fit beside the title. */
static void check_header_cut(const char *start) {
    const char *status = view()->status;
    TEST_ASSERT_EQUAL_STRING_LEN(start, status, strlen(start));
    TEST_ASSERT_TRUE(CHAR_WIDTH * (float)strlen(status) <= app_view_status_width(app));
    TEST_ASSERT_EQUAL_STRING(SKIFF_UI_ELLIPSIS,
                             status + strlen(status) - strlen(SKIFF_UI_ELLIPSIS));
}

static void test_a_long_names_sync_note_says_synced_first(void) {
    put_save(GAME_UP, UP_LOCAL);
    char hash[HASH_TEXT_MAX];
    local_hash(GAME_UP, hash);
    char ops[OPS_MAX];
    sync_op(ops, sizeof ops, "upload", ROM_UP, 0, NULL, APP_SAVES_SLOT);
    serve_negotiate(ops);
    serve_upload(ROM_UP, hash, "false", SAVE_UP);
    serve_complete();
    serve_rom_with_save(ROM_UP, LONG_GAME LONG_GAME, GAME_UP);
    open_saves_library_with_rom_up();
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_DETAILS);
    frame(SKIFF_UI_ACTION_EXTRA);
    TEST_PRINTF("while the upload runs: \"Syncing:\" first, the name cut");
    for (int i = 0; i < FRAMES_MAX && app->call.kind != CALL_SAVES_APPLY; i++) {
        frame(0);
    }
    print_view();
    TEST_ASSERT_EQUAL_INT(CALL_SAVES_APPLY, app->call.kind);
    check_header_cut("Syncing: " LONG_GAME);
    run_saves();
    TEST_PRINTF("then \"Synced:\" first, the name cut");
    check_header_cut("Synced: " LONG_GAME);
}

/* The library's Settings hint: plain, or with the saves that need the player. */
static int settings_hint(const char *label) { return hints(SKIFF_UI_ACTION_MENU, label); }

/* Settings' Saves row (its third). */
static const skiff_app_row *saves_row(void) { return &view()->rows[2]; }

static void test_saves_that_need_the_player_are_flagged_until_chosen(void) {
    set_saves_scene_titled(ZED_GAME);
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    TEST_ASSERT_EQUAL_STRING("Saves: 2 synced, 1 need you", view()->status);
    wait_ms(SKIFF_APP_NOTE_MS);
    TEST_PRINTF("after the launch note: the library's Settings hint keeps the count");
    print_view();
    TEST_ASSERT_TRUE(settings_hint("Settings (1)"));
    TEST_ASSERT_EQUAL_size_t(1, app_saves_waiting(app));

    TEST_PRINTF("Settings' Saves row says it, in the colour that asks for attention");
    frame(SKIFF_UI_ACTION_MENU);
    print_view();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SETTINGS, view()->screen);
    TEST_ASSERT_EQUAL_STRING(english(SKIFF_TEXT_TITLE_SAVES), saves_row()->label);
    TEST_ASSERT_EQUAL_STRING("1 needs you", saves_row()->detail);
    TEST_ASSERT_TRUE(saves_row()->attention);
    TEST_ASSERT_FALSE(view()->rows[0].attention);

    TEST_PRINTF("the Saves screen lists it first, marked, though its name sorts last");
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_CONFIRM);
    print_view();
    TEST_ASSERT_EQUAL_size_t(3, view()->row_count);
    TEST_ASSERT_EQUAL_STRING(ZED_GAME, view()->rows[0].label);
    TEST_ASSERT_TRUE(view()->rows[0].attention);
    TEST_ASSERT_EQUAL_STRING("Down Game", view()->rows[1].label);
    TEST_ASSERT_FALSE(view()->rows[1].attention);
    TEST_ASSERT_EQUAL_STRING(GAME_UP, view()->rows[2].label);
    TEST_ASSERT_FALSE(view()->rows[2].attention);

    TEST_PRINTF("two waiting: the plural");
    app_save_game *down = &app->saves->shown->games[1];
    const skiff_saves_decision decision = down->decision;
    down->decision.action = SKIFF_SAVES_ASK_CONFLICT;
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_STRING("2 need you", saves_row()->detail);
    app->config.language = SKIFF_LANGUAGE_SPANISH;
    app->dirty = 1;
    frame(0);
    print_view();
    TEST_ASSERT_EQUAL_STRING("2 te necesitan", saves_row()->detail);
    down->decision = decision;
    app->dirty = 1;
    frame(0);
    TEST_ASSERT_EQUAL_STRING("1 te necesita", saves_row()->detail);
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_TRUE(settings_hint("Ajustes (1)"));
    app->config.language = SKIFF_LANGUAGE_ENGLISH;

    TEST_PRINTF("chosen: Keep this PSP's, and nothing is flagged any more");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_app_open_saves(app));
    frame(0);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVES, view()->screen);
    select_save_row(ZED_GAME);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SAVE_CHOICE, view()->screen);
    serve_upload(ROM_ASK, scene.ask_hash, "true", SAVE_KEPT);
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_saves();
    TEST_ASSERT_EQUAL_STRING("Synced: " ZED_GAME, view()->status);
    TEST_ASSERT_FALSE(view()->rows[0].attention);
    TEST_ASSERT_EQUAL_size_t(0, app_saves_waiting(app));
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_EQUAL_STRING("", saves_row()->detail);
    TEST_ASSERT_FALSE(saves_row()->attention);
    frame(SKIFF_UI_ACTION_BACK);
    TEST_ASSERT_TRUE(settings_hint(english(SKIFF_TEXT_SETTINGS)));
}

/* No cell of the view's tables was cut to fit. */
static void check_tables_whole(void) {
    const skiff_app_view *v = view();
    for (size_t t = 0; t < v->table_count; t++) {
        for (size_t r = 0; r < v->tables[t].row_count; r++) {
            for (size_t c = 0; c < SKIFF_APP_TABLE_COLUMNS; c++) {
                const char *cell = v->tables[t].rows[r].cells[c];
                TEST_ASSERT_NULL_MESSAGE(strstr(cell, SKIFF_UI_ELLIPSIS), cell);
                TEST_ASSERT_TRUE(v->tables[t].column_x[c] + CHAR_WIDTH * (float)strlen(cell) <=
                                 SKIFF_APP_TEXT_WIDTH);
            }
        }
    }
}

/* The choice on screen, as the deletion action would have it. */
static void saves_choice_variant(skiff_saves_action action) {
    app->saves->choice.decision.action = action;
    app->dirty = 1;
    frame(0);
    print_view();
}

static void test_the_choice_fits_in_spanish(void) {
    open_the_conflict();
    app->config.language = SKIFF_LANGUAGE_SPANISH;
    app->dirty = 1;
    frame(0);
    print_view();
    TEST_ASSERT_EQUAL_STRING("¿Qué partida?", view()->title);
    TEST_ASSERT_EQUAL_STRING("Ask Game", view()->status);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, view()->line_count, "what happened takes one line");
    TEST_ASSERT_EQUAL_STRING("Esta PSP", side_row(0)->cells[SIDE_NAME]);
    TEST_ASSERT_EQUAL_STRING("11 oct", side_row(1)->cells[SIDE_DAY]);
    TEST_ASSERT_EQUAL_STRING("Usar la de la PSP", view()->tables[OPTIONS_TABLE].rows[0].cells[0]);
    check_tables_whole();
    TEST_PRINTF("and when RomM deleted it: the longest label");
    saves_choice_variant(SKIFF_SAVES_ASK_DELETED_SERVER);
    TEST_ASSERT_EQUAL_STRING("Borrar la de la PSP", view()->tables[OPTIONS_TABLE].rows[1].cells[0]);
    TEST_ASSERT_EQUAL_STRING("Se borra aquí, tras una copia",
                             view()->tables[OPTIONS_TABLE].rows[1].cells[1]);
    check_tables_whole();
    saves_choice_variant(SKIFF_SAVES_ASK_DELETED_LOCAL);
    check_tables_whole();
    app->config.language = SKIFF_LANGUAGE_ENGLISH;
    saves_choice_variant(SKIFF_SAVES_ASK_DELETED_LOCAL);
    check_tables_whole();
    TEST_PRINTF("a device name longer than its column is cut there, between characters");
    snprintf(app->saves->choice.server_device, sizeof app->saves->choice.server_device, "%s",
             "Teléfono de María, salón " LONG_GAME LONG_GAME);
    saves_choice_variant(SKIFF_SAVES_ASK_CONFLICT);
    const char *source = side_row(1)->cells[SIDE_SOURCE];
    const float column = view()->tables[SIDES_TABLE].column_x[SIDE_SOURCE];
    TEST_ASSERT_EQUAL_STRING_LEN("Teléfono de María", source, strlen("Teléfono de María"));
    TEST_ASSERT_EQUAL_STRING(SKIFF_UI_ELLIPSIS,
                             source + strlen(source) - strlen(SKIFF_UI_ELLIPSIS));
    TEST_ASSERT_TRUE(column + CHAR_WIDTH * (float)strlen(source) <= SKIFF_APP_TEXT_WIDTH);
}

#define SHIFTED_FIRST_ID 500U

static void test_a_rom_read_twice_across_shifted_pages_is_one_game(void) {
    TEST_PRINTF("a save uploaded while the list is read shifts ROM 524 onto the next page too");
    serve_saves_page(0, SHIFTED_FIRST_ID, SKIFF_ROMM_PAGE_SIZE, SKIFF_ROMM_PAGE_SIZE + 2);
    serve_saves_page(SKIFF_ROMM_PAGE_SIZE, SHIFTED_FIRST_ID + SKIFF_ROMM_PAGE_SIZE - 1, 2,
                     SKIFF_ROMM_PAGE_SIZE + 2);
    serve_negotiate("");
    serve_complete();
    open_saves_library(SAVE_SCOPES, SAVES_ROMM);
    run_saves();
    open_saves_screen();
    TEST_ASSERT_FALSE_MESSAGE(shows("Can't sync"), "not taken for two games with one save ID");
    TEST_ASSERT_TRUE(shows("No saves on this PSP or in RomM yet."));
}

/* ---- Games sent from RomM ---- */

/* A token paired with the device scopes but not the save ones (save sync stays out of the way). */
#define SENDS_SCOPES "platforms.read,roms.read,devices.read,devices.write"
#define DEVICE_PATH "/api/devices/" SAVE_DEVICE
#define CLAIM_PATH DEVICE_PATH "/installs/claim"
#define REPORT_PATH_MAX 128
#define SEND_ID_FORMAT "5e4d0000-0000-4000-8000-%012u"
#define SEND_ID_MAX 40
#define SENDS_JSON_MAX 8192
#define SENDS_LOG_MAX (64 * 1024)
/* RomM's other platform, and a file past what FAT holds. */
#define OTHER_PLATFORM 2U
#define TOO_LARGE_BYTES 4294967296ULL
#define LIBRARY_PAGE_PATH "/api/roms?platform_ids=1&limit=25&offset=0" LIST_QUERY
#define CLAIM_INTERVAL ((int64_t)SKIFF_APP_INSTALL_CLAIM_INTERVAL_MS)

static void send_id(unsigned n, char *out) { snprintf(out, SEND_ID_MAX, SEND_ID_FORMAT, n); }

static void report_path(unsigned n, char *out) {
    char id[SEND_ID_MAX];
    send_id(n, id);
    snprintf(out, REPORT_PATH_MAX, DEVICE_PATH "/installs/%s", id);
}

/* RomM 5.4's heartbeat with DEVICE_INSTALL.ENABLED as enabled ("true", "false"). */
static void serve_sends_heartbeat(const char *version, const char *enabled) {
    char raw[512];
    snprintf(raw, sizeof raw,
             JSON_OK "{\"SYSTEM\":{\"VERSION\":\"%s\"},\"DEVICE_INSTALL\":{\"ENABLED\":%s,"
                     "\"EXCLUDED_PLATFORM_SLUGS\":[\"win\"]}}",
             version, enabled);
    serve_raw("/api/heartbeat", raw);
}

/* RomM's answer to remote_install set to on. */
static fake_route *serve_register(int on) {
    char raw[256];
    snprintf(raw, sizeof raw,
             JSON_OK "{\"id\":\"" SAVE_DEVICE "\",\"name\":\"Skiff on PSP\",\"capabilities\":{"
                     "\"remote_install\":%s}}",
             on ? "true" : "false");
    return serve_raw(DEVICE_PATH, raw);
}

/* A claim of count sends, send i + 1 for roms[i] with files[i] file ids (1 when files is NULL). */
static fake_route *serve_claim(const unsigned *roms, const unsigned *files, size_t count) {
    static char raw[SENDS_JSON_MAX];
    int used = snprintf(raw, sizeof raw, JSON_OK "[");
    for (size_t i = 0; i < count; i++) {
        char id[SEND_ID_MAX];
        send_id((unsigned)i + 1, id);
        used += snprintf(raw + used, sizeof raw - (size_t)used,
                         "%s{\"id\":\"%s\",\"user_id\":1,\"device_id\":\"" SAVE_DEVICE
                         "\",\"rom_id\":%u,\"file_ids\":[",
                         i > 0 ? "," : "", id, roms[i]);
        const unsigned file_count = files != NULL ? files[i] : 1;
        for (unsigned f = 0; f < file_count; f++) {
            used += snprintf(raw + used, sizeof raw - (size_t)used, "%s%u", f > 0 ? "," : "",
                             roms[i] * 10 + f + 1);
        }
        used +=
            snprintf(raw + used, sizeof raw - (size_t)used,
                     "],\"status\":\"taken\",\"reason\":null,\"created_at\":"
                     "\"2026-10-12T13:52:53+00:00\",\"updated_at\":\"2026-10-12T13:52:54+00:00\"}");
    }
    snprintf(raw + used, sizeof raw - (size_t)used, "]");
    return serve_raw(CLAIM_PATH, raw);
}

static fake_route *serve_empty_claim(void) { return serve_raw(CLAIM_PATH, JSON_OK "[]"); }

/* RomM's answer to send n's report of status. */
static fake_route *serve_report(unsigned n, const char *status) {
    char id[SEND_ID_MAX];
    send_id(n, id);
    char raw[256];
    snprintf(raw, sizeof raw, JSON_OK "{\"id\":\"%s\",\"status\":\"%s\",\"rom_id\":1}", id, status);
    char path[REPORT_PATH_MAX];
    report_path(n, path);
    return serve_raw(path, raw);
}

/* A sent game's details: on platform, file_name (JSON text) of size, file_count files. */
static void serve_sent_rom(unsigned id, unsigned platform, const char *file_name,
                           unsigned long long size, unsigned file_count) {
    char raw[4096];
    int used = snprintf(raw, sizeof raw,
                        JSON_OK "{\"id\":%u,\"platform_id\":%u,\"name\":\"Sent %u\",\"fs_name\":"
                                "\"%s\",\"fs_size_bytes\":%llu,\"crc_hash\":\"%08x\","
                                "\"has_multiple_files\":%s,\"files\":[",
                        id, platform, id, file_name, size, (unsigned)body_crc,
                        file_count > 1 ? "true" : "false");
    for (unsigned f = 0; f < file_count; f++) {
        used += snprintf(raw + used, sizeof raw - (size_t)used,
                         "%s{\"rom_id\":%u,\"file_name\":\"%s%s\",\"file_size_bytes\":%llu,"
                         "\"crc_hash\":\"%08x\"}",
                         f > 0 ? "," : "", id, f > 0 ? "update " : "", file_name, size,
                         (unsigned)body_crc);
    }
    snprintf(raw + used, sizeof raw - (size_t)used, "]}");
    char path[64];
    snprintf(path, sizeof path, "/api/roms/%u", id);
    serve_raw(path, raw);
}

static void write_sends_config(const char *scopes, const char *extra) {
    char config[TEXT_MAX];
    snprintf(config, sizeof config,
             "[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN
             "\ndevice_identifier = 00112233445566778899aabbccddeeff\ndevice_id = " SAVE_DEVICE
             "\nscopes = %s\n[network]\nprofile = 1\n%s",
             scopes, extra);
    write_config(config);
}

/* A paired library of three games on RomM version, whose heartbeat says enabled (NULL: no
 * DEVICE_INSTALL at all, as 5.3), the token holding scopes; extra ends config.ini. */
static void open_sends_library_on(const char *version, const char *enabled, const char *scopes,
                                  const char *extra) {
    write_sends_config(scopes, extra);
    if (enabled != NULL) {
        serve_sends_heartbeat(version, enabled);
    } else {
        serve_heartbeat(version);
    }
    serve_platforms();
    serve_page(0, 3, 3);
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
}

static void open_sends_library(void) {
    open_sends_library_on(SAVES_ROMM, "true", SENDS_SCOPES, "");
}

static size_t requests_to(const char *path) {
    size_t count = 0;
    for (size_t i = 0; i < transport.log_count; i++) {
        count += strcmp(transport.log[i].url + strlen(SERVER), path) == 0;
    }
    return count;
}

/* The first request to path, from the first; -1 for none. */
static int request_index(const char *path) {
    for (size_t i = 0; i < transport.log_count; i++) {
        if (strcmp(transport.log[i].url + strlen(SERVER), path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static const fake_request *last_request_to(const char *path) {
    const fake_request *last = NULL;
    for (size_t i = 0; i < transport.log_count; i++) {
        if (strcmp(transport.log[i].url + strlen(SERVER), path) == 0) {
            last = &transport.log[i];
        }
    }
    return last;
}

/* Send n's last report: its body, or "" when none was made. */
static const char *report_of(unsigned n) {
    char path[REPORT_PATH_MAX];
    report_path(n, path);
    const fake_request *request = last_request_to(path);
    if (request == NULL) {
        return "";
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_PUT, request->method);
    return request->body;
}

static void assert_reported(unsigned n, const char *status, const char *reason) {
    char expected[FAKE_TRANSPORT_BODY_MAX];
    if (reason != NULL) {
        snprintf(expected, sizeof expected, "{\"status\":\"%s\",\"reason\":\"%s\"}", status,
                 reason);
    } else {
        snprintf(expected, sizeof expected, "{\"status\":\"%s\"}", status);
    }
    TEST_PRINTF("send %u reported %s", n, report_of(n));
    TEST_ASSERT_EQUAL_STRING(expected, report_of(n));
}

static void read_log(char *out, size_t size) {
    skiff_log_flush(app->log);
    read_app_file(SKIFF_APP_LOG_FILE_NAME, out, size);
}

/* Settings, its fourth row: Games from RomM. */
static void open_sends_row(void) {
    frame(SKIFF_UI_ACTION_MENU);
    run_until(SKIFF_APP_SCREEN_SETTINGS);
    for (int i = 0; i < 3; i++) {
        frame(SKIFF_UI_ACTION_DOWN);
    }
    TEST_ASSERT_EQUAL_size_t(3, view()->list.selected);
    print_view();
}

static const skiff_app_row *sends_row(void) { return &view()->rows[3]; }

/* Frames until the next claim has been made, then until its round has ended. */
static void wait_for_claim(void);

/* Frames until the round under way, if any, has ended. */
static void run_sends(void) {
    for (int i = 0; i < FRAMES_MAX && (app->sends.round != NULL || app_call_busy(app)); i++) {
        frame(0);
    }
    TEST_ASSERT_NULL(app->sends.round);
    print_view();
}

static void wait_for_claim(void) {
    const size_t claims = requests_to(CLAIM_PATH);
    for (int i = 0; i < 2 * FRAMES_MAX && requests_to(CLAIM_PATH) == claims; i++) {
        frame(0);
    }
    TEST_ASSERT_EQUAL_size_t(claims + 1, requests_to(CLAIM_PATH));
    run_sends();
}

/* queue.json holding jobs (their targets under the games folder). */
static void write_queue(skiff_job *jobs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        char name[sizeof "ISO/" + SKIFF_JOBS_FILE_NAME_MAX];
        snprintf(name, sizeof name, "ISO/%s", jobs[i].file_name);
        TEST_ASSERT_TRUE(temp_dir_path(dir, name, jobs[i].target, sizeof jobs[i].target));
    }
    static char text[SKIFF_JOBS_FILE_MAX];
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_jobs_format(jobs, count, (uint32_t)count + 1, text, sizeof text, &length));
    char path[TEMP_DIR_PATH_MAX];
    app_file(SKIFF_JOBS_FILE_NAME, path, sizeof path);
    write_file(path, text, length);
}

static skiff_job queued_game(unsigned id, unsigned rom, skiff_job_state state) {
    skiff_job job;
    memset(&job, 0, sizeof job);
    job.id = id;
    job.rom_id = rom;
    job.size = BODY_BYTES;
    job.has_crc32 = 1;
    job.crc32 = body_crc;
    job.state = state;
    job.error = state == SKIFF_JOB_FAILED ? SKIFF_ERR_ROMM_CHECKSUM : SKIFF_OK;
    snprintf(job.title, sizeof job.title, "Game %u", rom);
    snprintf(job.file_name, sizeof job.file_name, "Game %u.iso", rom);
    return job;
}

static void test_a_game_sent_from_romm_is_added_right_after_connecting(void) {
    serve_register(1);
    const unsigned roms[] = {2};
    serve_claim(roms, NULL, 1)->max_uses = 1;
    serve_empty_claim();
    serve_rom(2);
    serve_report(1, "done");
    open_sends_library();
    TEST_PRINTF("the library's first page, then remote_install, the claim, the game, its report");
    const int page = request_index(LIBRARY_PAGE_PATH);
    const int registered = request_index(DEVICE_PATH);
    const int claimed = request_index(CLAIM_PATH);
    const int rom = request_index("/api/roms/2");
    char path[REPORT_PATH_MAX];
    report_path(1, path);
    const int reported = request_index(path);
    TEST_PRINTF("page %d, register %d, claim %d, ROM %d, report %d", page, registered, claimed, rom,
                reported);
    TEST_ASSERT_TRUE(page >= 0 && page < registered && registered < claimed && claimed < rom &&
                     rom < reported);
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_PUT, transport.log[registered].method);
    TEST_ASSERT_EQUAL_STRING("{\"capabilities\":{\"remote_install\":true}}",
                             transport.log[registered].body);
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_POST, transport.log[claimed].method);
    assert_reported(1, "done", NULL);

    TEST_PRINTF("added without asking, into the games folder, the worker running");
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
    assert_job_target(&jobs[0], 2, "ISO/Game 2.iso");
    TEST_ASSERT_EQUAL_STRING("Game 2", jobs[0].title);
    TEST_ASSERT_EQUAL_INT(1, env_state.worker_starts);
    TEST_ASSERT_EQUAL_STRING("From RomM: 1 added to Downloads", view()->status);
    static char log_text[SENDS_LOG_MAX];
    read_log(log_text, sizeof log_text);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "send 5e4d0000: rom 2 -> done (queued)"));
    TEST_PRINTF("never the token or the device's headers");
    TEST_ASSERT_NULL(strstr(log_text, TOKEN));
}

static void test_claims_repeat_every_interval_and_empty_ones_leave_no_trace(void) {
    serve_register(1);
    serve_empty_claim();
    open_sends_library();
    TEST_ASSERT_EQUAL_size_t(1, requests_to(CLAIM_PATH));
    wait_ms(CLAIM_INTERVAL - 200);
    TEST_PRINTF("nothing before the interval");
    TEST_ASSERT_EQUAL_size_t(1, requests_to(CLAIM_PATH));
    wait_ms(400);
    TEST_ASSERT_EQUAL_size_t(2, requests_to(CLAIM_PATH));
    wait_ms(CLAIM_INTERVAL);
    TEST_ASSERT_EQUAL_size_t(3, requests_to(CLAIM_PATH));
    TEST_PRINTF("remote_install once for the connection");
    TEST_ASSERT_EQUAL_size_t(1, requests_to(DEVICE_PATH));
    TEST_PRINTF("an empty claim shows nothing and logs nothing");
    TEST_ASSERT_FALSE(shows("From RomM"));
    static char log_text[SENDS_LOG_MAX];
    read_log(log_text, sizeof log_text);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "send: RomM may send games to this PSP"));
    TEST_ASSERT_NULL(strstr(log_text, "claim"));
}

static void test_sends_wait_for_pages_download_all_dialogs_server_changes_and_pairing(void) {
    serve_register(1);
    serve_empty_claim();
    open_sends_library();
    size_t claims = requests_to(CLAIM_PATH);
    TEST_ASSERT_EQUAL_size_t(1, claims);

    TEST_PRINTF("a page on its way: the claim due meanwhile waits for it");
    serve_favourites(2);
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_SELECT);
    wait_ms(CLAIM_INTERVAL + 100);
    TEST_ASSERT_EQUAL_INT(CALL_PAGE, (int)app->call.kind);
    TEST_ASSERT_EQUAL_size_t(claims, requests_to(CLAIM_PATH));
    env_state.hold_calls = 0;
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_size_t(++claims, requests_to(CLAIM_PATH));

    TEST_PRINTF("Download all's question: no claim until it is answered");
    serve_batch_favourites(2);
    start_download_all(SKIFF_APP_SCREEN_CONFIRM);
    wait_ms(CLAIM_INTERVAL + 100);
    TEST_ASSERT_EQUAL_size_t(claims, requests_to(CLAIM_PATH));
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    TEST_ASSERT_EQUAL_size_t(++claims, requests_to(CLAIM_PATH));

    TEST_PRINTF("the server's keyboard: no claim while it is open");
    frame(SKIFF_UI_ACTION_MENU);
    run_until(SKIFF_APP_SCREEN_SETTINGS);
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_DIALOG_KEYBOARD, view()->dialog);
    wait_ms(CLAIM_INTERVAL + 100);
    TEST_ASSERT_EQUAL_size_t(claims, requests_to(CLAIM_PATH));
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_CANCELLED, NULL);
    run_until(SKIFF_APP_SCREEN_SETTINGS);
    TEST_ASSERT_EQUAL_size_t(++claims, requests_to(CLAIM_PATH));

    TEST_PRINTF("a new server's question (installed games would be forgotten): no claim");
    record_in_app(1, body_crc);
    frame(SKIFF_UI_ACTION_CONFIRM);
    skiff_app_dialog_done(app, SKIFF_APP_DIALOG_ACCEPTED, "https://other.test");
    frame(0);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_CONFIRM, view()->screen);
    wait_ms(CLAIM_INTERVAL + 100);
    TEST_ASSERT_EQUAL_size_t(claims, requests_to(CLAIM_PATH));
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_SETTINGS);
    TEST_ASSERT_EQUAL_size_t(++claims, requests_to(CLAIM_PATH));

    TEST_PRINTF("pairing again: no claim while the pair screen shows its code");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    frame(SKIFF_UI_ACTION_DOWN);
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_PAIR, view()->screen);
    wait_ms(CLAIM_INTERVAL + 100);
    TEST_ASSERT_EQUAL_size_t(claims, requests_to(CLAIM_PATH));
    frame(SKIFF_UI_ACTION_MENU);
    run_until(SKIFF_APP_SCREEN_SETTINGS);
    TEST_ASSERT_EQUAL_size_t(++claims, requests_to(CLAIM_PATH));
}

static void test_a_lost_connection_pauses_claims_until_the_wifi_is_joined_again(void) {
    serve_register(1);
    fake_route *lost = serve_empty_claim();
    lost->fail_before_response = SKIFF_ERR_NET_CONNECT;
    lost->max_uses = 1;
    serve_empty_claim();
    open_sends_library();
    TEST_ASSERT_EQUAL_size_t(1, requests_to(CLAIM_PATH));
    TEST_ASSERT_FALSE(app->net_joined);
    const int joins = env_state.net_starts;
    TEST_PRINTF("no claim, no join of its own, no error on screen");
    wait_ms(3 * CLAIM_INTERVAL);
    TEST_ASSERT_EQUAL_size_t(1, requests_to(CLAIM_PATH));
    TEST_ASSERT_EQUAL_INT(joins, env_state.net_starts);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    TEST_PRINTF("the player's next request joins again, and the claim follows the connection");
    open_details(1);
    TEST_ASSERT_EQUAL_INT(joins + 1, env_state.net_starts);
    TEST_ASSERT_EQUAL_size_t(2, requests_to(CLAIM_PATH));
    TEST_PRINTF("a new connection registers again");
    TEST_ASSERT_EQUAL_size_t(2, requests_to(DEVICE_PATH));
}

static void test_a_refused_claim_stops_sends_and_settings_offers_pair_again(void) {
    serve_register(1);
    serve_raw(CLAIM_PATH, "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n\r\n"
                          "{\"detail\":\"Forbidden\"}");
    open_sends_library();
    TEST_ASSERT_EQUAL_size_t(1, requests_to(CLAIM_PATH));
    wait_ms(2 * CLAIM_INTERVAL);
    TEST_ASSERT_EQUAL_size_t(1, requests_to(CLAIM_PATH));
    open_sends_row();
    TEST_ASSERT_EQUAL_STRING("Games from RomM: Pair again to allow it", sends_row()->label);
    TEST_ASSERT_FALSE(sends_row()->dim);
    TEST_PRINTF("X pairs again");
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_PAIR, view()->screen);
}

static void test_a_device_romm_no_longer_knows_gets_no_claim_until_the_next_connection(void) {
    serve_register(1);
    open_sends_library();
    TEST_PRINTF("the claim is 404: the device was deleted in RomM, or sends turned off");
    TEST_ASSERT_EQUAL_size_t(1, requests_to(CLAIM_PATH));
    wait_ms(2 * CLAIM_INTERVAL);
    TEST_ASSERT_EQUAL_size_t(1, requests_to(CLAIM_PATH));
    TEST_PRINTF("a suspend: the next connection asks again");
    env_state.suspends++;
    open_details(1);
    TEST_ASSERT_EQUAL_size_t(2, requests_to(CLAIM_PATH));
}

/* No request about sends at all, and Settings' row says why. */
static void assert_no_sends(const char *version, const char *enabled, const char *scopes,
                            const char *extra, const char *row, int dim) {
    TEST_PRINTF("RomM %s, DEVICE_INSTALL %s, scopes %s, %s", version,
                enabled != NULL ? enabled : "absent", scopes, extra);
    open_sends_library_on(version, enabled, scopes, extra);
    wait_ms(CLAIM_INTERVAL + 100);
    TEST_ASSERT_FALSE(requested_prefix("/api/devices"));
    open_sends_row();
    TEST_ASSERT_EQUAL_STRING(row, sends_row()->label);
    TEST_ASSERT_EQUAL_INT(dim, sends_row()->dim);
    if (dim) {
        TEST_PRINTF("X does nothing");
        frame(SKIFF_UI_ACTION_CONFIRM);
        TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_SETTINGS, view()->screen);
        TEST_ASSERT_FALSE(requested_prefix("/api/devices"));
    }
    skiff_app_destroy(app);
    app = NULL;
    skiff_transport_destroy(&transport.base);
    fake_transport_init(&transport);
}

static void test_no_request_on_romm_5_3_turned_off_without_devices_write_or_switched_off(void) {
    assert_no_sends("5.3.1", NULL, SENDS_SCOPES, "", "Games from RomM: Needs RomM 5.4", 1);
    assert_no_sends(SAVES_ROMM, "false", SENDS_SCOPES, "", "Games from RomM: Turned off in RomM",
                    1);
    assert_no_sends(SAVES_ROMM, "true", BROWSE_SCOPES, "",
                    "Games from RomM: Pair again to allow it", 0);
}

static void test_switched_off_tells_romm_once_per_connection_and_never_claims(void) {
    serve_register(0);
    open_sends_library_on(SAVES_ROMM, "true", SENDS_SCOPES, "[downloads]\nfrom_romm = off\n");
    TEST_PRINTF("an earlier run may have quit before telling RomM: false once per connection");
    TEST_ASSERT_EQUAL_size_t(1, requests_to(DEVICE_PATH));
    TEST_ASSERT_EQUAL_STRING("{\"capabilities\":{\"remote_install\":false}}",
                             last_request_to(DEVICE_PATH)->body);
    TEST_ASSERT_EQUAL_size_t(0, requests_to(CLAIM_PATH));
    wait_ms(2 * CLAIM_INTERVAL);
    TEST_ASSERT_EQUAL_size_t(1, requests_to(DEVICE_PATH));
    TEST_ASSERT_EQUAL_size_t(0, requests_to(CLAIM_PATH));
    TEST_PRINTF("no probe of the Wi-Fi for it either: nothing left to do");
    TEST_ASSERT_EQUAL_INT(0, env_state.online_asks);
    TEST_PRINTF("the next connection tells RomM again, and still never claims");
    env_state.suspends++;
    open_details(1);
    TEST_ASSERT_EQUAL_size_t(2, requests_to(DEVICE_PATH));
    TEST_ASSERT_EQUAL_size_t(0, requests_to(CLAIM_PATH));
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    open_sends_row();
    TEST_ASSERT_EQUAL_STRING("Games from RomM: Off", sends_row()->label);
}

static void test_an_unreachable_romm_on_a_joined_wifi_keeps_the_claim_interval(void) {
    serve_register(1);
    fake_route *down = serve_empty_claim();
    down->fail_before_response = SKIFF_ERR_NET_CONNECT;
    TEST_PRINTF("the access point stays joined while RomM cannot be reached");
    env_state.online = 1;
    open_sends_library();
    wait_ms(3 * CLAIM_INTERVAL);
    TEST_PRINTF("%zu claim(s) in three intervals", requests_to(CLAIM_PATH));
    TEST_ASSERT_TRUE(requests_to(CLAIM_PATH) <= 4);
    TEST_ASSERT_TRUE(requests_to(CLAIM_PATH) >= 3);
    static char log_text[SENDS_LOG_MAX];
    read_log(log_text, sizeof log_text);
    const char *first = strstr(log_text, "send: claim: SKIFF_ERR_NET_CONNECT");
    TEST_ASSERT_NOT_NULL(first);
    TEST_PRINTF("logged once");
    TEST_ASSERT_NULL(strstr(first + 1, "send: claim: SKIFF_ERR_NET_CONNECT"));
}

static void test_claims_go_on_once_something_else_joins_the_wifi_again(void) {
    serve_register(1);
    fake_route *lost = serve_empty_claim();
    lost->fail_before_response = SKIFF_ERR_NET_CONNECT;
    lost->max_uses = 1;
    serve_empty_claim();
    open_sends_library();
    TEST_ASSERT_FALSE(app->net_joined);
    const int joins = env_state.net_starts;
    TEST_PRINTF("the platform is asked at most once a second; nobody has joined yet");
    wait_ms(2 * CLAIM_INTERVAL);
    TEST_PRINTF("asked %d time(s)", env_state.online_asks);
    TEST_ASSERT_GREATER_THAN_INT(0, env_state.online_asks);
    TEST_ASSERT_TRUE(env_state.online_asks <=
                     (int)(2 * CLAIM_INTERVAL / APP_SENDS_JOIN_PROBE_MS) + 1);
    TEST_ASSERT_EQUAL_size_t(1, requests_to(CLAIM_PATH));
    TEST_PRINTF("the download worker rejoins (a PSP left on Downloads): the claim follows at its "
                "interval");
    env_state.online = 1;
    wait_ms(APP_SENDS_JOIN_PROBE_MS + 100);
    TEST_ASSERT_TRUE(app->net_joined);
    wait_for_claim();
    TEST_ASSERT_EQUAL_size_t(2, requests_to(CLAIM_PATH));
    TEST_ASSERT_TRUE(app->net_joined);
    TEST_ASSERT_EQUAL_INT(joins, env_state.net_starts);
    TEST_PRINTF("same connection to RomM: remote_install is not sent again");
    TEST_ASSERT_EQUAL_size_t(1, requests_to(DEVICE_PATH));
    const int asks = env_state.online_asks;
    wait_ms(CLAIM_INTERVAL);
    TEST_ASSERT_EQUAL_size_t(3, requests_to(CLAIM_PATH));
    TEST_ASSERT_EQUAL_INT(asks, env_state.online_asks);
    TEST_PRINTF("a suspend: a join the platform does not vouch for resumes nothing");
    env_state.online = 0;
    env_state.suspends++;
    wait_ms(2 * CLAIM_INTERVAL);
    TEST_ASSERT_EQUAL_size_t(3, requests_to(CLAIM_PATH));
    TEST_ASSERT_EQUAL_INT(joins, env_state.net_starts);
}

static void test_sends_romm_cannot_describe_or_skiff_cannot_install_are_refused_with_why(void) {
    serve_register(1);
    const unsigned roms[] = {41, 42, 43, 44, 45, 46, 47, 48, 49, 50};
    const unsigned files[] = {1, 1, 1, 1, 2, 1, 1, 1, 1, 2};
    serve_claim(roms, files, 10)->max_uses = 1;
    serve_empty_claim();
    TEST_PRINTF("41 unknown to RomM; 42 not the user's; 43 an answer Skiff refuses");
    serve_raw("/api/roms/42", "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n\r\n"
                              "{\"detail\":\"Forbidden\"}");
    serve_raw("/api/roms/43", JSON_OK "{}");
    TEST_PRINTF("44 a SNES game named .iso; 45 a folder; 46 a .zip; 47 a control character");
    serve_sent_rom(44, OTHER_PLATFORM, "Sent 44.iso", BODY_BYTES, 1);
    serve_sent_rom(45, 1, "Sent 45.iso", BODY_BYTES, 2);
    serve_sent_rom(46, 1, "Sent 46.zip", BODY_BYTES, 1);
    serve_sent_rom(47, 1, "Bad\\u0007Name.iso", BODY_BYTES, 1);
    TEST_PRINTF("48 empty; 49 past 4 GiB; 50 one file in RomM's list, but sent with its update");
    serve_sent_rom(48, 1, "Sent 48.iso", 0, 1);
    serve_sent_rom(49, 1, "Sent 49.iso", TOO_LARGE_BYTES, 1);
    serve_sent_rom(50, 1, "Sent 50.iso", BODY_BYTES, 1);
    TEST_PRINTF("no report is served: RomM's 404 (the send ended) is only logged");
    open_sends_library();
    assert_reported(1, "failed", "RomM could not find that item [202]");
    assert_reported(2, "failed", "Your RomM user is not allowed to do this [201]");
    assert_reported(3, "failed", "RomM sent a response Skiff does not understand [204]");
    assert_reported(4, "failed", "That game is not a PSP game [219]");
    assert_reported(5, "failed", english(SKIFF_TEXT_INSTALL_MULTIPLE_FILES));
    assert_reported(6, "failed", english(SKIFF_TEXT_INSTALL_UNKNOWN_EXTENSION));
    assert_reported(7, "failed", english(SKIFF_TEXT_NAME_UNSUPPORTED));
    assert_reported(8, "failed", english(SKIFF_TEXT_INSTALL_EMPTY_FILE));
    assert_reported(9, "failed", "File is larger than 4 GB, which the PSP cannot store [304]");
    assert_reported(10, "failed", english(SKIFF_TEXT_INSTALL_MULTIPLE_FILES));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(0, list_jobs(jobs));
    TEST_PRINTF("the player sees how many, RomM says why; no error on screen");
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    TEST_ASSERT_EQUAL_STRING("From RomM: 0 added, 10 not added (see RomM)", view()->status);
    static char log_text[SENDS_LOG_MAX];
    read_log(log_text, sizeof log_text);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "rom 44 -> failed (not a PSP game: "
                                          "SKIFF_ERR_ROMM_SEND_NOT_PSP); report: "
                                          "SKIFF_ERR_ROMM_NOT_FOUND"));
    TEST_PRINTF("claims go on as before");
    wait_ms(CLAIM_INTERVAL);
    TEST_ASSERT_EQUAL_size_t(2, requests_to(CLAIM_PATH));
}

static void test_sends_follow_download_alls_rules_for_what_this_psp_holds(void) {
    skiff_job held[] = {queued_game(1, 51, SKIFF_JOB_QUEUED), queued_game(2, 52, SKIFF_JOB_FAILED)};
    write_queue(held, 2);
    const unsigned installed[] = {53, 54};
    const uint32_t crcs[] = {body_crc, body_crc ^ 1U};
    record_installs(installed, crcs, 2);
    serve_register(1);
    TEST_PRINTF(
        "51 in Downloads; 52 failed there; 53 installed; 54 changed in RomM; 55 new, twice");
    const unsigned roms[] = {51, 52, 53, 54, 55, 55};
    serve_claim(roms, NULL, 6)->max_uses = 1;
    serve_empty_claim();
    for (unsigned id = 51; id <= 55; id++) {
        serve_rom(id);
    }
    open_sends_library();
    assert_reported(1, "done", NULL);
    assert_reported(2, "failed", english(SKIFF_TEXT_SEND_FAILED_DOWNLOAD));
    assert_reported(3, "already_installed", NULL);
    assert_reported(4, "failed", english(SKIFF_TEXT_SEND_CHANGED));
    assert_reported(5, "done", NULL);
    assert_reported(6, "done", NULL);
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(3, list_jobs(jobs));
    TEST_ASSERT_EQUAL_UINT64(55, jobs[2].rom_id);
    TEST_PRINTF("the changed copy is left as it is: replacing it stays the player's choice");
    TEST_ASSERT_EQUAL_INT(0, jobs[2].replace_target);
    TEST_ASSERT_EQUAL_STRING("From RomM: 1 added, 2 not added (see RomM)", view()->status);
}

static void test_sends_past_the_free_space_fail_in_claim_order(void) {
    serve_register(1);
    const unsigned roms[] = {63, 61, 62};
    serve_claim(roms, NULL, 3)->max_uses = 1;
    serve_empty_claim();
    for (unsigned id = 61; id <= 63; id++) {
        serve_rom(id);
    }
    TEST_PRINTF("room for two games past the 8 MiB margin: the oldest sends first");
    storage.free_bytes =
        SKIFF_STORAGE_FREE_MARGIN_BYTES + (uint64_t)2 * BODY_BYTES + BODY_BYTES / 2;
    open_sends_library();
    assert_reported(1, "done", NULL);
    assert_reported(2, "done", NULL);
    assert_reported(3, "failed", "Not enough free space on the Memory Stick [301]");
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(2, list_jobs(jobs));
    TEST_ASSERT_EQUAL_UINT64(63, jobs[0].rom_id);
    TEST_ASSERT_EQUAL_UINT64(61, jobs[1].rom_id);
}

static void test_a_send_to_a_full_download_list_says_so(void) {
    write_full_queue();
    serve_register(1);
    const unsigned roms[] = {64};
    serve_claim(roms, NULL, 1)->max_uses = 1;
    serve_empty_claim();
    serve_rom(64);
    open_sends_library();
    assert_reported(1, "failed", english(SKIFF_TEXT_QUEUE_FULL));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(SKIFF_JOBS_MAX, list_jobs(jobs));
}

static void test_a_send_past_the_installed_records_says_so(void) {
    static unsigned ids[SKIFF_INSTALL_RECORDS_MAX];
    static uint32_t crcs[SKIFF_INSTALL_RECORDS_MAX];
    for (unsigned i = 0; i < SKIFF_INSTALL_RECORDS_MAX; i++) {
        ids[i] = 1000 + i;
        crcs[i] = body_crc;
    }
    record_installs(ids, crcs, SKIFF_INSTALL_RECORDS_MAX);
    serve_register(1);
    const unsigned roms[] = {65};
    serve_claim(roms, NULL, 1)->max_uses = 1;
    serve_empty_claim();
    serve_rom(65);
    open_sends_library();
    assert_reported(1, "failed", english(SKIFF_TEXT_INSTALLED_FULL));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(0, list_jobs(jobs));
}

static void test_a_queue_that_cannot_be_saved_fails_each_send_with_why(void) {
    serve_register(1);
    serve_empty_claim()->max_uses = 1;
    const unsigned roms[] = {66, 67};
    serve_claim(roms, NULL, 2);
    serve_rom(66);
    serve_rom(67);
    open_sends_library();
    storage.fail_suffix = SKIFF_JOBS_FILE_NAME SKIFF_STORAGE_DRAFT_SUFFIX;
    storage.rename_error = SKIFF_ERR_STORAGE_IO;
    wait_for_claim();
    storage.rename_error = SKIFF_OK;
    assert_reported(1, "failed", "Could not read or write the Memory Stick [302]");
    assert_reported(2, "failed", "Could not read or write the Memory Stick [302]");
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(0, list_jobs(jobs));
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
}

/* What the queue's last save saw: queue.json opened, and the requests made by then. */
static size_t requests_at_queue_save;

static void note_queue_save(void *ctx, const char *call, const char *path) {
    count_queue_opens(ctx, call, path);
    if (strcmp(call, "open") == 0 && strstr(path, SKIFF_JOBS_FILE_NAME) != NULL) {
        requests_at_queue_save = transport.request_count;
    }
}

static void test_a_round_queues_with_one_save_and_reports_only_after_it(void) {
    serve_register(1);
    serve_empty_claim()->max_uses = 1;
    const unsigned roms[] = {71, 72, 73};
    serve_claim(roms, NULL, 3)->max_uses = 1;
    serve_empty_claim();
    for (unsigned id = 71; id <= 73; id++) {
        serve_rom(id);
    }
    open_sends_library();
    TEST_PRINTF("one game queued from its details: what one save of the queue opens");
    open_details(1);
    queue_file_opens = 0;
    storage.on_call = count_queue_opens;
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    const int opens_per_save = queue_file_opens;
    TEST_ASSERT_GREATER_THAN_INT(0, opens_per_save);

    TEST_PRINTF("three sends: one save, every report after it");
    queue_file_opens = 0;
    requests_at_queue_save = 0;
    storage.on_call = note_queue_save;
    wait_for_claim();
    storage.on_call = NULL;
    TEST_PRINTF("queue.json opened %d time(s), %d per save; the save after request %zu",
                queue_file_opens, opens_per_save, requests_at_queue_save);
    TEST_ASSERT_EQUAL_INT(opens_per_save, queue_file_opens);
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(4, list_jobs(jobs));
    for (unsigned n = 1; n <= 3; n++) {
        char path[REPORT_PATH_MAX];
        report_path(n, path);
        const int reported = request_index(path);
        TEST_ASSERT_TRUE(reported >= 0);
        TEST_ASSERT_TRUE((size_t)reported >= requests_at_queue_save);
        assert_reported(n, "done", NULL);
    }
    TEST_ASSERT_TRUE((size_t)request_index("/api/roms/73") < requests_at_queue_save);
    TEST_ASSERT_EQUAL_STRING("From RomM: 3 added to Downloads", view()->status);
}

static void test_a_send_seen_again_finds_its_game_in_downloads(void) {
    serve_register(1);
    const unsigned roms[] = {81};
    serve_claim(roms, NULL, 1);
    serve_rom(81);
    char path[REPORT_PATH_MAX];
    report_path(1, path);
    fake_route *lost = serve_report(1, "done");
    lost->fail_before_response = SKIFF_ERR_NET_CONNECT;
    lost->max_uses = 1;
    serve_report(1, "done");
    open_sends_library();
    TEST_PRINTF("queued, but the report was lost with the connection");
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
    TEST_ASSERT_FALSE(app->net_joined);
    char before[TEXT_MAX];
    read_app_file(SKIFF_JOBS_FILE_NAME, before, sizeof before);
    wait_ms(SKIFF_APP_NOTE_MS);
    TEST_PRINTF("joined again: the same send comes back, finds its game in Downloads");
    queue_file_opens = 0;
    storage.on_call = count_queue_opens;
    open_details(1);
    storage.on_call = NULL;
    TEST_ASSERT_EQUAL_size_t(2, requests_to(CLAIM_PATH));
    TEST_ASSERT_EQUAL_size_t(2, requests_to(path));
    assert_reported(1, "done", NULL);
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
    TEST_ASSERT_EQUAL_INT(0, queue_file_opens);
    char after[TEXT_MAX];
    read_app_file(SKIFF_JOBS_FILE_NAME, after, sizeof after);
    TEST_ASSERT_EQUAL_STRING(before, after);
    TEST_PRINTF("nothing new to say");
    TEST_ASSERT_FALSE(shows("From RomM"));
}

static void test_a_send_whose_game_is_lost_with_the_connection_stays_in_romm(void) {
    serve_register(1);
    const unsigned roms[] = {91, 92};
    serve_claim(roms, NULL, 2);
    fake_route *lost = serve_raw("/api/roms/91", JSON_OK "{}");
    lost->fail_before_response = SKIFF_ERR_NET_TIMEOUT;
    lost->max_uses = 1;
    serve_rom(91);
    serve_rom(92);
    open_sends_library();
    TEST_PRINTF("91's details lost: no report, 92 not even asked; nothing queued");
    TEST_ASSERT_EQUAL_STRING("", report_of(1));
    TEST_ASSERT_EQUAL_STRING("", report_of(2));
    TEST_ASSERT_EQUAL_size_t(0, requests_to("/api/roms/92"));
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(0, list_jobs(jobs));
    TEST_ASSERT_FALSE(app->net_joined);
    TEST_ASSERT_FALSE(shows("From RomM"));
    static char log_text[SENDS_LOG_MAX];
    read_log(log_text, sizeof log_text);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "rom 91 -> left taken (details: SKIFF_ERR_NET_TIMEOUT)"));
    TEST_PRINTF("joined again: both come back and are added");
    open_details(1);
    assert_reported(1, "done", NULL);
    assert_reported(2, "done", NULL);
    TEST_ASSERT_EQUAL_size_t(2, list_jobs(jobs));
}

static void test_switching_games_from_romm_off_tells_romm_and_stops_claims(void) {
    serve_register(1)->max_uses = 1;
    serve_register(0)->max_uses = 1;
    serve_register(1);
    serve_empty_claim();
    open_sends_library();
    open_sends_row();
    TEST_ASSERT_EQUAL_STRING("Games from RomM: On", sends_row()->label);
    TEST_PRINTF("X: off, saved, and RomM told at once");
    frame(SKIFF_UI_ACTION_CONFIRM);
    frame(0);
    run_sends();
    TEST_ASSERT_EQUAL_STRING("Games from RomM: Off", sends_row()->label);
    char config[TEXT_MAX];
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_ASSERT_NOT_NULL(strstr(config, "[downloads]\nfrom_romm = off\n"));
    TEST_ASSERT_EQUAL_size_t(2, requests_to(DEVICE_PATH));
    TEST_ASSERT_EQUAL_STRING("{\"capabilities\":{\"remote_install\":false}}",
                             last_request_to(DEVICE_PATH)->body);
    TEST_PRINTF("no claim, and no more requests about it");
    wait_ms(2 * CLAIM_INTERVAL);
    TEST_ASSERT_EQUAL_size_t(1, requests_to(CLAIM_PATH));
    TEST_ASSERT_EQUAL_size_t(2, requests_to(DEVICE_PATH));
    TEST_PRINTF("X again: on, RomM told, and the claim follows at once");
    frame(SKIFF_UI_ACTION_CONFIRM);
    frame(0);
    run_sends();
    TEST_ASSERT_EQUAL_STRING("Games from RomM: On", sends_row()->label);
    TEST_ASSERT_EQUAL_size_t(3, requests_to(DEVICE_PATH));
    TEST_ASSERT_EQUAL_STRING("{\"capabilities\":{\"remote_install\":true}}",
                             last_request_to(DEVICE_PATH)->body);
    TEST_ASSERT_EQUAL_size_t(2, requests_to(CLAIM_PATH));
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_ASSERT_NOT_NULL(strstr(config, "from_romm = on\n"));
}

static void test_a_failed_switch_off_is_tried_again_on_the_next_connection(void) {
    serve_register(1)->max_uses = 1;
    serve_raw(DEVICE_PATH,
              "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n"
              "\r\n{}")
        ->max_uses = 1;
    serve_register(0);
    serve_empty_claim();
    open_sends_library();
    open_sends_row();
    TEST_PRINTF("off: RomM fails the first time");
    frame(SKIFF_UI_ACTION_CONFIRM);
    frame(0);
    run_sends();
    TEST_ASSERT_EQUAL_size_t(2, requests_to(DEVICE_PATH));
    TEST_PRINTF("not again on this connection, and never a claim");
    wait_ms(2 * CLAIM_INTERVAL);
    TEST_ASSERT_EQUAL_size_t(2, requests_to(DEVICE_PATH));
    TEST_ASSERT_EQUAL_size_t(1, requests_to(CLAIM_PATH));
    static char log_text[SENDS_LOG_MAX];
    read_log(log_text, sizeof log_text);
    TEST_ASSERT_NOT_NULL(strstr(log_text,
                                "send: remote_install false: SKIFF_ERR_ROMM_SERVER (203), "
                                "not again until the next connection"));
    TEST_PRINTF("the next connection tries once more");
    frame(SKIFF_UI_ACTION_BACK);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    env_state.suspends++;
    open_details(1);
    TEST_ASSERT_EQUAL_size_t(3, requests_to(DEVICE_PATH));
    TEST_ASSERT_EQUAL_STRING("{\"capabilities\":{\"remote_install\":false}}",
                             last_request_to(DEVICE_PATH)->body);
    TEST_ASSERT_EQUAL_size_t(1, requests_to(CLAIM_PATH));
}

static void test_download_all_waits_for_a_round_that_may_take_its_room(void) {
    serve_register(1);
    serve_empty_claim()->max_uses = 1;
    const unsigned roms[] = {2};
    serve_claim(roms, NULL, 1)->max_uses = 1;
    serve_empty_claim();
    serve_rom(2);
    open_sends_library();
    serve_favourites(2);
    frame(SKIFF_UI_ACTION_SELECT);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    serve_batch_favourites(2);
    TEST_PRINTF("the next claim runs: START does nothing until its games are queued");
    for (int i = 0; i < 2 * FRAMES_MAX && app->call.kind != CALL_SENDS_CLAIM; i++) {
        frame(0);
    }
    TEST_ASSERT_EQUAL_INT(CALL_SENDS_CLAIM, (int)app->call.kind);
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_START);
    TEST_ASSERT_EQUAL_INT(BATCH_NONE, (int)app->batch.step);
    env_state.hold_calls = 0;
    run_sends();
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
    TEST_PRINTF("then Download all counts the room as it is");
    start_download_all(SKIFF_APP_SCREEN_CONFIRM);
    TEST_ASSERT_EQUAL_size_t(SKIFF_JOBS_MAX - 1, app->batch.queue_room);
    TEST_ASSERT_TRUE(shows("Download 2 favourites"));
}

static void test_a_press_that_would_change_the_queue_waits_for_the_sends_queueing(void) {
    serve_register(1);
    serve_empty_claim()->max_uses = 1;
    const unsigned roms[] = {2};
    serve_claim(roms, NULL, 1)->max_uses = 1;
    serve_empty_claim();
    serve_rom(2);
    open_sends_library();
    open_details(1);
    TEST_PRINTF("the next round reaches its queue call");
    for (int i = 0; i < FRAMES_MAX && app->call.kind != CALL_SENDS_QUEUE; i++) {
        frame(0);
    }
    TEST_ASSERT_EQUAL_INT(CALL_SENDS_QUEUE, (int)app->call.kind);
    env_state.hold_calls = 1;
    TEST_PRINTF("Download meanwhile does nothing");
    frame(SKIFF_UI_ACTION_CONFIRM);
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_DETAILS, view()->screen);
    skiff_job jobs[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(0, list_jobs(jobs));
    env_state.hold_calls = 0;
    frame(0);
    TEST_ASSERT_EQUAL_size_t(1, list_jobs(jobs));
    TEST_PRINTF("the reports to RomM hold nothing of the player's, however long they take");
    TEST_ASSERT_EQUAL_INT(CALL_SENDS_REPORT, (int)app->call.kind);
    env_state.hold_calls = 1;
    frame(SKIFF_UI_ACTION_CONFIRM);
    TEST_ASSERT_EQUAL_size_t(2, list_jobs(jobs));
    TEST_ASSERT_EQUAL_INT(SKIFF_APP_SCREEN_LIBRARY, view()->screen);
    env_state.hold_calls = 0;
    run_sends();
    assert_reported(1, "done", NULL);
}

static void test_a_refusal_is_said_in_the_psps_language(void) {
    serve_register(1);
    const unsigned roms[] = {44};
    serve_claim(roms, NULL, 1)->max_uses = 1;
    serve_empty_claim();
    serve_sent_rom(44, OTHER_PLATFORM, "Sent 44.iso", BODY_BYTES, 1);
    write_sends_config(SENDS_SCOPES, "");
    serve_sends_heartbeat(SAVES_ROMM, "true");
    serve_platforms();
    serve_page(0, 3, 3);
    const skiff_app_config config = {.storage = &storage.base,
                                     .roots = roots,
                                     .clock = fake_utc,
                                     .language = SKIFF_LANGUAGE_SPANISH};
    const skiff_app_env env = make_env();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_app_create(&config, &env, &app));
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    assert_reported(1, "failed", "Ese juego no es de PSP [219]");
    TEST_ASSERT_EQUAL_STRING("Desde RomM: 0 añadidos, 1 no añadidos (mira RomM)", view()->status);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_first_launch_asks_for_the_server_then_pairs);
    RUN_TEST(test_a_slow_down_waits_five_seconds_more);
    RUN_TEST(test_the_first_free_space_query_is_made_while_starting);
    RUN_TEST(test_the_first_free_space_count_runs_off_the_screen_while_the_wifi_joins);
    RUN_TEST(test_a_failed_free_space_count_shows_none_and_is_asked_again_later);
    RUN_TEST(test_a_denied_or_expired_pairing_offers_a_new_code);
    RUN_TEST(test_an_address_too_long_for_a_qr_code_is_shown_as_text_only);
    RUN_TEST(test_the_longest_address_leaves_room_for_the_code_and_an_error);
    RUN_TEST(test_a_long_path_before_the_code_is_shown_whole);
    RUN_TEST(test_a_code_that_runs_out_on_the_psp_ends_the_pairing);
    RUN_TEST(test_an_old_romm_is_refused);
    RUN_TEST(test_a_newer_romm_is_noticed_once);
    RUN_TEST(test_a_bad_config_names_its_line_and_quits);
    RUN_TEST(test_without_ark_tls_refuses_and_says_why);
    RUN_TEST(test_the_wifi_switch_and_a_failed_join);
    RUN_TEST(test_a_refused_join_is_tried_again_before_the_player_sees_it);
    RUN_TEST(test_pages_load_as_the_player_scrolls);
    RUN_TEST(test_the_library_fills_the_body);
    RUN_TEST(test_a_library_without_psp_games_says_so);
    RUN_TEST(test_select_switches_the_library_to_favourites_and_back);
    RUN_TEST(test_favourites_without_any_say_where_to_mark_them);
    RUN_TEST(test_a_page_of_the_list_left_behind_is_dropped);
    RUN_TEST(test_back_from_a_favourites_details_returns_to_the_favourites);
    RUN_TEST(test_a_library_without_psp_games_offers_no_favourites);
    RUN_TEST(test_select_cycles_every_game_favourites_and_collections);
    RUN_TEST(test_a_collection_opens_its_psp_games_and_back_keeps_its_row);
    RUN_TEST(test_back_from_a_games_details_returns_to_its_collection);
    RUN_TEST(test_without_collections_say_where_to_make_them);
    RUN_TEST(test_a_token_without_collections_read_offers_pair_again);
    RUN_TEST(test_a_forbidden_collections_list_offers_pair_again);
    RUN_TEST(test_collections_too_large_for_the_psp_say_so_and_retry);
    RUN_TEST(test_a_collections_list_that_lands_after_leaving_is_dropped);
    RUN_TEST(test_download_all_in_a_collection_names_it);
    RUN_TEST(test_download_all_queues_every_favourite_with_one_save);
    RUN_TEST(test_download_all_leaves_out_installed_queued_changed_and_refused_games);
    RUN_TEST(test_download_all_with_a_full_queue_queues_nothing_and_says_why);
    RUN_TEST(test_download_all_takes_what_fits_the_free_space_in_name_order);
    RUN_TEST(test_download_all_keeps_room_for_installed_records);
    RUN_TEST(test_a_download_that_ends_while_favourites_are_checked_frees_its_room);
    RUN_TEST(test_a_game_that_cannot_be_planned_is_named_apart_from_a_full_queue);
    RUN_TEST(test_a_favourite_repeated_across_pages_is_taken_once);
    RUN_TEST(test_nothing_else_runs_while_download_all_queues);
    RUN_TEST(test_download_all_drops_a_library_page_still_loading);
    RUN_TEST(test_an_empty_favourite_cannot_be_installed_and_spoils_nothing);
    RUN_TEST(test_back_while_favourites_are_checked_stops_download_all);
    RUN_TEST(test_download_all_is_offered_only_on_favourites_with_games);
    RUN_TEST(test_a_failed_favourites_page_ends_download_all_with_its_error);
    RUN_TEST(test_a_batch_of_its_own_sorts_and_queues_apart_from_download_all);
    RUN_TEST(test_a_download_is_queued_into_the_iso_folder);
    RUN_TEST(test_a_finished_download_shows_as_installed);
    RUN_TEST(test_a_changed_game_and_a_hand_copy);
    RUN_TEST(test_a_full_queue_shows_its_hint);
    RUN_TEST(test_a_full_installed_list_shows_its_hint);
    RUN_TEST(test_the_downloads_screen_follows_the_worker);
    RUN_TEST(test_the_downloads_list_stays_put_as_the_details_come_and_go);
    RUN_TEST(test_every_error_fits_under_the_downloads_list);
    RUN_TEST(test_cancel_retry_and_clear_from_the_downloads_screen);
    RUN_TEST(test_downloading_lasts_while_the_worker_has_a_download_to_run);
    RUN_TEST(test_saves_is_set_apart_from_the_server_rows_in_settings);
    RUN_TEST(test_a_new_server_stops_the_worker_and_asks_to_pair);
    RUN_TEST(test_a_worker_that_will_not_stop_changes_nothing);
    RUN_TEST(test_an_address_without_a_scheme_is_refused);
    RUN_TEST(test_a_new_server_cancels_the_old_downloads);
    RUN_TEST(test_a_failed_page_can_be_retried);
    RUN_TEST(test_the_library_answers_while_a_page_loads);
    RUN_TEST(test_leaving_a_game_while_it_loads_drops_its_details);
    RUN_TEST(test_a_game_shows_its_cover_and_keeps_it_on_the_memory_stick);
    RUN_TEST(test_a_game_without_a_usable_cover_keeps_its_placeholder);
    RUN_TEST(test_long_names_beside_the_cover_leave_room_for_the_details);
    RUN_TEST(test_leaving_a_game_while_its_cover_loads_drops_the_cover);
    RUN_TEST(test_a_cover_that_lands_under_a_confirmation_is_kept);
    RUN_TEST(test_a_cover_lost_with_the_network_shows_no_error);
    RUN_TEST(test_a_lost_connection_under_a_dropped_request_still_counts);
    RUN_TEST(test_a_page_asked_for_while_another_loaded_is_dropped_once_off_screen);
    RUN_TEST(test_a_new_server_drops_what_the_old_one_was_still_sending);
    RUN_TEST(test_a_request_done_before_its_start_returns_keeps_its_error);
    RUN_TEST(test_a_request_that_cannot_start_shows_why);
    RUN_TEST(test_leaving_a_pairing_drops_its_poll_and_wipes_its_codes);
    RUN_TEST(test_a_lost_network_is_joined_again_before_retrying);
    RUN_TEST(test_cancelling_pair_again_returns_to_the_library);
    RUN_TEST(test_a_server_change_that_cannot_be_saved_keeps_the_old_server);
    RUN_TEST(test_a_failed_details_request_is_retried_for_the_same_rom);
    RUN_TEST(test_a_lost_connection_joins_again_then_opens_the_same_rom);
    RUN_TEST(test_a_worker_that_cannot_start_is_shown);
    RUN_TEST(test_a_new_server_forgets_the_old_installs);
    RUN_TEST(test_an_abandoned_retry_is_forgotten);
    RUN_TEST(test_a_new_code_after_a_lost_connection_joins_first);
    RUN_TEST(test_queued_downloads_hold_their_installed_records);
    RUN_TEST(test_a_cancelled_retry_needs_room_in_the_installed_list);
    RUN_TEST(test_another_network_restarts_the_worker_on_it);
    RUN_TEST(test_installed_json_is_saved_without_the_lock_the_ui_reads);
    RUN_TEST(test_secrets_and_the_log_level);
    RUN_TEST(test_transport_settings_come_from_the_skiff_folder);
    RUN_TEST(test_an_unset_ca_file_trusts_the_bundled_cas);
    RUN_TEST(test_the_app_browses_and_downloads_with_the_bundled_cas);
    RUN_TEST(test_wrapping_breaks_between_words_and_inside_long_ones);
    RUN_TEST(test_bad_arguments_and_locks);
    RUN_TEST(test_the_launch_check_syncs_what_needs_no_choice_and_counts_the_rest);
    RUN_TEST(test_a_conflict_shows_both_saves_and_keep_this_psps_overwrites);
    RUN_TEST(test_use_romms_restores_it_after_a_backup);
    RUN_TEST(test_romms_409_turns_an_upload_into_a_question);
    RUN_TEST(test_a_save_deleted_on_this_psp_asks_and_can_be_left_deleted);
    RUN_TEST(test_without_the_save_scopes_saves_offer_pair_again);
    RUN_TEST(test_a_forbidden_save_call_offers_pair_again);
    RUN_TEST(test_romm_5_3_browses_but_says_save_sync_needs_5_4);
    RUN_TEST(test_a_check_that_times_out_says_so_and_retries);
    RUN_TEST(test_a_restore_cut_short_is_settled_before_anything_with_a_notice);
    RUN_TEST(test_a_games_details_show_its_save_and_sync_it);
    RUN_TEST(test_a_details_sync_that_lands_after_leaving_does_nothing_more);
    RUN_TEST(test_a_new_server_forgets_every_baseline_and_save_id);
    RUN_TEST(test_the_saves_row_waits_for_the_connection);
    RUN_TEST(test_the_saves_screen_before_any_server_says_why_it_is_empty);
    RUN_TEST(test_rows_say_why_a_game_cannot_sync_and_one_twin_is_chosen);
    RUN_TEST(test_a_game_without_a_save_id_says_so_on_its_details);
    RUN_TEST(test_a_save_that_fails_romms_checksum_is_never_restored);
    RUN_TEST(test_a_lost_connection_stops_the_batch_for_the_next_check);
    RUN_TEST(test_a_suspend_joins_the_wifi_again_before_the_next_save_call);
    RUN_TEST(test_a_save_download_cut_by_a_suspend_runs_again_after_joining);
    RUN_TEST(test_a_save_download_cut_by_a_second_suspend_says_the_connection_was_lost);
    RUN_TEST(test_without_a_suspend_a_memory_stick_fault_stays_one);
    RUN_TEST(test_a_batch_upload_cut_by_a_suspend_counts_as_a_lost_connection);
    RUN_TEST(test_a_table_added_to_the_view_starts_empty);
    RUN_TEST(test_a_transfer_cut_by_a_suspend_after_leaving_keeps_the_players_screen);
    RUN_TEST(test_a_pairing_approved_across_a_suspend_still_pairs);
    RUN_TEST(test_a_suspend_mid_connect_joins_again_before_the_next_step);
    RUN_TEST(test_a_retry_dropped_by_a_blocked_sync_leaves_the_next_transfer_its_own);
    RUN_TEST(test_a_retried_transfer_waits_until_the_wifi_is_joined);
    RUN_TEST(test_a_restore_cut_by_a_suspend_is_settled_once_the_stick_is_back);
    RUN_TEST(test_a_settle_that_keeps_failing_stops_save_sync_until_one_works);
    RUN_TEST(test_a_suspend_on_the_pair_screen_joins_again_and_keeps_the_code);
    RUN_TEST(test_leaving_while_a_cut_restore_settles_drops_the_automatic_retry);
    RUN_TEST(test_a_cut_restore_is_settled_while_the_wifi_is_still_joining);
    RUN_TEST(test_a_restore_that_cannot_be_finished_at_launch_stops_save_sync);
    RUN_TEST(test_a_new_server_waits_until_the_old_baselines_are_gone);
    RUN_TEST(test_a_transfer_that_ends_after_leaving_keeps_the_players_screen);
    RUN_TEST(test_a_restore_cut_mid_way_is_settled_and_stops_save_sync);
    RUN_TEST(test_leave_it_compares_again_when_the_save_came_back);
    RUN_TEST(test_a_save_id_learnt_from_details_reaches_saves_json);
    RUN_TEST(test_a_baseline_the_memory_stick_refused_is_written_by_the_next_call);
    RUN_TEST(test_a_roms_details_skiff_refuses_cost_only_that_game);
    RUN_TEST(test_a_save_deleted_in_romm_asks_and_says_removing_deletes_it_here);
    RUN_TEST(test_many_games_are_negotiated_in_rounds);
    RUN_TEST(test_sync_all_asks_again_about_a_game_once_without_a_save_id);
    RUN_TEST(test_installed_games_past_the_lookup_budget_are_reached_next_time);
    RUN_TEST(test_another_rom_with_the_same_save_id_starts_as_a_first_sync);
    RUN_TEST(test_the_status_takes_the_width_the_title_leaves);
    RUN_TEST(test_a_long_name_is_shortened_so_the_action_stays);
    RUN_TEST(test_a_long_names_sync_note_says_synced_first);
    RUN_TEST(test_saves_that_need_the_player_are_flagged_until_chosen);
    RUN_TEST(test_the_choice_fits_in_spanish);
    RUN_TEST(test_a_rom_read_twice_across_shifted_pages_is_one_game);
    RUN_TEST(test_a_game_sent_from_romm_is_added_right_after_connecting);
    RUN_TEST(test_claims_repeat_every_interval_and_empty_ones_leave_no_trace);
    RUN_TEST(test_sends_wait_for_pages_download_all_dialogs_server_changes_and_pairing);
    RUN_TEST(test_a_lost_connection_pauses_claims_until_the_wifi_is_joined_again);
    RUN_TEST(test_a_refused_claim_stops_sends_and_settings_offers_pair_again);
    RUN_TEST(test_a_device_romm_no_longer_knows_gets_no_claim_until_the_next_connection);
    RUN_TEST(test_no_request_on_romm_5_3_turned_off_without_devices_write_or_switched_off);
    RUN_TEST(test_sends_romm_cannot_describe_or_skiff_cannot_install_are_refused_with_why);
    RUN_TEST(test_sends_follow_download_alls_rules_for_what_this_psp_holds);
    RUN_TEST(test_sends_past_the_free_space_fail_in_claim_order);
    RUN_TEST(test_a_send_to_a_full_download_list_says_so);
    RUN_TEST(test_a_send_past_the_installed_records_says_so);
    RUN_TEST(test_a_queue_that_cannot_be_saved_fails_each_send_with_why);
    RUN_TEST(test_a_round_queues_with_one_save_and_reports_only_after_it);
    RUN_TEST(test_a_send_seen_again_finds_its_game_in_downloads);
    RUN_TEST(test_a_send_whose_game_is_lost_with_the_connection_stays_in_romm);
    RUN_TEST(test_switching_games_from_romm_off_tells_romm_and_stops_claims);
    RUN_TEST(test_a_failed_switch_off_is_tried_again_on_the_next_connection);
    RUN_TEST(test_switched_off_tells_romm_once_per_connection_and_never_claims);
    RUN_TEST(test_claims_go_on_once_something_else_joins_the_wifi_again);
    RUN_TEST(test_an_unreachable_romm_on_a_joined_wifi_keeps_the_claim_interval);
    RUN_TEST(test_download_all_waits_for_a_round_that_may_take_its_room);
    RUN_TEST(test_a_press_that_would_change_the_queue_waits_for_the_sends_queueing);
    RUN_TEST(test_a_refusal_is_said_in_the_psps_language);
    return UNITY_END();
}
