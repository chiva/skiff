#ifndef SKIFF_PSP_APP_PSP_H
#define SKIFF_PSP_APP_PSP_H

/*
 * The app on the PSP: what skiff/app.h asks of the platform (skiff_app_env) and drawing its view.
 * One object holds the network stack, the five mutexes the threads share, the download worker,
 * the system dialog and the renderer.
 *
 * Threads: the UI thread runs the app and every hook here; the browsing thread (call_psp.h) runs
 * the app's requests to RomM, and the worker thread (jobs_psp.h) the download queue. The network
 * stack is used by one thread at a time: the worker takes net_lock to rejoin or reload, a request
 * holds it for its transfer, and the UI holds it from the start of a join to its end (several
 * frames) and while the network picker is open. Nobody on the UI side waits for it: while the
 * worker rejoins (up to 30 s) the UI keeps drawing and tries again on the next frame, and a request
 * fails as a lost connection.
 *
 * Teardown follows the worker: when it would not stop, nothing it may still use is freed or
 * unloaded (the queue, the log, the RomM client, TLS, the network, the mutexes); the process exit
 * releases them.
 */

#include <intraFont.h>

#include "skiff/app.h"
#include "skiff/error.h"
#include "skiff/storage.h"
#include "skiff/storage_paths.h"
#include "skiff/ui.h"

#include "call_psp.h"
#include "dialog_psp.h"
#include "jobs_psp.h"
#include "net_psp.h"
#include "ui_psp.h"

/* How long a join may take, as for the worker. */
#define SKIFF_PSP_APP_JOIN_TIMEOUT_US SKIFF_PSP_WORKER_JOIN_TIMEOUT_US
/* Dropping a half-gone connection before joining blocks the UI, so it gets a second; a join that
 * then fails is shown with Retry. Quitting waits as long as the worker would. */
#define SKIFF_PSP_APP_DROP_TIMEOUT_US (1000LL * 1000)
#define SKIFF_PSP_APP_DISCONNECT_TIMEOUT_US SKIFF_PSP_WORKER_DISCONNECT_TIMEOUT_US
/* How often skiff.log gets the frame, memory and worker stack figures, at [log] level = debug. */
#define SKIFF_PSP_APP_STATS_PERIOD_US (10LL * 1000 * 1000)
/* A frame this long since the last one is logged with where its time went (debug level). */
#define SKIFF_PSP_APP_SLOW_FRAME_US (100LL * 1000)

/* Measurements for the hardware tier (row A1): logged at debug level, so a player's skiff.log at
 * the default level never has them. Frames are timed from one present to the next. */
typedef struct skiff_psp_app_stats {
    long long period_start_us;
    long long last_frame_us;
    long long gap_total_us;
    long long gap_max_us;
    int frames;
    /* This frame's steps, for a slow frame's line. */
    long long input_us;
    long long update_us;
    long long draw_us;
    long long present_us;
} skiff_psp_app_stats;

typedef struct skiff_psp_app {
    skiff_storage *storage;
    skiff_storage_roots roots;
    skiff_app *app;

    /* Drawing. */
    intraFont *font;
    skiff_psp_ui ui;
    skiff_psp_ui_style style;
    int ui_started;
    skiff_ui_input input;

    /* The mutexes the threads share (skiff_app_config's four, and the network's). */
    skiff_psp_mutex log_lock;
    skiff_psp_mutex jobs_lock;
    skiff_psp_mutex jobs_save_lock;
    skiff_psp_mutex manifest_lock;
    skiff_psp_mutex net_lock;
    int mutexes_created;

    /* The network: loaded on first need (and again after the worker's reload failed), always
     * under net_lock, and joined through net_start/net_poll. */
    skiff_psp_net net;
    /* The UI holds net_lock: a join is pending, or the network picker is open. */
    int holds_net;
    /* net_start asked to join join_profile: net_poll begins it once it has the network lock. */
    int join_wanted;
    int join_profile;
    /* The connection the network picker joined, read before net_lock is given back. */
    int picked_profile;
    skiff_err picked_error;
    int tls_started;

    skiff_psp_worker worker;
    /* The worker would not stop: leave everything it may use to the process exit. */
    int worker_stuck;
    /* The browsing thread: the app's requests to RomM. */
    skiff_psp_caller caller;

    /* The system dialog the view asked for; static storage, as the system writes into it. */
    skiff_psp_dialog *dialog;
    /* A dialog would not close: end without tearing anything down. */
    int dialog_stuck;

    skiff_psp_app_stats stats;
} skiff_psp_app;

/*
 * Prepares platform (zeroed) for the EBOOT at program_path (argv[0]): storage, roots, mutexes,
 * font and renderer. dialog must be static (or zeroed) storage. SKIFF_OK, or why Skiff cannot run.
 */
skiff_err skiff_psp_app_start(skiff_psp_app *platform, const char *program_path,
                              skiff_psp_dialog *dialog);

/* The app's config and env, pointing at platform. */
skiff_app_config skiff_psp_app_config(skiff_psp_app *platform);
skiff_app_env skiff_psp_app_env(skiff_psp_app *platform);

/*
 * One frame: reads the buttons, runs the app (skiff_app_update()), draws its view, runs the dialog
 * it asks for, and presents the frame. Returns the view drawn. At debug level it also logs, every
 * SKIFF_PSP_APP_STATS_PERIOD_US, the frame times, memory and the worker's stack, and for a frame
 * longer than SKIFF_PSP_APP_SLOW_FRAME_US where its time went.
 */
const skiff_app_view *skiff_psp_app_step(skiff_psp_app *platform);

/* Asks an open dialog to close and runs frames until it has (or is given up on). */
void skiff_psp_app_close_dialog(skiff_psp_app *platform);

/*
 * Ends the app: stops the worker (SKIFF_PSP_WORKER_STOP_TIMEOUT_US), then, only if it stopped,
 * destroys the app, disconnects and unloads the network, stops TLS and frees the rest. Returns 1
 * when everything was released, 0 when something was left to the process exit.
 */
int skiff_psp_app_finish(skiff_psp_app *platform);

#endif
