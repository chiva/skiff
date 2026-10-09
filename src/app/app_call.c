#include <string.h>

#include "app_internal.h"

/* ---- On the platform's thread ---- */

/* The request itself: it reads the call's inputs and the browse client, and writes only the
 * call's results. */
static void run(void *arg) {
    app_call *call = arg;
    switch (call->kind) {
    case CALL_HEARTBEAT:
        call->err = skiff_romm_heartbeat(call->romm, &call->server);
        break;
    case CALL_PLATFORM:
        call->err = skiff_romm_find_platform(call->romm, SKIFF_APP_PLATFORM_SLUG, &call->platform);
        break;
    case CALL_PAGE:
        call->err = skiff_romm_list_roms(call->romm, call->platform_id,
                                         call->page_index * SKIFF_ROMM_PAGE_SIZE,
                                         SKIFF_ROMM_PAGE_SIZE, &call->page);
        break;
    case CALL_ROM:
        call->err = skiff_romm_get_rom(call->romm, call->rom_id, &call->rom);
        break;
    case CALL_PAIRING_START:
        call->err = skiff_romm_pairing_start(call->romm, call->device_identifier, &call->pairing);
        break;
    case CALL_PAIRING_POLL:
        call->err = skiff_romm_pairing_poll(call->romm, &call->pairing, &call->pairing_result);
        break;
    case CALL_NONE:
        call->err = SKIFF_ERR_INVALID_ARG;
        break;
    }
}

/* ---- On the screen's thread ---- */

int app_call_busy(const skiff_app *app) { return app->call.kind != CALL_NONE; }

void app_call_start(skiff_app *app, app_call_kind kind) {
    app_call *call = &app->call;
    call->kind = kind;
    call->abandoned = 0;
    call->failed_to_start = 0;
    call->err = SKIFF_OK;
    call->romm = &app->romm;
    /* Kept apart from call->err: once started, the thread may write that before call_start()
     * returns. */
    skiff_err launch = app_ensure_client(app);
    if (launch == SKIFF_OK) {
        launch = app->env.call_start(app->env.ctx, run, call);
    }
    if (launch != SKIFF_OK) {
        call->err = launch;
        call->failed_to_start = 1;
        skiff_log_write(app->log, SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG, "call %d: %s (%d)", (int)kind,
                        skiff_err_name(launch), (int)launch);
    }
    app->dirty = 1;
}

void app_call_abandon(skiff_app *app) {
    if (!app_call_busy(app) || app->call.abandoned) {
        return;
    }
    app->call.abandoned = 1;
    if (!app->call.failed_to_start) {
        app->env.call_cancel(app->env.ctx);
    }
    skiff_log_write(app->log, SKIFF_LOG_DEBUG, SKIFF_APP_LOG_TAG, "call %d: abandoned",
                    (int)app->call.kind);
}

static void apply(skiff_app *app, app_call_kind kind) {
    switch (kind) {
    case CALL_HEARTBEAT:
        app_heartbeat_done(app);
        break;
    case CALL_PLATFORM:
        app_platform_done(app);
        break;
    case CALL_PAGE:
        app_page_done(app);
        break;
    case CALL_ROM:
        app_rom_done(app);
        break;
    case CALL_PAIRING_START:
        app_pairing_started(app);
        break;
    case CALL_PAIRING_POLL:
        app_pairing_polled(app);
        break;
    case CALL_NONE:
        break;
    }
}

void app_call_update(skiff_app *app) {
    app_call *call = &app->call;
    if (!app_call_busy(app) || (!call->failed_to_start && !app->env.call_done(app->env.ctx))) {
        return;
    }
    const app_call_kind kind = call->kind;
    /* Idle before the result is applied: applying may start the next call. */
    call->kind = CALL_NONE;
    app->dirty = 1;
    if (call->drop_client) {
        call->drop_client = 0;
        app_drop_client(app);
    }
    if (call->abandoned) {
        skiff_log_write(app->log, SKIFF_LOG_DEBUG, SKIFF_APP_LOG_TAG,
                        "call %d: dropped after %s (%d)", (int)kind, skiff_err_name(call->err),
                        (int)call->err);
        app_network_failed(app, call->err);
    } else {
        apply(app, kind);
    }
    /* A pairing's codes and token are wiped whatever became of them. */
    if (kind == CALL_PAIRING_START || kind == CALL_PAIRING_POLL) {
        skiff_romm_pairing_clear(&call->pairing, &call->pairing_result);
    }
}
