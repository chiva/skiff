#include "app_psp.h"

#include <malloc.h>
#include <psa/crypto.h>
#include <pspctrl.h>
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspwlan.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/curl_transport.h"

#include "kirk_entropy.h"
#include "lifecycle.h"
#include "storage_psp.h"

#define US_PER_MS 1000LL
#define BYTES_PER_KB 1024U
/* sceWlanGetSwitchState() with the switch off (pspwlan.h names no value). */
#define WLAN_SWITCH_OFF 0
/* A name lookup waits this long per try, with this many more tries (about 15 s in all), against the
 * minutes curl's unbounded lookup took on a PSP whose DNS server did not answer (0.2.0,
 * 2026-10-09).
 */
#define RESOLVE_TIMEOUT_S 5U
#define RESOLVE_RETRIES 2
/* The most one of the screen's requests may take, the lookup aside: a page of the library or a
 * game's details is tens of KB, seconds even on poor Wi-Fi. A request that still trickles after
 * this ends as a timeout the player can retry, instead of holding the screen. */
#define BROWSE_TIMEOUT_S 60L

enum {
    LINE_HEIGHT = SKIFF_APP_LINE_HEIGHT,
    LINE_BASELINE = 10,
    BLOCK_GAP = SKIFF_APP_BLOCK_GAP,
    PROGRESS_HEIGHT = SKIFF_APP_PROGRESS_HEIGHT,
};

/* The app sizes its lists by its body geometry: it must be the body this renderer draws. */
_Static_assert(SKIFF_APP_BODY_HEIGHT == SKIFF_PSP_UI_CONTENT_BOTTOM - SKIFF_PSP_UI_CONTENT_TOP,
               "skiff/app.h's body height must be the area between header and footer");
_Static_assert(SKIFF_APP_BLOCK_GAP + SKIFF_COVER_HEIGHT <= SKIFF_APP_BODY_HEIGHT,
               "the cover box must fit the body");
_Static_assert(SKIFF_APP_LINE_HEIGHT == SKIFF_PSP_UI_ROW_HEIGHT,
               "skiff/app.h's line height must be a list row's");

/* ---- Measurements (hardware row A1, debug level) ---- */

static unsigned heap_used_kb(void) {
    const struct mallinfo heap = mallinfo();
    return (unsigned)((size_t)heap.uordblks / BYTES_PER_KB);
}

/* One line per request on the UI thread: the first one on a new connection carries the TLS
 * handshake and the parsing of the trusted CAs, and the heap after it what they keep. */
static void log_request(skiff_psp_app *platform, long long started_us,
                        const skiff_http_response *response, skiff_err err) {
    skiff_log_write(skiff_app_log(platform->app), SKIFF_LOG_DEBUG, SKIFF_APP_LOG_TAG,
                    "request: %lld ms, HTTP %ld, %llu body bytes, %ld new connection(s), %s %s, "
                    "heap %u KB: %s (%d)",
                    (sceKernelGetSystemTimeWide() - started_us) / US_PER_MS, response->status,
                    (unsigned long long)response->body_bytes, response->new_connections,
                    response->tls_version, response->tls_cipher, heap_used_kb(),
                    skiff_err_name(err), (int)err);
}

static void count_frame(skiff_psp_app *platform) {
    skiff_psp_app_stats *stats = &platform->stats;
    const long long now = sceKernelGetSystemTimeWide();
    if (stats->last_frame_us != 0) {
        const long long gap = now - stats->last_frame_us;
        stats->gap_total_us += gap;
        stats->gap_max_us = gap > stats->gap_max_us ? gap : stats->gap_max_us;
        stats->frames++;
        if (gap >= SKIFF_PSP_APP_SLOW_FRAME_US) {
            /* The rest is the time between two steps (the main loop) or a suspend. */
            skiff_log_write(skiff_app_log(platform->app), SKIFF_LOG_DEBUG, SKIFF_APP_LOG_TAG,
                            "slow frame: %lld ms: buttons %lld, update %lld, draw %lld, present "
                            "%lld ms",
                            gap / US_PER_MS, stats->input_us / US_PER_MS,
                            stats->update_us / US_PER_MS, stats->draw_us / US_PER_MS,
                            stats->present_us / US_PER_MS);
        }
    } else {
        stats->period_start_us = now;
    }
    stats->last_frame_us = now;
    if (now - stats->period_start_us < SKIFF_PSP_APP_STATS_PERIOD_US || stats->frames == 0) {
        return;
    }
    skiff_log_write(
        skiff_app_log(platform->app), SKIFF_LOG_DEBUG, SKIFF_APP_LOG_TAG,
        "stats: %d frames, mean %lld ms, longest %lld ms; heap %u KB; system free %u KB "
        "(largest %u KB); worker stack lowest free %d bytes, browse %d",
        stats->frames, stats->gap_total_us / stats->frames / US_PER_MS,
        stats->gap_max_us / US_PER_MS, heap_used_kb(),
        (unsigned)(sceKernelTotalFreeMemSize() / BYTES_PER_KB),
        (unsigned)(sceKernelMaxFreeMemSize() / BYTES_PER_KB),
        skiff_psp_worker_stack_free(&platform->worker),
        skiff_psp_caller_stack_free(&platform->caller));
    stats->period_start_us = now;
    stats->gap_total_us = 0;
    stats->gap_max_us = 0;
    stats->frames = 0;
}

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

/* Loads the network modules unless they all are, with net_lock held: the worker's reload may have
 * left them unloaded or half loaded. */
static skiff_err load_network(skiff_psp_app *platform) {
    if (platform->net.stage == SKIFF_PSP_NET_APCTL) {
        return SKIFF_OK;
    }
    /* Whatever is up from a failed load goes down first, so this one starts clean. A layer that
     * will not come down keeps its stage: loading over it would lose track of it, so the error is
     * returned instead (the next try unloads again). */
    if (platform->net.stage != SKIFF_PSP_NET_NONE) {
        const skiff_err err = skiff_psp_net_unload(&platform->net);
        if (err != SKIFF_OK) {
            return err;
        }
    }
    const skiff_err err = skiff_psp_net_load(&platform->net, SKIFF_PSP_NET_CPU_MHZ);
    if (err != SKIFF_OK) {
        (void)skiff_psp_net_unload(&platform->net);
    }
    return err;
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
    skiff_err err = load_network(platform);
    if (err != SKIFF_OK) {
        give_network(platform);
        return err;
    }
    if (skiff_psp_net_online(&platform->net) == SKIFF_OK) {
        give_network(platform);
        *joined = 1;
        return SKIFF_OK;
    }
    /* A connection half gone (out of range, after a suspend) is dropped before joining again. */
    (void)skiff_psp_net_disconnect(&platform->net, SKIFF_PSP_APP_DROP_TIMEOUT_US);
    err = skiff_psp_net_connect_start(&platform->net, platform->join_profile,
                                      SKIFF_PSP_APP_JOIN_TIMEOUT_US);
    if (err != SKIFF_OK) {
        give_network(platform);
    }
    return err;
}

static skiff_err net_start(void *ctx, int profile) {
    skiff_psp_app *platform = ctx;
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
    /* Read before the lock goes: the worker's rejoin may overwrite them right after. */
    const char *failed_call = platform->net.failed_call != NULL ? platform->net.failed_call : "-";
    const int sce_result = platform->net.sce_result;
    give_network(platform);
    *joined = result == SKIFF_OK;
    if (result != SKIFF_OK) {
        skiff_log_write(skiff_app_log(platform->app), SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG,
                        "join: %s 0x%08X", failed_call, (unsigned)sce_result);
    }
    return result;
}

static skiff_err net_profile(void *ctx, int *profile) {
    const skiff_psp_app *platform = ctx;
    if (platform->picked_error == SKIFF_OK) {
        *profile = platform->picked_profile;
    }
    return platform->picked_error;
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

/* curl's lookups cannot time out on the PSP (skiff/curl_transport.h): every transport looks names
 * up here instead, within RESOLVE_TIMEOUT_S per try and RESOLVE_RETRIES more tries. Called from the
 * UI thread and the worker alike; the log is thread-safe. */
static skiff_err resolve(void *ctx, const char *host, char *address, size_t address_size) {
    skiff_psp_app *platform = ctx;
    const long long started_us = sceKernelGetSystemTimeWide();
    const skiff_err err =
        skiff_psp_net_resolve(host, RESOLVE_TIMEOUT_S, RESOLVE_RETRIES, address, address_size);
    skiff_log_write(
        skiff_app_log(platform->app), err == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_WARN,
        SKIFF_APP_LOG_TAG, "resolve %s: %lld ms: %s (%d)", host,
        (sceKernelGetSystemTimeWide() - started_us) / US_PER_MS, skiff_err_name(err), (int)err);
    return err;
}

/* total_timeout_s: BROWSE_TIMEOUT_S for the screen's requests, 0 (none) for downloads. */
static skiff_curl_config curl_config(skiff_psp_app *platform,
                                     const skiff_app_transport_settings *settings,
                                     long total_timeout_s) {
    const skiff_curl_config curl = {
        .ca_file = settings->ca_file[0] != '\0' ? settings->ca_file : NULL,
        .client_cert = settings->client_cert[0] != '\0' ? settings->client_cert : NULL,
        .client_key = settings->client_key[0] != '\0' ? settings->client_key : NULL,
        .default_headers = settings->headers,
        .default_header_count = settings->header_count,
        .total_timeout_s = total_timeout_s,
        .resolve = resolve,
        .resolve_ctx = platform,
    };
    return curl;
}

/* The browse transport, used on the browsing thread (call_psp.h): each request holds net_lock, so
 * the worker cannot disconnect or unload the network under it. The lock is taken without waiting:
 * while the worker recovers the network (or the UI joins), the request fails as a lost connection,
 * and the player's retry joins again once it is free. A cancelled call stops its transfer. */
typedef struct guarded_transport {
    skiff_transport base;
    skiff_transport *inner;
    skiff_psp_app *platform;
} guarded_transport;

/* The request's own stop hook, and the call's cancel before it. */
typedef struct guarded_stop {
    skiff_http_stop_fn inner;
    void *inner_ctx;
    const skiff_psp_caller *caller;
} guarded_stop;

static skiff_err guarded_should_stop(void *ctx) {
    const guarded_stop *stop = ctx;
    if (skiff_psp_caller_cancelled(stop->caller)) {
        return SKIFF_ERR_CANCELLED;
    }
    return stop->inner != NULL ? stop->inner(stop->inner_ctx) : SKIFF_OK;
}

static skiff_err guarded_perform(skiff_transport *transport, const skiff_http_request *request,
                                 skiff_http_response *response) {
    guarded_transport *guarded = (guarded_transport *)transport;
    skiff_psp_app *platform = guarded->platform;
    if (!skiff_psp_mutex_try_lock(&platform->net_lock)) {
        return SKIFF_ERR_NET_CONNECTION_LOST;
    }
    guarded_stop stop = {request->should_stop, request->stop_ctx, &platform->caller};
    skiff_http_request stoppable = *request;
    stoppable.should_stop = guarded_should_stop;
    stoppable.stop_ctx = &stop;
    const long long started_us = sceKernelGetSystemTimeWide();
    const skiff_err err = guarded->inner->ops->perform(guarded->inner, &stoppable, response);
    skiff_psp_mutex_unlock(&platform->net_lock);
    log_request(platform, started_us, response, err);
    return err;
}

static void guarded_destroy(skiff_transport *transport) {
    guarded_transport *guarded = (guarded_transport *)transport;
    skiff_transport_destroy(guarded->inner);
    free(guarded);
}

static const skiff_transport_ops GUARDED_OPS = {guarded_perform, guarded_destroy};

static skiff_err open_transport(void *ctx, const skiff_app_transport_settings *settings,
                                skiff_transport **out) {
    skiff_psp_app *platform = ctx;
    *out = NULL;
    guarded_transport *guarded = calloc(1, sizeof *guarded);
    if (guarded == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    const skiff_curl_config curl = curl_config(platform, settings, BROWSE_TIMEOUT_S);
    const skiff_err err = skiff_curl_transport_create(&curl, &guarded->inner);
    if (err != SKIFF_OK) {
        free(guarded);
        return err;
    }
    guarded->base.ops = &GUARDED_OPS;
    guarded->platform = platform;
    *out = &guarded->base;
    return SKIFF_OK;
}

/* The browsing thread starts with the first call: no stack is taken before Skiff goes online. */
static skiff_err call_start(void *ctx, skiff_app_call_fn fn, void *arg) {
    skiff_psp_app *platform = ctx;
    if (!platform->caller.started) {
        const skiff_err err =
            skiff_psp_caller_start(&platform->caller, skiff_app_log(platform->app));
        if (err != SKIFF_OK) {
            return err;
        }
    }
    return skiff_psp_caller_call(&platform->caller, fn, arg);
}

static int call_done(void *ctx) {
    skiff_psp_app *platform = ctx;
    return skiff_psp_caller_done(&platform->caller);
}

static void call_cancel(void *ctx) {
    skiff_psp_app *platform = ctx;
    skiff_psp_caller_cancel(&platform->caller);
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
        .curl = curl_config(platform, spec->transport, 0),
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
        .call_start = call_start,
        .call_done = call_done,
        .call_cancel = call_cancel,
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

static unsigned read_input(skiff_psp_app *platform) {
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

/* The view's QR code in the body's top right corner, as large as SKIFF_APP_QR_WIDTH and the body's
 * height allow. */
static void draw_qr(const skiff_psp_ui *ui, const skiff_ui_qr *qr) {
    const int width = (int)SKIFF_APP_QR_WIDTH;
    const int height = SKIFF_PSP_UI_CONTENT_BOTTOM - SKIFF_PSP_UI_CONTENT_TOP - 2 * BLOCK_GAP;
    const int scale = skiff_ui_qr_scale(qr, width < height ? width : height);
    const int side = (qr->size + 2 * SKIFF_UI_QR_QUIET_ZONE) * scale;
    skiff_psp_ui_qr(ui, SKIFF_PSP_UI_SCREEN_WIDTH - SKIFF_PSP_UI_MARGIN - side,
                    SKIFF_PSP_UI_CONTENT_TOP + BLOCK_GAP, scale, qr);
}

/* The details screen's cover box in the body's top right corner: the placeholder, and the cover
 * over it when it is in, centred across the box and at its top. */
static void draw_cover(const skiff_psp_ui *ui, const skiff_cover *cover) {
    const int x = SKIFF_PSP_UI_SCREEN_WIDTH - SKIFF_PSP_UI_MARGIN - SKIFF_COVER_WIDTH;
    const int y = SKIFF_PSP_UI_CONTENT_TOP + BLOCK_GAP;
    skiff_psp_ui_rect(ui, x, y, SKIFF_COVER_WIDTH, SKIFF_COVER_HEIGHT,
                      SKIFF_PSP_UI_COLOUR_COVER_BOX);
    if (cover != NULL) {
        skiff_psp_ui_cover(ui, x + (SKIFF_COVER_WIDTH - cover->width) / 2, y, cover);
    }
}

/* The view's lines from y down, then its progress bar: where they end. */
static int draw_details(const skiff_psp_ui *ui, const skiff_app_view *view, int y) {
    for (size_t i = 0; i < view->line_count; i++) {
        skiff_psp_ui_text(ui, SKIFF_PSP_UI_MARGIN, y + LINE_BASELINE, SKIFF_PSP_UI_TEXT_SIZE,
                          SKIFF_PSP_UI_COLOUR_TEXT, view->lines[i]);
        y += LINE_HEIGHT;
    }
    if (view->has_progress) {
        y += BLOCK_GAP;
        skiff_psp_ui_progress_bar(ui, SKIFF_PSP_UI_MARGIN, y,
                                  SKIFF_PSP_UI_SCREEN_WIDTH - 2 * SKIFF_PSP_UI_MARGIN,
                                  PROGRESS_HEIGHT, view->percent);
        y += PROGRESS_HEIGHT;
    }
    return y;
}

static void draw_view(const skiff_psp_app *platform, const skiff_app_view *view) {
    const skiff_psp_ui *ui = &platform->ui;
    skiff_psp_ui_header(ui, view->title, view->status[0] != '\0' ? view->status : NULL);
    if (view->qr != NULL) {
        draw_qr(ui, view->qr);
    }
    if (view->has_cover_box) {
        draw_cover(ui, view->cover);
    }
    const int top = SKIFF_PSP_UI_CONTENT_TOP + BLOCK_GAP;
    if (view->lines_below) {
        if (view->has_list) {
            skiff_psp_ui_rows(ui, &view->list, top, view_row, (void *)view);
        }
        const int height = (int)view->line_count * LINE_HEIGHT +
                           (view->has_progress ? BLOCK_GAP + PROGRESS_HEIGHT : 0);
        (void)draw_details(ui, view, SKIFF_PSP_UI_CONTENT_BOTTOM - height);
    } else {
        int y = draw_details(ui, view, top);
        if (view->has_list) {
            if (view->line_count > 0 || view->has_progress) {
                y += BLOCK_GAP;
            }
            skiff_psp_ui_rows(ui, &view->list, y, view_row, (void *)view);
        }
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
    /* The connection the picker joined is read while the worker cannot touch the network. A stuck
     * picker may still use the network: the lock then stays held until the process exits. */
    if (dialog->kind == SKIFF_PSP_DIALOG_NETWORK && state != SKIFF_PSP_DIALOG_STUCK) {
        platform->picked_error =
            state == SKIFF_PSP_DIALOG_ACCEPTED
                ? skiff_psp_net_connected_profile(&platform->net, &platform->picked_profile)
                : SKIFF_ERR_NET_UNAVAILABLE;
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
        /* The picker joins with the network modules: the worker must not rejoin meanwhile.
         * While it does, the picker waits for the next frame. */
        if (!try_take_network(platform)) {
            return 0;
        }
        err = load_network(platform);
        if (err == SKIFF_OK) {
            err = skiff_psp_dialog_start_network(dialog);
        }
        if (err != SKIFF_OK) {
            give_network(platform);
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

static void draw_frame(skiff_psp_app *platform, const skiff_app_view *view) {
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
    const long long drawn = sceKernelGetSystemTimeWide();
    skiff_psp_ui_present(&platform->ui);
    platform->stats.present_us = sceKernelGetSystemTimeWide() - drawn;
}

const skiff_app_view *skiff_psp_app_step(skiff_psp_app *platform) {
    skiff_psp_app_stats *stats = &platform->stats;
    const long long started = sceKernelGetSystemTimeWide();
    const unsigned actions = read_input(platform);
    const long long read = sceKernelGetSystemTimeWide();
    skiff_app_update(platform->app, actions);
    const skiff_app_view *view = skiff_app_view_now(platform->app);
    const long long updated = sceKernelGetSystemTimeWide();
    draw_frame(platform, view);
    stats->input_us = read - started;
    stats->update_us = updated - read;
    stats->draw_us = sceKernelGetSystemTimeWide() - updated - stats->present_us;
    count_frame(platform);
    return view;
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
    /* A request still running uses the app's client and call: they stay to the process exit. */
    if (skiff_psp_caller_stop(&platform->caller, SKIFF_PSP_CALLER_STOP_TIMEOUT_US) != SKIFF_OK) {
        skiff_log_flush(skiff_app_log(platform->app));
        return 0;
    }
    skiff_app_destroy(platform->app);
    platform->app = NULL;
    int released = 1;
    if (platform->net.stage != SKIFF_PSP_NET_NONE) {
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
