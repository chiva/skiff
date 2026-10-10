#ifndef SKIFF_APP_INTERNAL_H
#define SKIFF_APP_INTERNAL_H

/* What the app's files share: the app object behind skiff/app.h's opaque type. */

#include "skiff/app.h"
#include "skiff/install.h"
#include "skiff/romm_pairing.h"

#define APP_QUEUE_FILE "app:/" SKIFF_JOBS_FILE_NAME
#define APP_CONFIG_FILE "app:/" SKIFF_CONFIG_FILE_NAME
#define APP_LOG_FILE "app:/" SKIFF_APP_LOG_FILE_NAME
#define APP_MANIFEST_FILE "app:/" SKIFF_INSTALL_MANIFEST_NAME
#define APP_MS_PER_S 1000
#define APP_BYTES_PER_MIB (1024ULL * 1024ULL)
/* What the keyboard starts with when there is no address yet. */
#define APP_URL_PREFILL "https://"
/* The rows each list shows. The library's fills the body. Downloads keeps room under its list for
 * the selected download's details (progress, speed and recovery lines, or its error) and the
 * progress bar, shown or not. Settings has the version, RomM and free-space lines over its list. */
#define APP_LIBRARY_ROWS SKIFF_APP_ROWS_FIT(0, 0)
#define APP_QUEUE_DETAIL_LINES 3
#define APP_QUEUE_ROWS SKIFF_APP_ROWS_FIT(APP_QUEUE_DETAIL_LINES, 1)
#define APP_SETTINGS_LINES 3
#define APP_SETTINGS_ROWS SKIFF_APP_ROWS_FIT(APP_SETTINGS_LINES, 0)
/* Lines a game's title and its file name each take at most on the details screen, beside the
 * cover: the size, the download state and the free space always fit under them. */
#define APP_DETAILS_NAME_LINES 3

/* Where the walk from a launch to the library is. */
typedef enum app_connect_step {
    CONNECT_NETWORK,
    CONNECT_JOINING,
    CONNECT_TLS,
    CONNECT_HEARTBEAT,
    CONNECT_PAIRING,
    CONNECT_PLATFORM,
    CONNECT_DONE,
} app_connect_step;

/* What OK (and the other choice) does on a message. */
typedef enum app_message_action {
    MESSAGE_BACK,
    MESSAGE_RETRY_CONNECT,
    /* The page or ROM that failed to load, joining the Wi-Fi first when it was lost. */
    MESSAGE_RETRY_REQUEST,
    MESSAGE_RETRY_WORKER,
    /* Carry on connecting from where it stopped (after a change that was undone). */
    MESSAGE_RESUME,
    MESSAGE_PICK_NETWORK,
    MESSAGE_NOTICE_SEEN,
    MESSAGE_NEW_PAIRING,
    MESSAGE_SETTINGS,
    MESSAGE_QUIT,
    /* No second choice. */
    MESSAGE_NONE,
} app_message_action;

typedef enum app_confirm_action {
    CONFIRM_REPLACE,
    CONFIRM_CANCEL_JOB,
    CONFIRM_SERVER_CHANGE,
} app_confirm_action;

/* A request to RomM, made on the frame after the one that said it was coming. */
typedef enum app_request {
    REQUEST_NONE,
    REQUEST_PAGE,
    REQUEST_ROM,
    /* The ROM's cover, once its details are in. */
    REQUEST_COVER,
} app_request;

/* A request to RomM, which runs off the screen's thread (env.call_start), one at a time. Its inputs
 * are set before it starts and its results read once it is done (app_call_update()), so the thread
 * touches nothing of the app's but app->call and the browse client, which nothing else uses
 * meanwhile: while app_call_busy(), the client is not replaced (app_drop_client() waits). */
typedef enum app_call_kind {
    CALL_NONE,
    CALL_HEARTBEAT,
    CALL_PLATFORM,
    CALL_PAGE,
    CALL_ROM,
    CALL_PAIRING_START,
    CALL_PAIRING_POLL,
    /* The details screen's cover, from the Memory Stick cache or RomM (decoded and cached). */
    CALL_COVER,
} app_call_kind;

typedef struct app_call {
    skiff_romm_client *romm;
    /* In: the platform and page to list, the ROM to fetch. */
    uint64_t platform_id;
    uint64_t page_index;
    uint64_t rom_id;
    /* Out: what the request returned. */
    skiff_romm_server server;
    skiff_romm_platform platform;
    skiff_romm_rom_page page;
    skiff_romm_rom rom;
    /* In and out: a poll reads the pairing and may lengthen its interval; the approval's token.
     * Wiped once used (skiff_romm_pairing_clear()). */
    skiff_romm_pairing pairing;
    skiff_romm_pairing_result pairing_result;
    char device_identifier[SKIFF_CONFIG_DEVICE_IDENTIFIER_MAX];
    /* A cover: in, where it is, its cache key, the Memory Stick and a clock; out, the cover
     * decoded into the app's spare buffer, and how it went (for the log). */
    char cover_path[SKIFF_ROMM_COVER_PATH_MAX];
    skiff_cover_key cover_key;
    skiff_storage *storage;
    skiff_storage_roots roots;
    int64_t (*now_ms)(void *ctx);
    void *clock_ctx;
    skiff_cover *cover;
    int cover_cached;
    size_t cover_bytes;
    int64_t cover_fetch_ms;
    int64_t cover_decode_ms;
    skiff_err cover_cache_err;
    skiff_err cover_store_err;
    skiff_err err;
    app_call_kind kind;
    /* It could not start: err says why, and the next update applies it as the request's error. */
    int failed_to_start;
    /* The player left what it was for: its result is dropped. */
    int abandoned;
    /* The browse client is replaced once it is done (a lost connection, a new server). */
    int drop_client;
} app_call;

typedef struct app_message {
    skiff_text_id title;
    size_t line_count;
    char lines[SKIFF_APP_LINES_MAX][SKIFF_TEXT_MAX];
    app_message_action ok;
    app_message_action other;
    skiff_text_id other_label;
    /* Where MESSAGE_BACK returns. */
    skiff_app_screen back;
} app_message;

typedef struct app_page {
    int valid;
    uint64_t index;
    /* Bumped on use; the oldest is replaced. */
    uint64_t used;
    skiff_romm_rom_page page;
    char labels[SKIFF_ROMM_PAGE_SIZE][SKIFF_APP_LABEL_MAX];
} app_page;

typedef struct app_pairing {
    int active;
    skiff_romm_pairing pairing;
    int64_t started_ms;
    int64_t next_poll_ms;
    /* The pairing ended with this error (SKIFF_OK while it runs); the last poll's error, which
     * the next poll may clear. */
    skiff_err ended;
    skiff_err last_error;
    /* The address with the code, as a QR code; size 0 when it does not fit one. */
    skiff_ui_qr qr;
} app_pairing;

/* What the queue screen shows: a copy of the queue and the active job's progress. */
typedef struct app_queue_view {
    size_t count;
    skiff_job jobs[SKIFF_JOBS_MAX];
    char labels[SKIFF_JOBS_MAX][SKIFF_APP_LABEL_MAX];
    uint32_t progress_job;
    skiff_ui_progress progress;
    int has_recovery;
    skiff_jobs_event recovery;
} app_queue_view;

/* Fields are grouped by size (clang-tidy's padding check); see the comments for what goes
 * together. */
struct skiff_app {
    /* What it was made with, and what the PSP is shown. */
    skiff_app_config config;
    skiff_app_env env;
    skiff_app_view view;
    int64_t last_refresh_ms;

    /* config.ini: its text as on the Memory Stick, and parsed. */
    size_t config_length;
    skiff_config settings;

    skiff_log *log;
    skiff_jobs *jobs;
    skiff_install_manifest *manifest;
    const skiff_installer *installer;

    /* Browsing: the client the calls use, and the call running or just done. */
    skiff_transport *transport;
    skiff_app_transport_settings transport_settings;
    skiff_romm_client romm;
    app_call call;

    /* The worker's own copies: they must not change while it runs. */
    skiff_config worker_settings;
    skiff_app_transport_settings worker_transport;
    skiff_romm_client worker_romm;

    app_message message;
    app_pairing pairing;

    /* The library. */
    skiff_romm_platform platform;
    uint64_t total;
    uint64_t page_clock;
    uint64_t request_page;
    /* What to load again when the player retries. */
    uint64_t failed_page;
    int64_t note_until_ms;
    skiff_ui_list library;
    app_page pages[SKIFF_APP_CACHED_PAGES];
    /* The ROM on the details screen. */
    skiff_romm_rom rom;
    /* Its cover (when cover_key is its key, see app_cover_shown()), and the buffer the next one is
     * decoded into. */
    skiff_cover *cover;
    skiff_cover *cover_spare;
    skiff_cover_key cover_key;
    /* Free space on the Memory Stick, read when a screen showing it opens. */
    uint64_t free_bytes;

    app_queue_view queue;
    skiff_ui_list queue_list;
    skiff_ui_list settings_list;

    /* Flags and small values. */
    int dirty;
    int quit;
    int has_server;
    int net_joined;
    int tls_started;
    int profile;
    /* Waiting for the Wi-Fi switch before joining. */
    int waiting_switch;
    /* Joins retried since the last success or shown error, and when the next may start. */
    int join_retries;
    int64_t join_retry_ms;
    /* Frames the starting screen has been drawn (SKIFF_APP_STARTING_FRAMES). */
    int starting_frames;
    int worker_running;
    /* The connection the running worker rejoins. */
    int worker_profile;
    skiff_app_screen screen;
    app_connect_step connect;
    skiff_app_dialog dialog;
    app_confirm_action confirm;
    skiff_text_id confirm_text;
    uint32_t confirm_job;
    /* The screen the keyboard edits the server address for. */
    skiff_app_screen keyboard_from;
    int has_platform;
    int total_known;
    app_request request;
    app_request failed_request;
    int has_rom;
    /* app->cover holds the cover cover_key names. */
    int has_cover;
    /* A new pairing waits for the Wi-Fi to be joined again. */
    int pairing_requested;
    int free_known;

    skiff_romm_server server;
    char profile_name[SKIFF_APP_TITLE_MAX];
    char note[SKIFF_TEXT_MAX];
    /* An address waiting for the player to confirm that downloads will be cancelled. */
    char pending_url[SKIFF_CONFIG_URL_MAX];
    char config_text[SKIFF_CONFIG_TEXT_MAX + 1];
    char edit_text[SKIFF_CONFIG_TEXT_MAX + 1];
};

/* ---- app.c ---- */

void app_set_screen(skiff_app *app, skiff_app_screen screen);
/* Shows err's sentence and code (and the config.ini line for a config error) with title. */
void app_show_error(skiff_app *app, skiff_err err, const skiff_config_issue *issue,
                    app_message_action ok, app_message_action other, skiff_text_id other_label,
                    skiff_app_screen back);
void app_show_text(skiff_app *app, skiff_text_id title, const char *text, app_message_action ok,
                   skiff_app_screen back);
void app_confirm(skiff_app *app, skiff_text_id question, app_confirm_action action, uint32_t job);
/* Sets key in section of config.ini, saves the file and parses it again. */
skiff_err app_config_set(skiff_app *app, const char *section, const char *key, const char *value,
                         skiff_config_issue *issue);
skiff_err app_config_replace(skiff_app *app, const char *text, size_t length);
void app_note(skiff_app *app, const char *text);
int64_t app_now(const skiff_app *app);
void app_lock_manifest(skiff_app *app);
void app_unlock_manifest(skiff_app *app);
/* Stops the worker for a change of server or token; 0 when it would not stop. */
int app_stop_worker(skiff_app *app);
skiff_err app_start_worker(skiff_app *app);
/* Drops what was loaded from the old server, and the queue, so they are made again. */
skiff_err app_reset_server(skiff_app *app);

/* ---- app_connect.c ---- */

void app_connect_begin(skiff_app *app, app_connect_step from);
void app_connect_update(skiff_app *app, unsigned actions);
void app_pair_update(skiff_app *app, unsigned actions);
void app_pair_begin(skiff_app *app);
void app_network_picked(skiff_app *app, skiff_app_dialog_result result);
/* A request failed with err: the network may be gone, so the next connect joins first. */
void app_network_failed(skiff_app *app, skiff_err err);
/* An error of the Wi-Fi or the connection, not of RomM. */
int app_is_network_error(skiff_err err);
/* Connected again: the library, or the details request that failed before. */
void app_resume_after_connect(skiff_app *app);
/* The browse transport and RomM client, made when missing; SKIFF_OK or why not. */
skiff_err app_ensure_client(skiff_app *app);
/* Drops the browse transport and RomM client, now or, while a call uses them, once it is done. */
void app_drop_client(skiff_app *app);
/* What a finished call of theirs returned (in app->call), applied on the screen's thread. */
void app_heartbeat_done(skiff_app *app);
void app_platform_done(skiff_app *app);
void app_pairing_started(skiff_app *app);
void app_pairing_polled(skiff_app *app);

/* ---- app_call.c ---- */

int app_call_busy(const skiff_app *app);
/* Runs app->call as kind, its inputs set, once the browse client is up. A call that cannot start
 * (no client, or the platform refused) still ends through app_call_update(), with its error. Only
 * when !app_call_busy(). */
void app_call_start(skiff_app *app, app_call_kind kind);
/* The player left what the running call was for: its result will be dropped, and its transfers are
 * asked to stop. */
void app_call_abandon(skiff_app *app);
/* Once a frame, before anything else: applies the result of a call that has finished. */
void app_call_update(skiff_app *app);

/* ---- app_library.c ---- */

void app_library_open(skiff_app *app);
void app_library_update(skiff_app *app, unsigned actions);
void app_library_refresh_markers(skiff_app *app);
void app_details_update(skiff_app *app, unsigned actions);
void app_download_confirmed(skiff_app *app);
/* Starts the page or ROM request waiting in app->request, once no call runs. */
void app_request_run(skiff_app *app);
void app_page_done(skiff_app *app);
void app_rom_done(skiff_app *app);
void app_cover_done(skiff_app *app);
/* app->cover is the cover of the ROM on the details screen as the server names it now: the same
 * server, ROM and cover path ("?ts=" included). */
int app_cover_shown(const skiff_app *app);
/* Why the ROM on the details screen cannot be downloaded, or SKIFF_TEXT_COUNT when it can. */
skiff_text_id app_details_refusal(const skiff_app *app);
/* Whether a list item is one Skiff can download (dimmed otherwise). */
int app_rom_downloadable(const skiff_app *app, const skiff_romm_rom_summary *rom);
/* installed.json can still record rom_id's file_name, counting the records queued downloads will
 * need (takes the manifest lock). */
int app_installed_room(skiff_app *app, uint64_t rom_id, const char *file_name);

/* ---- app_queue.c ---- */

void app_queue_refresh(skiff_app *app);
void app_queue_event(skiff_app *app, const skiff_jobs_event *event);
void app_queue_update(skiff_app *app, unsigned actions);
void app_cancel_confirmed(skiff_app *app, uint32_t id);
/* Jobs queued, active or failed: what counts against SKIFF_JOBS_MAX. */
size_t app_queue_unfinished(const skiff_app *app);
/* The unfinished job for rom_id, or NULL. */
const skiff_job *app_queue_job_for(const skiff_app *app, uint64_t rom_id);

/* ---- app_settings.c ---- */

void app_settings_update(skiff_app *app, unsigned actions);
void app_server_entered(skiff_app *app, const char *url);
void app_server_change_confirmed(skiff_app *app);

/* ---- app_view.c ---- */

void app_view_build(skiff_app *app);
const char *app_text(const skiff_app *app, skiff_text_id id);
/* Fills template with args into out (SKIFF_TEXT_MAX). */
void app_format(const skiff_app *app, skiff_text_id id, const char *const *args, size_t count,
                char *out);
void app_format_bytes(const skiff_app *app, uint64_t bytes, char *out, size_t out_size);
void app_fit(const skiff_app *app, const char *text, float width, char *out, size_t out_size);
/* The detail a ROM shows in the library: its download or its install state; "" for none. */
void app_rom_detail(skiff_app *app, const skiff_romm_rom_summary *rom, char *out, size_t size);

#endif
