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
/* What the keyboard starts with when there is no address yet. */
#define APP_URL_PREFILL "https://"

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
} app_request;

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

    /* Browsing, on this thread. */
    skiff_transport *transport;
    skiff_app_transport_settings transport_settings;
    skiff_romm_client romm;

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
    int worker_running;
    skiff_app_screen screen;
    /* A frame has shown what the next step waits for: the step may now block. */
    int announced;
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
/* The browse transport and RomM client, made when missing; SKIFF_OK or why not. */
skiff_err app_ensure_client(skiff_app *app);

/* ---- app_library.c ---- */

void app_library_open(skiff_app *app);
void app_library_update(skiff_app *app, unsigned actions);
void app_library_refresh_markers(skiff_app *app);
void app_details_update(skiff_app *app, unsigned actions);
void app_download_confirmed(skiff_app *app);
void app_request_run(skiff_app *app);
/* Why the ROM on the details screen cannot be downloaded, or SKIFF_TEXT_COUNT when it can. */
skiff_text_id app_details_refusal(const skiff_app *app);
/* Whether a list item is one Skiff can download (dimmed otherwise). */
int app_rom_downloadable(const skiff_app *app, const skiff_romm_rom_summary *rom);

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
