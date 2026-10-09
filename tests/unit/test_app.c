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

/* ROM id's list item: "Game <id>.iso", or an unusable name when id is 13. */
static int rom_item(char *out, size_t size, unsigned id) {
    if (id == 13) {
        return snprintf(out, size,
                        "{\"id\":13,\"platform_id\":1,\"name\":\"Bad\\u0007Name\",\"fs_name\":"
                        "\"Bad\\u0007Name.iso\",\"fs_size_bytes\":%u}",
                        BODY_BYTES);
    }
    return snprintf(out, size,
                    "{\"id\":%u,\"platform_id\":1,\"name\":\"Game %u\",\"fs_name\":\"Game %u.iso\","
                    "\"fs_size_bytes\":%u,\"crc_hash\":\"%08x\"}",
                    id, id, id, BODY_BYTES, (unsigned)body_crc);
}

/* A page of ROMs with ids from first_id, for a library of total. */
static void serve_page(unsigned offset, unsigned count, unsigned total) {
    static char raw[RAW_MAX];
    int used = snprintf(raw, sizeof raw, JSON_OK "{\"items\":[");
    for (unsigned i = 0; i < count; i++) {
        used += snprintf(raw + used, sizeof raw - (size_t)used, i > 0 ? "," : "");
        used += rom_item(raw + used, sizeof raw - (size_t)used, offset + i + 1);
    }
    snprintf(raw + used, sizeof raw - (size_t)used, "],\"total\":%u,\"limit\":%d,\"offset\":%u}",
             total, SKIFF_ROMM_PAGE_SIZE, offset);
    char path[256];
    snprintf(path, sizeof path, "/api/roms?platform_ids=1&limit=%d&offset=%u" LIST_QUERY,
             SKIFF_ROMM_PAGE_SIZE, offset);
    serve_raw(path, raw);
}

static void serve_rom(unsigned id) {
    char raw[2048];
    char item[512];
    rom_item(item, sizeof item, id);
    /* The item without its closing brace, then its files. */
    item[strlen(item) - 1] = '\0';
    snprintf(raw, sizeof raw,
             JSON_OK "%s,\"files\":[{\"rom_id\":%u,\"file_name\":\"Game %u.iso\","
                     "\"file_size_bytes\":%u,\"crc_hash\":\"%08x\"}]}",
             item, id, id, BODY_BYTES, (unsigned)body_crc);
    char path[64];
    snprintf(path, sizeof path, "/api/roms/%u", id);
    serve_raw(path, raw);
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
            app->request == REQUEST_NONE && !app->announced && pairing_settled) {
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

static void record_installed(unsigned id, uint32_t crc) {
    char iso[TEMP_DIR_PATH_MAX];
    char name[64];
    snprintf(name, sizeof name, "ISO/Game %u.iso", id);
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO", iso, sizeof iso));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, iso));
    TEST_ASSERT_TRUE(temp_dir_path(dir, name, iso, sizeof iso));
    write_file(iso, (const char *)body, BODY_BYTES);
    skiff_install_manifest *manifest = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_create(&manifest));
    skiff_install_record record;
    memset(&record, 0, sizeof record);
    record.rom_id = id;
    snprintf(record.file_name, sizeof record.file_name, "Game %u.iso", id);
    snprintf(record.path, sizeof record.path, "games:/Game %u.iso", id);
    record.size = BODY_BYTES;
    record.has_crc32 = 1;
    record.crc32 = crc;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_record(manifest, &record));
    char path[TEMP_DIR_PATH_MAX];
    app_file(SKIFF_INSTALL_MANIFEST_NAME, path, sizeof path);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_install_manifest_save(manifest, posix, path));
    skiff_install_manifest_destroy(manifest);
}

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

static void write_full_queue(void) {
    static skiff_job jobs[SKIFF_JOBS_MAX];
    memset(jobs, 0, sizeof jobs);
    for (unsigned i = 0; i < SKIFF_JOBS_MAX; i++) {
        jobs[i].id = i + 1;
        jobs[i].rom_id = 100 + i;
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
    event.step = SKIFF_JOBS_WAITING_FOR_WIFI;
    app_queue_event(app, &event);
    frame(0);
    TEST_ASSERT_TRUE(shows(english(SKIFF_TEXT_QUEUE_WAITING_WIFI)));
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
    TEST_PRINTF("a RomM error needs no new join");
    TEST_ASSERT_EQUAL_INT(joins, env_state.net_starts);
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
    frame(SKIFF_UI_ACTION_START);
    TEST_ASSERT_TRUE(skiff_app_quit_requested(app));
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
    RUN_TEST(test_pages_load_as_the_player_scrolls);
    RUN_TEST(test_a_library_without_psp_games_says_so);
    RUN_TEST(test_a_download_is_queued_into_the_iso_folder);
    RUN_TEST(test_a_finished_download_shows_as_installed);
    RUN_TEST(test_a_changed_game_and_a_hand_copy);
    RUN_TEST(test_a_full_queue_shows_its_hint);
    RUN_TEST(test_a_full_installed_list_shows_its_hint);
    RUN_TEST(test_the_downloads_screen_follows_the_worker);
    RUN_TEST(test_cancel_retry_and_clear_from_the_downloads_screen);
    RUN_TEST(test_a_new_server_stops_the_worker_and_asks_to_pair);
    RUN_TEST(test_a_worker_that_will_not_stop_changes_nothing);
    RUN_TEST(test_an_address_without_a_scheme_is_refused);
    RUN_TEST(test_a_new_server_cancels_the_old_downloads);
    RUN_TEST(test_a_failed_page_can_be_retried);
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
