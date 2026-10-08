#include "app_psp.h"

#include <psa/crypto.h>
#include <pspctrl.h>
#include <pspkernel.h>
#include <pspwlan.h>
#include <string.h>

#include "skiff/curl_transport.h"

#include "kirk_entropy.h"
#include "lifecycle.h"
#include "storage_psp.h"

#define US_PER_MS 1000LL
/* sceWlanGetSwitchState() with the switch off (pspwlan.h names no value). */
#define WLAN_SWITCH_OFF 0
/* The pairing code, drawn large. */
#define EMPHASIS_SIZE 1.2f

enum {
    LINE_HEIGHT = SKIFF_PSP_UI_ROW_HEIGHT,
    EMPHASIS_HEIGHT = 26,
    LINE_BASELINE = 10,
    EMPHASIS_BASELINE = 22,
    BLOCK_GAP = 4,
    PROGRESS_HEIGHT = 8,
};

/* ---- skiff_app_env ---- */

static int64_t now_ms(void *ctx) {
    (void)ctx;
    return (int64_t)(sceKernelGetSystemTimeWide() / US_PER_MS);
}

static float measure(void *ctx, const char *text) {
    const skiff_psp_app *platform = ctx;
    skiff_psp_ui_style style = platform->style;
    return skiff_psp_ui_measure(&style, text);
}

static int switch_on(void *ctx) {
    (void)ctx;
    return sceWlanGetSwitchState() != WLAN_SWITCH_OFF;
}

static skiff_err load_network(skiff_psp_app *platform) {
    if (platform->net_loaded) {
        return SKIFF_OK;
    }
    const skiff_err err = skiff_psp_net_load(&platform->net, SKIFF_PSP_NET_CPU_MHZ);
    if (err != SKIFF_OK) {
        /* Whatever came up goes down again, so the next try starts clean. */
        (void)skiff_psp_net_unload(&platform->net);
        return err;
    }
    platform->net_loaded = 1;
    return SKIFF_OK;
}

/* The network lock without waiting: the worker may hold it for a whole rejoin (up to 30 s), and the
 * UI keeps drawing meanwhile and tries again on the next frame. */
static int try_take_network(skiff_psp_app *platform) {
    if (!platform->holds_net) {
        platform->holds_net = skiff_psp_mutex_try_lock(&platform->net_lock);
    }
    return platform->holds_net;
}

static void give_network(skiff_psp_app *platform) {
    if (platform->holds_net) {
        platform->holds_net = 0;
        skiff_psp_mutex_unlock(&platform->net_lock);
    }
}

/* The join net_start asked for begins once the UI has the network lock: whether the access point is
 * still joined is read under it too, since the worker may be tearing the connection down. */
static skiff_err begin_join(skiff_psp_app *platform, int *joined) {
    platform->join_wanted = 0;
    if (skiff_psp_net_online(&platform->net) == SKIFF_OK) {
        give_network(platform);
        *joined = 1;
        return SKIFF_OK;
    }
    /* A connection half gone (out of range, after a suspend) is dropped before joining again. */
    (void)skiff_psp_net_disconnect(&platform->net, SKIFF_PSP_APP_DISCONNECT_TIMEOUT_US);
    const skiff_err err = skiff_psp_net_connect_start(&platform->net, platform->join_profile,
                                                      SKIFF_PSP_APP_JOIN_TIMEOUT_US);
    if (err != SKIFF_OK) {
        give_network(platform);
    }
    return err;
}

static skiff_err net_start(void *ctx, int profile) {
    skiff_psp_app *platform = ctx;
    const skiff_err err = load_network(platform);
    if (err != SKIFF_OK) {
        return err;
    }
    platform->join_profile = profile;
    platform->join_wanted = 1;
    return SKIFF_OK;
}

static skiff_err net_poll(void *ctx, int *joined) {
    skiff_psp_app *platform = ctx;
    *joined = 0;
    if (platform->join_wanted) {
        return try_take_network(platform) ? begin_join(platform, joined) : SKIFF_OK;
    }
    skiff_err result = SKIFF_OK;
    if (!skiff_psp_net_connect_poll(&platform->net, &result)) {
        return SKIFF_OK;
    }
    give_network(platform);
    *joined = result == SKIFF_OK;
    return result;
}

static skiff_err net_profile(void *ctx, int *profile) {
    skiff_psp_app *platform = ctx;
    return skiff_psp_net_connected_profile(&platform->net, profile);
}

static skiff_err net_profile_name(void *ctx, int profile, char *out, size_t size) {
    (void)ctx;
    return skiff_psp_net_profile_name(profile, out, size);
}

static skiff_err tls_start(void *ctx) {
    skiff_psp_app *platform = ctx;
    if (platform->tls_started) {
        return SKIFF_OK;
    }
    const skiff_err init = skiff_net_global_init();
    if (init == SKIFF_OK) {
        platform->tls_started = 1;
        return SKIFF_OK;
    }
    /* Without ARK, or with a generator that failed its health test: say which. */
    const skiff_err status = skiff_psp_entropy_status();
    return status != SKIFF_OK ? status : init;
}

static skiff_curl_config curl_config(const skiff_app_transport_settings *settings) {
    const skiff_curl_config curl = {
        .ca_file = settings->ca_file[0] != '\0' ? settings->ca_file : NULL,
        .client_cert = settings->client_cert[0] != '\0' ? settings->client_cert : NULL,
        .client_key = settings->client_key[0] != '\0' ? settings->client_key : NULL,
        .default_headers = settings->headers,
        .default_header_count = settings->header_count,
    };
    return curl;
}

static skiff_err open_transport(void *ctx, const skiff_app_transport_settings *settings,
                                skiff_transport **out) {
    (void)ctx;
    const skiff_curl_config curl = curl_config(settings);
    return skiff_curl_transport_create(&curl, out);
}

static skiff_err random_bytes(void *ctx, unsigned char *out, size_t size) {
    (void)ctx;
    /* TLS's generator, seeded from KIRK (kirk_entropy.c); started by tls_start. */
    return psa_generate_random(out, size) == PSA_SUCCESS ? SKIFF_OK : SKIFF_ERR_NET_ENTROPY;
}

static skiff_err start_worker(void *ctx, const skiff_app_worker_spec *spec) {
    skiff_psp_app *platform = ctx;
    const skiff_psp_worker_config config = {
        .jobs = spec->jobs,
        .romm = spec->romm,
        .net = &platform->net,
        .profile = spec->profile,
        .curl = curl_config(spec->transport),
        .transport_status = platform->tls_started ? SKIFF_OK : SKIFF_ERR_NET_ENTROPY,
        .net_lock = &platform->net_lock,
        .log = spec->log,
    };
    return skiff_psp_worker_start(&platform->worker, &config);
}

static skiff_err stop_worker(void *ctx) {
    skiff_psp_app *platform = ctx;
    const skiff_err err =
        skiff_psp_worker_stop(&platform->worker, SKIFF_PSP_WORKER_STOP_TIMEOUT_US);
    platform->worker_stuck = err != SKIFF_OK;
    return err;
}

skiff_app_env skiff_psp_app_env(skiff_psp_app *platform) {
    const skiff_app_env env = {
        .ctx = platform,
        .now_ms = now_ms,
        .measure = measure,
        .switch_on = switch_on,
        .net_start = net_start,
        .net_poll = net_poll,
        .net_profile = net_profile,
        .net_profile_name = net_profile_name,
        .tls_start = tls_start,
        .open_transport = open_transport,
        .random = random_bytes,
        .start_worker = start_worker,
        .stop_worker = stop_worker,
    };
    return env;
}

skiff_app_config skiff_psp_app_config(skiff_psp_app *platform) {
    const skiff_app_config config = {
        .storage = platform->storage,
        .roots = platform->roots,
        .language = skiff_language_from_psp(skiff_psp_ui_system_language()),
        .clock = skiff_psp_utc_ms,
        .clock_ctx = NULL,
        .lock = skiff_psp_mutex_lock,
        .unlock = skiff_psp_mutex_unlock,
        .log_lock = &platform->log_lock,
        .jobs_lock = &platform->jobs_lock,
        .jobs_save_lock = &platform->jobs_save_lock,
        .manifest_lock = &platform->manifest_lock,
    };
    return config;
}

/* ---- Starting ---- */

static skiff_err create_mutexes(skiff_psp_app *platform) {
    skiff_psp_mutex *const mutexes[] = {&platform->log_lock, &platform->jobs_lock,
                                        &platform->jobs_save_lock, &platform->manifest_lock,
                                        &platform->net_lock};
    static const char *const NAMES[] = {"skiff_log_lock", "skiff_jobs_lock", "skiff_jobs_save_lock",
                                        "skiff_manifest_lock", "skiff_net_lock"};
    for (size_t i = 0; i < sizeof mutexes / sizeof mutexes[0]; i++) {
        const skiff_err err = skiff_psp_mutex_create(mutexes[i], NAMES[i]);
        if (err != SKIFF_OK) {
            for (size_t j = 0; j < i; j++) {
                skiff_psp_mutex_destroy(mutexes[j]);
            }
            return err;
        }
    }
    platform->mutexes_created = 1;
    return SKIFF_OK;
}

skiff_err skiff_psp_app_start(skiff_psp_app *platform, const char *program_path,
                              skiff_psp_dialog *dialog) {
    platform->dialog = dialog;
    skiff_err err = skiff_psp_storage_create(&platform->storage);
    if (err == SKIFF_OK) {
        err = skiff_storage_roots_from_program(program_path, &platform->roots);
    }
    if (err == SKIFF_OK) {
        err = create_mutexes(platform);
    }
    if (err != SKIFF_OK) {
        return err;
    }
    platform->font = skiff_psp_ui_load_font();
    if (platform->font == NULL) {
        /* The firmware font is read whole into the heap: without it there is nothing to draw. */
        return SKIFF_ERR_NO_MEMORY;
    }
    skiff_psp_ui_start(&platform->ui, platform->font);
    platform->ui_started = 1;
    platform->style.ui = &platform->ui;
    platform->style.size = SKIFF_PSP_UI_TEXT_SIZE;
    skiff_ui_input_init(&platform->input, skiff_psp_ui_confirm_is_cross());
    return SKIFF_OK;
}

unsigned skiff_psp_app_read_input(skiff_psp_app *platform) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    if (sceCtrlReadBufferPositive(&pad, 1) < 0) {
        pad.Buttons = 0;
    }
    return skiff_ui_input_update(&platform->input, pad.Buttons);
}

/* ---- Drawing ---- */

static void view_row(void *ctx, size_t index, skiff_psp_ui_row *row) {
    const skiff_app_view *view = ctx;
    const size_t shown = index - view->list.first;
    if (shown >= view->row_count) {
        return;
    }
    row->label = view->rows[shown].label;
    row->detail = view->rows[shown].detail;
    row->dim = view->rows[shown].dim;
}

static void draw_view(const skiff_psp_app *platform, const skiff_app_view *view) {
    const skiff_psp_ui *ui = &platform->ui;
    skiff_psp_ui_header(ui, view->title, view->status[0] != '\0' ? view->status : NULL);
    int y = SKIFF_PSP_UI_CONTENT_TOP + BLOCK_GAP;
    for (size_t i = 0; i < view->line_count; i++) {
        const int emphasis = (int)i == view->emphasis_line;
        skiff_psp_ui_text(ui, SKIFF_PSP_UI_MARGIN,
                          y + (emphasis ? EMPHASIS_BASELINE : LINE_BASELINE),
                          emphasis ? EMPHASIS_SIZE : SKIFF_PSP_UI_TEXT_SIZE,
                          SKIFF_PSP_UI_COLOUR_TEXT, view->lines[i]);
        y += emphasis ? EMPHASIS_HEIGHT : LINE_HEIGHT;
    }
    if (view->has_progress) {
        y += BLOCK_GAP;
        skiff_psp_ui_progress_bar(ui, SKIFF_PSP_UI_MARGIN, y,
                                  SKIFF_PSP_UI_SCREEN_WIDTH - 2 * SKIFF_PSP_UI_MARGIN,
                                  PROGRESS_HEIGHT, view->percent);
        y += PROGRESS_HEIGHT;
    }
    if (view->has_list) {
        if (view->line_count > 0 || view->has_progress) {
            y += BLOCK_GAP;
        }
        skiff_psp_ui_rows(ui, &view->list, y, view_row, (void *)view);
    }
    skiff_psp_ui_hint hints[SKIFF_APP_HINTS_MAX];
    for (size_t i = 0; i < view->hint_count; i++) {
        hints[i].action = view->hints[i].action;
        hints[i].label = view->hints[i].label;
    }
    skiff_psp_ui_footer(ui, hints, view->hint_count);
}

/* ---- System dialogs ---- */

static void dialog_ended(skiff_psp_app *platform, skiff_psp_dialog_state state) {
    skiff_psp_dialog *dialog = platform->dialog;
    /* A stuck picker may still use the network: the lock stays held until the process exits. */
    if (dialog->kind == SKIFF_PSP_DIALOG_NETWORK && state != SKIFF_PSP_DIALOG_STUCK) {
        give_network(platform);
    }
    if (state == SKIFF_PSP_DIALOG_STUCK) {
        skiff_log_write(skiff_app_log(platform->app), SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                        "dialog %d would not close: %s 0x%08X", (int)dialog->kind,
                        dialog->failed_call != NULL ? dialog->failed_call : "-",
                        (unsigned)dialog->sce_result);
        platform->dialog_stuck = 1;
        return;
    }
    if (state == SKIFF_PSP_DIALOG_FAILED) {
        skiff_log_write(skiff_app_log(platform->app), SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                        "dialog %d failed: %s 0x%08X", (int)dialog->kind,
                        dialog->failed_call != NULL ? dialog->failed_call : "-",
                        (unsigned)dialog->sce_result);
    }
    static char text[SKIFF_PSP_DIALOG_TEXT_UTF8_MAX];
    text[0] = '\0';
    skiff_app_dialog_result result = SKIFF_APP_DIALOG_FAILED;
    if (state == SKIFF_PSP_DIALOG_ACCEPTED) {
        result = SKIFF_APP_DIALOG_ACCEPTED;
        if (dialog->kind == SKIFF_PSP_DIALOG_KEYBOARD &&
            skiff_psp_dialog_text(dialog, text, sizeof text) != SKIFF_OK) {
            result = SKIFF_APP_DIALOG_FAILED;
        }
    } else if (state == SKIFF_PSP_DIALOG_CANCELLED) {
        result = SKIFF_APP_DIALOG_CANCELLED;
    }
    skiff_app_dialog_done(platform->app, result, text);
}

/* Opens the dialog the view asks for, if it is not open yet: 1 when one is running. */
static int open_dialog(skiff_psp_app *platform, const skiff_app_view *view) {
    skiff_psp_dialog *dialog = platform->dialog;
    if (dialog->state == SKIFF_PSP_DIALOG_RUNNING) {
        return 1;
    }
    if (view->dialog == SKIFF_APP_DIALOG_NONE) {
        return 0;
    }
    skiff_err err = SKIFF_OK;
    if (view->dialog == SKIFF_APP_DIALOG_KEYBOARD) {
        err = skiff_psp_dialog_start_keyboard(dialog, view->dialog_title, view->dialog_text,
                                              SKIFF_PSP_DIALOG_TEXT_MAX);
    } else {
        err = load_network(platform);
        if (err == SKIFF_OK) {
            /* The picker joins with the network modules: the worker must not rejoin meanwhile.
             * While it does, the picker waits for the next frame. */
            if (!try_take_network(platform)) {
                return 0;
            }
            err = skiff_psp_dialog_start_network(dialog);
            if (err != SKIFF_OK) {
                give_network(platform);
            }
        }
    }
    if (err != SKIFF_OK) {
        skiff_log_write(skiff_app_log(platform->app), SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                        "dialog %d did not open: %s (%d)", (int)view->dialog, skiff_err_name(err),
                        (int)err);
        skiff_app_dialog_done(platform->app, SKIFF_APP_DIALOG_FAILED, NULL);
        return 0;
    }
    return 1;
}

void skiff_psp_app_frame(skiff_psp_app *platform, const skiff_app_view *view) {
    const int dialog_open = open_dialog(platform, view);
    skiff_psp_ui_begin_frame(&platform->ui);
    draw_view(platform, view);
    if (dialog_open) {
        skiff_psp_ui_backdrop(&platform->ui);
    }
    skiff_psp_ui_end_frame(&platform->ui);
    if (dialog_open) {
        const skiff_psp_dialog_state state = skiff_psp_dialog_update(platform->dialog);
        if (state != SKIFF_PSP_DIALOG_RUNNING) {
            dialog_ended(platform, state);
        }
    }
    skiff_psp_ui_present(&platform->ui);
}

void skiff_psp_app_close_dialog(skiff_psp_app *platform) {
    skiff_psp_dialog *dialog = platform->dialog;
    if (dialog == NULL || dialog->state != SKIFF_PSP_DIALOG_RUNNING) {
        return;
    }
    skiff_psp_dialog_close(dialog);
    const skiff_app_view *view = skiff_app_view_now(platform->app);
    while (dialog->state == SKIFF_PSP_DIALOG_RUNNING) {
        skiff_psp_ui_begin_frame(&platform->ui);
        if (view != NULL) {
            draw_view(platform, view);
        }
        skiff_psp_ui_backdrop(&platform->ui);
        skiff_psp_ui_end_frame(&platform->ui);
        const skiff_psp_dialog_state state = skiff_psp_dialog_update(dialog);
        skiff_psp_ui_present(&platform->ui);
        if (state != SKIFF_PSP_DIALOG_RUNNING) {
            dialog_ended(platform, state);
        }
    }
}

/* ---- Ending ---- */

int skiff_psp_app_finish(skiff_psp_app *platform) {
    skiff_psp_app_close_dialog(platform);
    if (platform->dialog_stuck) {
        return 0;
    }
    if (skiff_psp_worker_stop(&platform->worker, SKIFF_PSP_WORKER_STOP_TIMEOUT_US) != SKIFF_OK) {
        skiff_log_write(skiff_app_log(platform->app), SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                        "quit: the worker did not stop; the exit releases the rest");
        skiff_log_flush(skiff_app_log(platform->app));
        return 0;
    }
    skiff_app_destroy(platform->app);
    platform->app = NULL;
    int released = 1;
    if (platform->net_loaded) {
        /* Tearing the modules down under a live connection can hang the PSP: only after a
         * disconnect that succeeded (which also abandons a pending join). */
        released = skiff_psp_net_disconnect(&platform->net, SKIFF_PSP_APP_DISCONNECT_TIMEOUT_US) ==
                       SKIFF_OK &&
                   skiff_psp_net_unload(&platform->net) == SKIFF_OK;
    }
    give_network(platform);
    if (platform->tls_started) {
        skiff_net_global_cleanup();
    }
    if (platform->mutexes_created && released) {
        skiff_psp_mutex_destroy(&platform->log_lock);
        skiff_psp_mutex_destroy(&platform->jobs_lock);
        skiff_psp_mutex_destroy(&platform->jobs_save_lock);
        skiff_psp_mutex_destroy(&platform->manifest_lock);
        skiff_psp_mutex_destroy(&platform->net_lock);
    }
    if (platform->ui_started) {
        skiff_psp_ui_stop(&platform->ui);
    }
    skiff_psp_ui_unload_font(platform->font);
    skiff_storage_destroy(platform->storage);
    return released;
}
