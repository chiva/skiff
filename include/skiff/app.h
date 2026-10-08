#ifndef SKIFF_APP_H
#define SKIFF_APP_H

/*
 * The app: its screens and the state machine that walks the player from a first launch to a
 * downloaded game (set the server, pair, browse, download, watch the downloads, settings). It is
 * portable: the platform reads the buttons, draws the view this module describes, runs the system
 * dialogs and the network, and reaches it through skiff_app_env. Host tests drive it with fakes.
 *
 * Threads: everything here runs on the UI thread, once per frame (skiff_app_update()), except the
 * hook the download queue calls on the worker when a download finished (it records the file in
 * installed.json under the manifest lock). The UI thread never waits for the Memory Stick in its
 * per-frame path: it reads the queue through skiff_jobs_next_event() and skiff_jobs_list() only,
 * and writes files (config.ini, the queue) only when the player asked for something.
 *
 * Requests to RomM run on the UI thread, one per frame at most, and only on a frame after the one
 * that showed what is being waited for ("Contacting RomM..."), so the screen never freezes without
 * saying why. Joining the Wi-Fi takes seconds and is polled instead (net_start, net_poll).
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/config.h"
#include "skiff/error.h"
#include "skiff/i18n.h"
#include "skiff/jobs.h"
#include "skiff/log.h"
#include "skiff/romm.h"
#include "skiff/storage.h"
#include "skiff/storage_paths.h"
#include "skiff/transport.h"
#include "skiff/ui.h"

#define SKIFF_APP_LOG_TAG "app"
#define SKIFF_APP_LOG_FILE_NAME "skiff.log"
/* The platform the app browses and the installer it downloads with. */
#define SKIFF_APP_PLATFORM_SLUG "psp"

/* What the view holds; the PSP renderer draws it in the area between header and footer. */
#define SKIFF_APP_LINES_MAX 8
#define SKIFF_APP_ROWS_MAX 12
#define SKIFF_APP_HINTS_MAX 5
#define SKIFF_APP_TITLE_MAX 64
#define SKIFF_APP_HINT_MAX 32
#define SKIFF_APP_DETAIL_MAX 32
#define SKIFF_APP_LABEL_MAX 160
/* Widths in the measure callback's units (pixels on the PSP): a body line, a list row's label,
 * and the column on its right that holds the row's detail ("Installed", "45 %"). */
#define SKIFF_APP_TEXT_WIDTH 456.0f
#define SKIFF_APP_LABEL_WIDTH 340.0f
#define SKIFF_APP_DETAIL_WIDTH 110.0f
/* The header's right part, after the title. */
#define SKIFF_APP_STATUS_WIDTH 230.0f

/* ROM pages kept in memory while browsing: the one on screen and its neighbours. */
#define SKIFF_APP_CACHED_PAGES 3
/* How long a short message ("Added to downloads") stays in the header. */
#define SKIFF_APP_NOTE_MS 3000
/* The view is rebuilt at most this often for progress alone. */
#define SKIFF_APP_PROGRESS_REFRESH_MS 250

typedef enum skiff_app_screen {
    /* Reading config.ini, the queue and installed.json. */
    SKIFF_APP_SCREEN_STARTING,
    /* No server address yet: the player types one. */
    SKIFF_APP_SCREEN_SERVER,
    /* Joining the Wi-Fi, starting TLS, or contacting RomM. */
    SKIFF_APP_SCREEN_CONNECTING,
    SKIFF_APP_SCREEN_PAIR,
    SKIFF_APP_SCREEN_LIBRARY,
    SKIFF_APP_SCREEN_DETAILS,
    SKIFF_APP_SCREEN_QUEUE,
    SKIFF_APP_SCREEN_SETTINGS,
    /* An error or a notice, with OK and sometimes another choice (retry, choose a network). */
    SKIFF_APP_SCREEN_MESSAGE,
    /* A question: OK goes ahead, back does not. */
    SKIFF_APP_SCREEN_CONFIRM,
} skiff_app_screen;

/* The system dialog the platform should run, while the view shows the screen under it. */
typedef enum skiff_app_dialog {
    SKIFF_APP_DIALOG_NONE,
    /* The on-screen keyboard, for the server address: the view's dialog_title and dialog_text. */
    SKIFF_APP_DIALOG_KEYBOARD,
    /* The network picker (netconf), connecting to an access point. */
    SKIFF_APP_DIALOG_NETWORK,
} skiff_app_dialog;

typedef enum skiff_app_dialog_result {
    /* Keyboard: text accepted. Network: connected. */
    SKIFF_APP_DIALOG_ACCEPTED,
    SKIFF_APP_DIALOG_CANCELLED,
    SKIFF_APP_DIALOG_FAILED,
} skiff_app_dialog_result;

typedef struct skiff_app_row {
    /* Fitted to SKIFF_APP_LABEL_WIDTH and SKIFF_APP_DETAIL_WIDTH. */
    char label[SKIFF_APP_LABEL_MAX];
    char detail[SKIFF_APP_DETAIL_MAX];
    /* Shown but not usable (a ROM Skiff cannot download): draw it dimmed. */
    int dim;
} skiff_app_row;

typedef struct skiff_app_hint {
    unsigned action;
    char label[SKIFF_APP_HINT_MAX];
} skiff_app_hint;

/* What to draw this frame. Strings are UTF-8, already fitted or wrapped to their widths. */
typedef struct skiff_app_view {
    skiff_app_screen screen;
    char title[SKIFF_APP_TITLE_MAX];
    /* Right of the title: a count, or a short message. */
    char status[SKIFF_APP_TITLE_MAX];
    /* Body text, each line within SKIFF_APP_TEXT_WIDTH; the emphasised one (-1 for none) is drawn
     * large, as the pairing code is. */
    size_t line_count;
    char lines[SKIFF_APP_LINES_MAX][SKIFF_TEXT_MAX];
    int emphasis_line;
    /* A list under the lines: rows[i] is item list.first + i. */
    int has_list;
    skiff_ui_list list;
    size_t row_count;
    skiff_app_row rows[SKIFF_APP_ROWS_MAX];
    /* A progress bar under the list or the lines. */
    int has_progress;
    unsigned percent;
    size_t hint_count;
    skiff_app_hint hints[SKIFF_APP_HINTS_MAX];
    /* A system dialog to show over the screen (draw the dim backdrop under it). */
    skiff_app_dialog dialog;
    char dialog_title[SKIFF_APP_TITLE_MAX];
    char dialog_text[SKIFF_CONFIG_URL_MAX];
} skiff_app_view;

/* ---- What the app needs from the platform ---- */

/* The file names in config.ini made into paths, and its custom headers, for a transport. */
typedef struct skiff_app_transport_settings {
    /* Empty for none. */
    char ca_file[SKIFF_STORAGE_PATH_MAX];
    char client_cert[SKIFF_STORAGE_PATH_MAX];
    char client_key[SKIFF_STORAGE_PATH_MAX];
    /* Views into the config the app keeps. */
    skiff_http_header headers[SKIFF_CONFIG_HEADERS_MAX];
    size_t header_count;
} skiff_app_transport_settings;

/* What the download worker runs with; everything it points to lives in the app until
 * stop_worker() returned SKIFF_OK. */
typedef struct skiff_app_worker_spec {
    skiff_jobs *jobs;
    const skiff_romm_client *romm;
    const skiff_app_transport_settings *transport;
    /* The Network Settings connection to rejoin. */
    int profile;
    skiff_log *log;
} skiff_app_worker_spec;

typedef struct skiff_app_env {
    void *ctx;
    /* A monotonic clock in milliseconds. */
    int64_t (*now_ms)(void *ctx);
    /* The width text takes in the UI's text style. */
    skiff_ui_measure_fn measure;
    /* The Wi-Fi switch is on. */
    int (*switch_on)(void *ctx);
    /* Starts joining a Network Settings connection (loading the network first if needed), then
     * net_poll() once per frame: SKIFF_OK with *joined 0 while it runs, 1 once joined; or the
     * join's error (SKIFF_ERR_NET_UNAVAILABLE, SKIFF_ERR_NET_WIFI_JOIN). */
    skiff_err (*net_start)(void *ctx, int profile);
    skiff_err (*net_poll)(void *ctx, int *joined);
    /* After the network picker connected: the connection it joined, for config.ini. */
    skiff_err (*net_profile)(void *ctx, int *profile);
    /* The name the player gave a Network Settings connection, shown while joining it. */
    skiff_err (*net_profile_name)(void *ctx, int profile, char *out, size_t size);
    /* Starts TLS once: SKIFF_OK, or why it cannot (SKIFF_ERR_NET_NEEDS_ARK,
     * SKIFF_ERR_NET_ENTROPY). */
    skiff_err (*tls_start)(void *ctx);
    /* A transport for browsing (destroyed by the app). */
    skiff_err (*open_transport)(void *ctx, const skiff_app_transport_settings *settings,
                                skiff_transport **out);
    /* Random bytes from the source TLS uses (for this PSP's device identifier). */
    skiff_err (*random)(void *ctx, unsigned char *out, size_t size);
    /* The download worker: started once Skiff can reach RomM; a stop that returns anything but
     * SKIFF_OK leaves the worker running, and nothing it uses may be changed or freed. */
    skiff_err (*start_worker)(void *ctx, const skiff_app_worker_spec *spec);
    skiff_err (*stop_worker)(void *ctx);
} skiff_app_env;

typedef void (*skiff_app_lock_fn)(void *ctx);

typedef struct skiff_app_config {
    /* Not owned; must outlive the app. */
    skiff_storage *storage;
    skiff_storage_roots roots;
    /* From the PSP's system language (skiff_language_from_psp()). */
    skiff_language language;
    /* For skiff.log's timestamps; NULL for none. */
    skiff_log_clock_fn clock;
    void *clock_ctx;
    /* Mutexes shared with the worker, passed to lock and unlock: the log's, the queue's state and
     * commit locks, and installed.json's. All four or none (one thread, as in tests without a
     * worker); each must be its own. */
    skiff_app_lock_fn lock;
    skiff_app_lock_fn unlock;
    void *log_lock;
    void *jobs_lock;
    void *jobs_save_lock;
    void *manifest_lock;
} skiff_app_config;

typedef struct skiff_app skiff_app;

/*
 * Creates the app on the STARTING screen; the first updates read config.ini, start the log, the
 * queue and installed.json, and go on from there. Returns SKIFF_ERR_INVALID_ARG for a NULL
 * argument, a missing hook or storage, or some but not all of the locks; SKIFF_ERR_NO_MEMORY.
 * *out is NULL on error.
 */
skiff_err skiff_app_create(const skiff_app_config *config, const skiff_app_env *env,
                           skiff_app **out);

/*
 * Frees the app: the queue, installed.json's records, the log (written out first) and the browse
 * transport. Call it only after the worker stopped (stop_worker() returned SKIFF_OK); otherwise
 * leave everything to the process exit. Does nothing for NULL.
 */
void skiff_app_destroy(skiff_app *app);

/* One frame: the actions from skiff_ui_input_update(), the queue's events, and at most one request
 * to RomM. */
void skiff_app_update(skiff_app *app, unsigned actions);

/* What to draw now. Valid until the next update. */
const skiff_app_view *skiff_app_view_now(const skiff_app *app);

/* The system dialog the view asked for ended: the keyboard's text (UTF-8) when accepted, or how
 * the network picker ended. */
void skiff_app_dialog_done(skiff_app *app, skiff_app_dialog_result result, const char *text);

/* The player asked to quit (START). The platform then stops the worker and calls
 * skiff_app_destroy(), as it does for HOME -> Quit. */
int skiff_app_quit_requested(const skiff_app *app);

/* The logger, once started (NULL before), for the platform's own lines. */
skiff_log *skiff_app_log(const skiff_app *app);

/* ---- Exposed for the tests and the platform ---- */

/*
 * Makes config's ca_file, cert_file and key_file into paths in app: and points headers at its
 * custom headers. SKIFF_ERR_INVALID_ARG for a NULL argument, skiff_storage_resolve()'s error for a
 * name that does not resolve.
 */
skiff_err skiff_app_transport_settings_from(const skiff_config *config,
                                            const skiff_storage_roots *roots,
                                            skiff_app_transport_settings *out);

/*
 * Breaks text into lines of at most width (measured), between words where it can and between
 * UTF-8 characters where a word alone is too wide. Writes up to max_lines lines of line_size bytes
 * into lines and returns how many; text that does not fit max_lines ends its last line cut with
 * SKIFF_UI_ELLIPSIS.
 */
size_t skiff_app_wrap(const char *text, float width, skiff_ui_measure_fn measure, void *measure_ctx,
                      char (*lines)[SKIFF_TEXT_MAX], size_t max_lines);

#endif
