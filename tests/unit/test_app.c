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

static skiff_app_env make_env(void) {
    const skiff_app_env env = {
        .ctx = &env_state,
        .now_ms = env_now,
        .measure = env_measure,
        .switch_on = env_switch_on,
        .net_start = env_net_start,
        .net_poll = env_net_poll,
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

static void frame(unsigned actions) {
    skiff_app_update(app, actions);
    env_state.now_ms += FRAME_MS;
}

static const char *screen_name(skiff_app_screen screen) {
    static const char *const NAMES[] = {"starting", "server", "connecting", "pair",    "library",
                                        "details",  "queue",  "settings",   "message", "confirm"};
    return NAMES[screen];
}

static void print_view(void) {
    const skiff_app_view *v = view();
    TEST_PRINTF("[%s] %s | %s", screen_name(v->screen), v->title, v->status);
    for (size_t i = 0; i < v->line_count; i++) {
        TEST_PRINTF("   %s", v->lines[i]);
    }
    for (size_t i = 0; i < v->row_count; i++) {
        TEST_PRINTF("  %c %s | %s%s", v->list.first + i == v->list.selected ? '>' : ' ',
                    v->rows[i].label, v->rows[i].detail, v->rows[i].dim ? " (dim)" : "");
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

static void open_paired_library(unsigned total) {
    write_config(
        "[server]\nurl = " SERVER "\n[auth]\ntoken = " TOKEN
        "\ndevice_identifier = 00112233445566778899aabbccddeeff\n[network]\nprofile = 1\n");
    serve_library(total);
    create_app();
    run_until(SKIFF_APP_SCREEN_LIBRARY);
}

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
    TEST_PRINTF("the first query counts the whole Memory Stick (2.6 s on a PSP): made at startup, "
                "not when a screen that shows free space first opens");
    write_config("[server]\nurl = " SERVER "\n[network]\nprofile = 1\n");
    serve_heartbeat("5.3.1");
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_fixture(&transport, PATH_INIT, "romm/device-init.http"));
    create_app();
    run_until(SKIFF_APP_SCREEN_PAIR);
    TEST_PRINTF("free space queries before any screen showed it: %d", storage.free_space_queries);
    TEST_ASSERT_EQUAL_INT(1, storage.free_space_queries);
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
    serve_heartbeat("5.4.0");
    serve_platforms();
    serve_page(0, 1, 1);
    create_app();
    run_until(SKIFF_APP_SCREEN_MESSAGE);
    TEST_ASSERT_TRUE(shows("RomM 5.4.0 is newer"));
    frame(SKIFF_UI_ACTION_CONFIRM);
    run_until(SKIFF_APP_SCREEN_LIBRARY);
    char config[TEXT_MAX];
    read_app_file(SKIFF_CONFIG_FILE_NAME, config, sizeof config);
    TEST_ASSERT_NOT_NULL(strstr(config, "romm_notice = 5.4"));
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
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_SELECT, english(SKIFF_TEXT_SHOW_ALL_GAMES)));

    TEST_PRINTF("SELECT again: every game, from their first page again");
    const size_t requests = transport.request_count;
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
    TEST_ASSERT_TRUE(hints(SKIFF_UI_ACTION_SELECT, english(SKIFF_TEXT_SHOW_ALL_GAMES)));
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
    TEST_PRINTF("back to every game while the favourites' first page is on its way");
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
    storage.free_bytes = SKIFF_STORAGE_FREE_MARGIN_BYTES + 2 * BODY_BYTES + BODY_BYTES / 2;
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
    storage.free_bytes = SKIFF_STORAGE_FREE_MARGIN_BYTES + 3 * BODY_BYTES + BODY_BYTES / 2;
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
    TEST_ASSERT_TRUE(app->favourites);
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

/* ---- Settings ---- */

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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_first_launch_asks_for_the_server_then_pairs);
    RUN_TEST(test_a_slow_down_waits_five_seconds_more);
    RUN_TEST(test_the_first_free_space_query_is_made_while_starting);
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
    RUN_TEST(test_back_while_favourites_are_checked_stops_download_all);
    RUN_TEST(test_download_all_is_offered_only_on_favourites_with_games);
    RUN_TEST(test_a_failed_favourites_page_ends_download_all_with_its_error);
    RUN_TEST(test_a_download_is_queued_into_the_iso_folder);
    RUN_TEST(test_a_finished_download_shows_as_installed);
    RUN_TEST(test_a_changed_game_and_a_hand_copy);
    RUN_TEST(test_a_full_queue_shows_its_hint);
    RUN_TEST(test_a_full_installed_list_shows_its_hint);
    RUN_TEST(test_the_downloads_screen_follows_the_worker);
    RUN_TEST(test_the_downloads_list_stays_put_as_the_details_come_and_go);
    RUN_TEST(test_every_error_fits_under_the_downloads_list);
    RUN_TEST(test_cancel_retry_and_clear_from_the_downloads_screen);
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
    return UNITY_END();
}
