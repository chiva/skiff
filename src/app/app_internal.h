#ifndef SKIFF_APP_INTERNAL_H
#define SKIFF_APP_INTERNAL_H

/* What the app's files share: the app object behind skiff/app.h's opaque type. */

#include "skiff/app.h"
#include "skiff/install.h"
#include "skiff/romm_installs.h"
#include "skiff/romm_pairing.h"

/* A text for the header: plain, or the text a game's name fills ({1}), whose name is shortened when
 * the header is too narrow so the rest stays whole ("Synced: Pinball Hall of Fa..."). text is the
 * plain text or the name; "" for none. */
typedef struct app_header_text {
    char text[SKIFF_TEXT_MAX];
    /* The text the name fills; SKIFF_TEXT_COUNT for a plain one. */
    skiff_text_id named;
} app_header_text;

#include "app_saves.h"

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
/* Settings' rows: the server, Pair again, Saves, Games from RomM. */
#define APP_SETTINGS_ROW_COUNT 4
/* Lines a game's title and its file name each take at most on the details screen, beside the
 * cover: the size, the download state and the free space always fit under them. */
#define APP_DETAILS_NAME_LINES 3
/* An open collection's name as the library's title, in the measure's units: drawn
 * SKIFF_APP_TITLE_SCALE the size of body text, it leaves the status on its right room for the
 * game count and the save notes. */
#define APP_TITLE_NAME_WIDTH 180.0f

/* What the library lists; SELECT goes to the next one (every game, the favourites, the
 * collections, every game again). Not saved: every launch starts on every game. */
typedef enum app_source {
    SOURCE_ALL,
    SOURCE_FAVOURITES,
    /* The RomM user's collections, then the games of the one opened from them. */
    SOURCE_COLLECTIONS,
} app_source;

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
    /* Go on from the starting screen (after the notice of a save restore finished at launch). */
    MESSAGE_START_CONTINUE,
    /* No second choice. */
    MESSAGE_NONE,
} app_message_action;

typedef enum app_confirm_action {
    CONFIRM_REPLACE,
    CONFIRM_CANCEL_JOB,
    CONFIRM_SERVER_CHANGE,
    /* "Download all": the confirmation lists what the batch found (app->batch). */
    CONFIRM_DOWNLOAD_ALL,
    /* A save backup over the game's saves on this PSP. */
    CONFIRM_RESTORE_BACKUP,
} app_confirm_action;

/* A request to RomM, made on the frame after the one that said it was coming. */
typedef enum app_request {
    REQUEST_NONE,
    REQUEST_PAGE,
    REQUEST_ROM,
    /* The ROM's cover, once its details are in. */
    REQUEST_COVER,
    /* The collections list. */
    REQUEST_COLLECTIONS,
    /* A save sync the player asked for, which found the Wi-Fi gone: it goes on once joined. */
    REQUEST_SAVES,
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
    /* "Download all" (app_batch.c): a page of the list's games with their files, the Memory
     * Stick's free space, then planning and queueing the chosen games with one save. */
    CALL_BATCH_PAGE,
    CALL_BATCH_SPACE,
    CALL_BATCH_QUEUE,
    /* The RomM user's collections, regular and smart. */
    CALL_COLLECTIONS,
    /* Save sync (app_saves.h): a check of every game or of one, one game's transfer, the session
     * closed, a game's backups. */
    CALL_SAVES_CHECK,
    CALL_SAVES_APPLY,
    CALL_SAVES_COMPLETE,
    CALL_SAVES_BACKUPS,
    /* Games sent from RomM (app_sends.c): remote_install registered when needed, the claim, each
     * claimed game's details and the free space; then the chosen games queued with one save (only
     * the Memory Stick); then every send reported, after that save. */
    CALL_SENDS_CLAIM,
    CALL_SENDS_QUEUE,
    CALL_SENDS_REPORT,
    /* The Memory Stick's free space at startup: its first count reads the whole FAT (3 s on a
     * PSP-1000, S1), so it runs off the screen while the Wi-Fi joins. */
    CALL_FREE_SPACE,
} app_call_kind;

/* The most games one "Download all" queues: the queue holds no more unfinished ones. */
#define APP_BATCH_MAX SKIFF_JOBS_MAX

typedef enum app_batch_step {
    BATCH_NONE,
    /* Favourites pages, one call each, classified as they arrive. */
    BATCH_SCANNING,
    /* The free space, then the confirmation. */
    BATCH_SPACE,
    BATCH_CONFIRMING,
    /* Planning and queueing, on the browsing thread. */
    BATCH_QUEUEING,
} app_batch_step;

/* What app_batch_take() made of a game: chosen, or left out and why. */
typedef enum app_batch_outcome {
    OUTCOME_TAKEN,
    /* Already chosen in this batch. */
    OUTCOME_REPEATED,
    /* Skiff cannot install it (no single usable file, empty, too large, not a supported kind). */
    OUTCOME_REFUSED,
    /* A job for it is in the queue (queued, active or failed). */
    OUTCOME_QUEUED,
    OUTCOME_INSTALLED,
    /* Installed, but changed in RomM since. */
    OUTCOME_CHANGED,
    /* No room left in the queue, or in installed.json. */
    OUTCOME_OVER_QUEUE,
    OUTCOME_OVER_RECORDS,
} app_batch_outcome;

/* Games chosen to queue, and why the others were left out: "Download all" on the favourites or an
 * open collection uses app->batch, in name order; another list may own one of its own. While it
 * queues (app_batch_queue_run()), the browsing thread reads it and fills plans, requests and ids;
 * the screen's thread leaves it alone until the call is done. */
typedef struct app_batch {
    /* The list it reads, and the collection's name for the question (empty for the favourites). */
    skiff_romm_list_filter filter;
    uint64_t collection_id;
    char name[SKIFF_APP_LABEL_MAX];
    uint64_t next_page;
    uint64_t total;
    uint64_t seen;
    /* What will not be queued, and why. */
    size_t installed;
    size_t changed;
    size_t queued;
    size_t refused;
    size_t over_queue;
    size_t over_records;
    size_t over_space;
    /* Room when the batch started: unfinished jobs, and installed.json records. */
    size_t queue_room;
    size_t record_room;
    /* Chosen while scanning; count is cut down to what fits the free space. */
    size_t count;
    uint64_t bytes;
    /* The Memory Stick's free space, and what unfinished downloads still need of it. */
    uint64_t free_bytes;
    uint64_t pending_bytes;
    int free_known;
    app_batch_step step;
    skiff_romm_rom_summary games[APP_BATCH_MAX];
    /* Out of the queue call: each game's plan (planned[i] when it has one, else why not in
     * plan_errors[i]), the requests made of them, the queue as it was, and what was queued. */
    int planned[APP_BATCH_MAX];
    skiff_err plan_errors[APP_BATCH_MAX];
    skiff_install_plan plans[APP_BATCH_MAX];
    skiff_job_request requests[APP_BATCH_MAX];
    uint32_t ids[APP_BATCH_MAX];
    skiff_job listed[SKIFF_JOBS_MAX];
    size_t listed_count;
    size_t request_count;
    size_t added;
    /* Games whose target could not be planned, and the first one's error. */
    size_t plan_failures;
    skiff_err plan_error;
} app_batch;

/* While claims wait for the Wi-Fi, the platform is asked this often whether something else joined
 * it again (skiff_app_env.net_online). */
#define APP_SENDS_JOIN_PROBE_MS 1000
/* A round of games sent from RomM, from its claim to its reports (app_sends.c), on the heap. */
typedef struct app_sends_round app_sends_round;

/* Why Settings' "Games from RomM" cannot be switched, in the order the row says it. */
typedef enum app_sends_block {
    SENDS_OK,
    /* Nothing is known about the server yet: the row shows the setting. */
    SENDS_NOT_CONNECTED,
    /* RomM before 5.4 (no sends at all). */
    SENDS_NEEDS_UPDATE,
    /* RomM 5.4 with DEVICE_INSTALL turned off (its heartbeat's ENABLED is not true). */
    SENDS_TURNED_OFF,
    /* The token lacks devices.write, there is no device id, or RomM refused it (218). */
    SENDS_NEEDS_PAIRING,
} app_sends_block;

/* Games sent from RomM (app_sends.c): what this run told RomM and when it next asks. */
typedef struct app_sends {
    /* When the next claim may run (app_now()); 0 right after connecting. */
    int64_t next_claim_ms;
    /* The round under way, from its claim to its reports; NULL between rounds. */
    app_sends_round *round;
    /* The last failed claim's error, logged once until another result. */
    skiff_err last_error;
    /* When the platform may next be asked whether the Wi-Fi is joined again (app_now()). */
    int64_t next_probe_ms;
    /* remote_install true was sent on this connection. */
    int registered;
    /* Switched off: remote_install false was sent, or tried, on this connection (once per
     * connection, so RomM stops listing this PSP even if an earlier run could not tell it; not
     * again after a failure until the next connection). */
    int unregistered;
    /* RomM refused the token (218): Settings offers Pair again until a new pairing or server. */
    int forbidden;
    /* RomM answered 404 (the device deleted, or sends turned off since the heartbeat): no claim
     * until the next connection. */
    int gone;
} app_sends;

typedef struct app_call {
    skiff_romm_client *romm;
    /* In: the platform, list and page to list (with each ROM's files), the ROM to fetch. */
    uint64_t platform_id;
    skiff_romm_list_filter filter;
    uint64_t collection_id;
    int with_files;
    uint64_t page_index;
    uint64_t rom_id;
    /* Out: what the request returned. */
    skiff_romm_server server;
    skiff_romm_platform platform;
    skiff_romm_rom_page page;
    skiff_romm_rom rom;
    skiff_romm_collection_list collections;
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
    /* How long CALL_FREE_SPACE took, for the log. */
    int64_t free_space_ms;
    /* "Download all": out, the free space (CALL_BATCH_SPACE); in, the app whose batch
     * CALL_BATCH_QUEUE plans and queues (its installer, manifest under its lock, roots and queue:
     * none of them changes while a call runs). */
    uint64_t free_bytes;
    skiff_app *app;
    /* Save sync's inputs and results. */
    app_saves_call saves;
    skiff_err err;
    app_call_kind kind;
    /* It could not start: err says why, and the next update applies it as the request's error. */
    int failed_to_start;
    /* The player left what it was for: its result is dropped. */
    int abandoned;
    /* The browse client is replaced once it is done (a lost connection, a new server). */
    int drop_client;
    /* env.suspends() when it started, and whether it failed across a suspend: err is then
     * SKIFF_ERR_NET_CONNECTION_LOST whatever the request saw (app_call_update()). */
    uint32_t suspends_at_start;
    int suspended;
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
    app_batch batch;
    app_sends sends;
    /* Save sync (app_saves.c), on the heap. */
    app_saves *saves;

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
    /* The collections (SOURCE_COLLECTIONS): RomM's list as last loaded, its rows, and the
     * collection whose games the library lists while one is open. */
    skiff_romm_collection_list collections;
    char collection_labels[SKIFF_ROMM_COLLECTIONS_MAX][SKIFF_APP_LABEL_MAX];
    skiff_ui_list collections_list;
    skiff_romm_collection collection;
    /* Why the list could not load (shown with Retry); SKIFF_OK when it did or loads. */
    skiff_err collections_error;
    /* The ROM on the details screen. */
    skiff_romm_rom rom;
    /* Its cover (when cover_key is its key, see app_cover_shown()), and the buffer the next one is
     * decoded into. */
    skiff_cover *cover;
    skiff_cover *cover_spare;
    skiff_cover_key cover_key;
    /* Free space on the Memory Stick, read when a screen showing it opens. */
    uint64_t free_bytes;
    /* The first count runs on the browsing thread (CALL_FREE_SPACE): the screens show the free
     * space once it is known, and ask again only then. */
    int free_pending;

    app_queue_view queue;
    skiff_ui_list queue_list;
    skiff_ui_list settings_list;

    /* Flags and small values. */
    int dirty;
    int quit;
    int has_server;
    int net_joined;
    /* env.suspends() when last looked at (app_notice_suspend()), and whether the browse client
     * goes once the call running across that suspend has been taken. */
    uint32_t suspends_seen;
    int drop_after_call;
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
    app_source source;
    /* The collections list is in, and a collection of it is open. */
    int collections_known;
    int collection_open;
    /* RomM refused the token the collections (403): Pair again, as without collections.read. */
    int collections_forbidden;
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
    /* A short message in the header, until note_until_ms. */
    app_header_text note;
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
/* Adds text, wrapped, under the message on screen. */
void app_message_add(skiff_app *app, const char *text);
/* Sets key in section of config.ini, saves the file and parses it again. */
skiff_err app_config_set(skiff_app *app, const char *section, const char *key, const char *value,
                         skiff_config_issue *issue);
skiff_err app_config_replace(skiff_app *app, const char *text, size_t length);
void app_note(skiff_app *app, const char *text);
/* A note the name fills: id with {1}, the name shortened to fit the header (app_header_text). */
void app_note_named(skiff_app *app, skiff_text_id id, const char *name);
int64_t app_now(const skiff_app *app);
void app_lock_manifest(skiff_app *app);
void app_unlock_manifest(skiff_app *app);
/* Stops the worker for a change of server or token; 0 when it would not stop. */
int app_stop_worker(skiff_app *app);
skiff_err app_start_worker(skiff_app *app);
/* Drops what was loaded from the old server, and the queue, so they are made again. */
skiff_err app_reset_server(skiff_app *app);
/* Goes on from the starting screen: the server's address, or connecting. */
void app_start_continue(skiff_app *app);

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
/* Errors a suspend causes in a call that spans it: the connection gone, and the Memory Stick's
 * files lost to it or not back yet (a CA file that cannot be read fails as a setting). */
int app_is_suspend_error(skiff_err err);
/* Once a frame: a suspend since the last look leaves the Wi-Fi to join again before the next
 * request, as a lost connection does. */
void app_notice_suspend(skiff_app *app);
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
/* Why Skiff cannot install a file skiff_install_check() refused, as the details screen says it. */
skiff_text_id app_install_support_text(skiff_install_support support);
/* Whether a list item is one Skiff can download (dimmed otherwise). */
int app_rom_downloadable(const skiff_app *app, const skiff_romm_rom_summary *rom);
/* installed.json can still record rom_id's file_name, counting the records queued downloads will
 * need (takes the manifest lock). */
int app_installed_room(skiff_app *app, uint64_t rom_id, const char *file_name);
/* Records installed.json can still take, the ones queued downloads will need counted (takes the
 * manifest lock). */
size_t app_installed_records_free(skiff_app *app);
/* The name a ROM shows: its title, or its file name without one. */
const char *app_rom_name(const skiff_romm_rom_summary *rom);
/* The games the library lists: every game, the favourites, or the open collection's (its id). */
void app_current_list(const skiff_app *app, skiff_romm_list_filter *filter,
                      uint64_t *collection_id);
/* The library lists games (not the collections themselves). */
int app_games_listed(const skiff_app *app);
/* "every game", "favourites", "collection 5", "smart collection 2": a list as the log names it. */
#define APP_LIST_NAME_MAX 48
void app_list_name(skiff_romm_list_filter filter, uint64_t collection_id, char *out, size_t size);
/* START offers "Download all": the favourites or an open collection, with games. */
int app_batch_offered(const skiff_app *app);

/* ---- app_collections.c ---- */

/* The collections list is entered: it loads again (or says to pair again). */
void app_collections_enter(skiff_app *app);
/* Forgets the list, the open collection and RomM's refusal (a new server or token). */
void app_collections_forget(skiff_app *app);
/* The token cannot list collections: no collections.read in [auth] scopes, or RomM said 403. */
int app_collections_denied(const skiff_app *app);
/* Asks for the list when the screen needs it and it is not on its way. */
void app_collections_request(skiff_app *app);
/* Starts the list's call, when it is still wanted (REQUEST_COLLECTIONS). */
void app_collections_run(skiff_app *app);
void app_collections_done(skiff_app *app);
/* The collections list's own buttons (the library handles the shared ones first): moving, Retry,
 * Pair again. 1 when the player opened the selected collection. */
int app_collections_update(skiff_app *app, unsigned actions);
/* The name a collection shows: its own, or "Untitled collection". */
const char *app_collection_name(const skiff_app *app, const skiff_romm_collection *collection);

/* ---- app_batch.c ---- */

/* START on the favourites or an open collection: checks its games, then asks to download what
 * fits. */
void app_batch_start(skiff_app *app);
/* Once a frame, after app_call_update(): starts the batch's next call when none runs. */
void app_batch_update(skiff_app *app);
/* Back while the list's games are checked: the batch ends, its call dropped. */
void app_batch_cancel(skiff_app *app);
void app_batch_confirmed(skiff_app *app);
/* The confirmation's lines (CONFIRM_DOWNLOAD_ALL), for the view. */
size_t app_batch_lines(const skiff_app *app, char lines[][SKIFF_TEXT_MAX], size_t capacity);
/* Any batch's steps, its owner's own or app->batch: emptied with the room the queue and
 * installed.json have now; a game sorted (counted as left out, or chosen while there is room); the
 * chosen games cut down to the longest run that fits the free space (free_err not SKIFF_OK:
 * unknown, not held against them). */
void app_batch_reset(skiff_app *app, app_batch *batch);
app_batch_outcome app_batch_take(skiff_app *app, app_batch *batch,
                                 const skiff_romm_rom_summary *rom);
void app_batch_fit(skiff_app *app, app_batch *batch, skiff_err free_err, uint64_t free_bytes);
/* On the browsing thread: plans and queues batch's chosen games with one save of the queue
 * (CALL_BATCH_QUEUE: app->batch). */
skiff_err app_batch_queue_run(skiff_app *app, app_batch *batch);
void app_batch_page_done(skiff_app *app);
void app_batch_space_done(skiff_app *app);
/* Starts the first free-space count off the screen (CALL_FREE_SPACE), and takes its result. */
void app_free_space_start(skiff_app *app);
void app_free_space_done(skiff_app *app);
void app_batch_queued(skiff_app *app);

/* ---- app_queue.c ---- */

void app_queue_refresh(skiff_app *app);
void app_queue_event(skiff_app *app, const skiff_jobs_event *event);
void app_queue_update(skiff_app *app, unsigned actions);
void app_cancel_confirmed(skiff_app *app, uint32_t id);
/* Jobs queued, active or failed: what counts against SKIFF_JOBS_MAX. */
size_t app_queue_unfinished(const skiff_app *app);
/* The unfinished job for rom_id, or NULL. */
const skiff_job *app_queue_job_for(const skiff_app *app, uint64_t rom_id);
/* The bytes unfinished jobs will write: their whole sizes (a partial file's progress is not known
 * here, so this errs on the side of room). */
uint64_t app_queue_unfinished_bytes(const skiff_app *app);
/* Whether two real paths name the same file on FAT (ASCII letters ignoring case). */
int app_same_path(const char *a, const char *b);
/* Whether a job is queued, active or failed. */
int app_job_unfinished(const skiff_job *job);

/* ---- app_settings.c ---- */

void app_settings_update(skiff_app *app, unsigned actions);
void app_server_entered(skiff_app *app, const char *url);
void app_server_change_confirmed(skiff_app *app);

/* ---- app_sends.c ---- */

/* Once a frame, after app_batch_update() and before app_saves_update(): registers and claims when a
 * claim is due (right after connecting, then every SKIFF_APP_INSTALL_CLAIM_INTERVAL_MS) and nothing
 * else runs or waits, and tells RomM remote_install false after the player switched it off. */
void app_sends_update(skiff_app *app);
/* The heartbeat came back: a new connection registers again and claims at once. */
void app_sends_connected(skiff_app *app);
/* A new server or token: what RomM refused and a false still to send go with the old one. */
void app_sends_reset(skiff_app *app);
/* Frees the round of a call that was abandoned, or of an app that quits. */
void app_sends_dropped(skiff_app *app);
/* A round is under way or a claim is due: save sync waits for it (sends go first). */
int app_sends_busy(const skiff_app *app);
/* The queue call runs: a single download, "Download all", a cancel, a server change and a new
 * pairing wait for it. */
int app_sends_queueing(const skiff_app *app);
/* A round is under way and has not queued its games yet (or queues them): the room in the queue
 * and installed.json counted now may still be theirs. */
int app_sends_taking_room(const skiff_app *app);
app_sends_block app_sends_blocked(const skiff_app *app);
/* Settings' row: its label (SKIFF_APP_LABEL_MAX), whether it is dimmed, and its button. */
void app_sends_row(const skiff_app *app, char *label, size_t size, int *dim);
void app_sends_choose(skiff_app *app);
/* On the browsing thread (call->app's round). */
skiff_err app_sends_claim_run(app_call *call);
skiff_err app_sends_queue_run(app_call *call);
skiff_err app_sends_report_run(app_call *call);
/* Back on the screen's thread. */
void app_sends_claimed(skiff_app *app);
void app_sends_queued(skiff_app *app);
void app_sends_reported(skiff_app *app);

/* ---- app_saves.c ---- */

skiff_err app_saves_create(app_saves **out);
void app_saves_destroy(app_saves *saves);
/* At startup: loads saves.json and finishes a restore cut short (skiff_saves_recover()'s result:
 * SKIFF_OK, or the notice 308 or 309 to show). */
skiff_err app_saves_start(skiff_app *app);
/* The token holds every scope save sync needs. */
int app_saves_has_scopes(const skiff_config *settings);
app_saves_block app_saves_blocked(const skiff_app *app);
/* A save call runs or waits to. */
int app_saves_busy(const skiff_app *app);
/* Forgets every game's baseline (pairing again), and the ROMs' save IDs too after a new server
 * (server_changed), on the Memory Stick first: before the new token or address is saved, so a
 * restart can never pair the old baselines with the new identity. Backups and the saves themselves
 * stay. On an error nothing in memory changes and the caller keeps the old identity. */
skiff_err app_saves_forget(skiff_app *app, int server_changed);
/* Connected again for a save sync the player asked for: back to where they asked. */
void app_saves_resume(skiff_app *app);
/* Once a frame, after app_call_update(): starts the next save call when nothing else waits. */
void app_saves_update(skiff_app *app);
/* What a finished save call returned, applied on the screen's thread. */
void app_saves_checked(skiff_app *app);
void app_saves_applied(skiff_app *app);
void app_saves_completed(skiff_app *app);
void app_saves_backups_listed(skiff_app *app);
/* The screens' buttons. */
void app_saves_open(skiff_app *app);
void app_saves_screen_update(skiff_app *app, unsigned actions);
void app_saves_backups_update(skiff_app *app, unsigned actions);
void app_saves_choice_update(skiff_app *app, unsigned actions);
void app_saves_restore_confirmed(skiff_app *app);
/* The player asks for the restore journal that stopped save sync to be settled once more. */
void app_saves_settle_again(skiff_app *app);
/* The name a game shows: RomM's title, or its save ID without one. */
const char *app_saves_title(const app_save_game *game);
const app_save_game *app_saves_game_of_rom(const skiff_app *app, uint64_t rom_id);
/* The player must choose for this game, or a transfer of it failed. */
int app_saves_needs_player(const app_save_game *game);
/* The games on the Saves screen that need the player (app_saves_needs_player()); 0 while save sync
 * cannot run. */
size_t app_saves_waiting(const skiff_app *app);
void app_saves_problem_text(const skiff_app *app, const app_save_game *game, char *out);
/* The details screen's ROM as a game to sync (1), or 0 when RomM has no save ID Skiff syncs. */
int app_saves_rom_game(const skiff_app *app, app_save_game *out);
/* The details screen's ROM came in: its save ID is remembered for the next check. */
void app_saves_rom_loaded(skiff_app *app);
/* The details screen offers "Sync save", and the button. */
int app_saves_details_offered(const skiff_app *app);
void app_saves_details_sync(skiff_app *app);

/* ---- app_saves_run.c (on the browsing thread) ---- */

skiff_err app_saves_check_run(app_call *call);
skiff_err app_saves_apply_run(app_call *call);
skiff_err app_saves_complete_run(app_call *call);
skiff_err app_saves_backups_run(app_call *call);

/* ---- app_saves_view.c ---- */

void app_saves_view(skiff_app *app);
void app_saves_backups_view(skiff_app *app);
void app_saves_choice_view(skiff_app *app);
/* The details screen's "Save: ..." line into out (SKIFF_TEXT_MAX); 0 when it has none. */
int app_saves_details_line(const skiff_app *app, char *out);
/* A game's state, short (a row's detail) or whole, into out (SKIFF_TEXT_MAX); "" for none. */
void app_saves_state_text(const skiff_app *app, const app_save_game *game, int whole, char *out);

/* ---- app_view.c ---- */

void app_view_build(skiff_app *app);
/* Pieces of a view, shared with app_saves_view.c. */
void app_view_add_hint(skiff_app *app, unsigned action, skiff_text_id label);
void app_view_add_wrapped(skiff_app *app, const char *text, float width);
void app_view_set_status(skiff_app *app, const char *text);
void app_view_set_title(skiff_app *app, const char *title);
/* The width the status may take beside the view's title (skiff/app.h's header), in the measure's
 * units: set the title first. */
float app_view_status_width(const skiff_app *app);
/* A table under the view's lines, cells from dim_from on dimmed; NULL when the view holds no more.
 * Add its rows, then lay it out. */
skiff_app_table *app_view_add_table(skiff_app *app, size_t dim_from);
/* A row of count cells (NULL for an empty one) led by action's button (0 for none); nothing when
 * the table is full. */
void app_view_add_table_row(const skiff_app *app, skiff_app_table *table, unsigned action,
                            const char *const *cells, size_t count);
/* Places the table's columns: each after the widest cell of the one before and
 * SKIFF_APP_COLUMN_GAP, past the button symbols when a row has one, every cell fitted to what is
 * left of the line. */
void app_view_layout_table(const skiff_app *app, skiff_app_table *table);
const char *app_text(const skiff_app *app, skiff_text_id id);
/* Fills template with args into out (SKIFF_TEXT_MAX). */
void app_format(const skiff_app *app, skiff_text_id id, const char *const *args, size_t count,
                char *out);
void app_format_bytes(const skiff_app *app, uint64_t bytes, char *out, size_t out_size);
void app_fit(const skiff_app *app, const char *text, float width, char *out, size_t out_size);
/* id with {1} filled by name, within width: the name shortened with SKIFF_UI_ELLIPSIS so the rest
 * of the text stays whole, or the whole cut when even that leaves no room. */
void app_format_fit_name(const skiff_app *app, skiff_text_id id, const char *name, float width,
                         char *out, size_t out_size);
/* The detail a ROM shows in the library: its download or its install state; "" for none. */
void app_rom_detail(skiff_app *app, const skiff_romm_rom_summary *rom, char *out, size_t size);

#endif
