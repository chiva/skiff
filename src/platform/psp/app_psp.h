#ifndef SKIFF_PSP_APP_PSP_H
#define SKIFF_PSP_APP_PSP_H

/*
 * The app on the PSP: what skiff/app.h asks of the platform (skiff_app_env) and drawing its view.
 * One object holds the network stack, the five mutexes the threads share, the download worker,
 * the system dialog and the renderer.
 *
 * Threads: the UI thread runs the app and every hook here; the worker thread
 * (src/platform/psp/jobs_psp.h) runs the download queue. The network stack is used by one thread
 * at a time: the worker takes net_lock to rejoin or reload, and the UI holds it from the start of a
 * join to its end (several frames) and while the network picker is open.
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

#include "dialog_psp.h"
#include "jobs_psp.h"
#include "net_psp.h"
#include "ui_psp.h"

/* How long the UI gives a join and a disconnect, as the worker does. */
#define SKIFF_PSP_APP_JOIN_TIMEOUT_US SKIFF_PSP_WORKER_JOIN_TIMEOUT_US
#define SKIFF_PSP_APP_DISCONNECT_TIMEOUT_US SKIFF_PSP_WORKER_DISCONNECT_TIMEOUT_US

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

    /* The network: loaded on first need, joined through net_start/net_poll. */
    skiff_psp_net net;
    int net_loaded;
    /* The UI holds net_lock: a join is pending, or the network picker is open. */
    int holds_net;
    /* net_start found the access point already joined: net_poll reports it at once. */
    int already_joined;
    int tls_started;

    skiff_psp_worker worker;
    /* The worker would not stop: leave everything it may use to the process exit. */
    int worker_stuck;

    /* The system dialog the view asked for; static storage, as the system writes into it. */
    skiff_psp_dialog *dialog;
    /* A dialog would not close: end without tearing anything down. */
    int dialog_stuck;
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

/* The actions of this frame from the buttons. */
unsigned skiff_psp_app_read_input(skiff_psp_app *platform);

/* Draws view, runs the dialog it asks for, and presents the frame. */
void skiff_psp_app_frame(skiff_psp_app *platform, const skiff_app_view *view);

/* Asks an open dialog to close and runs frames until it has (or is given up on). */
void skiff_psp_app_close_dialog(skiff_psp_app *platform);

/*
 * Ends the app: stops the worker (SKIFF_PSP_WORKER_STOP_TIMEOUT_US), then, only if it stopped,
 * destroys the app, disconnects and unloads the network, stops TLS and frees the rest. Returns 1
 * when everything was released, 0 when something was left to the process exit.
 */
int skiff_psp_app_finish(skiff_psp_app *platform);

#endif
