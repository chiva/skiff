#include <stdlib.h>
#include <string.h>

#include "app_internal.h"

/* ---- On the platform's thread ---- */

/* A cover: from its slot in the Memory Stick cache, or else fetched from RomM, decoded and cached.
 * A cover that cannot be cached is still shown. */
static skiff_err fetch_cover(app_call *call) {
    const int64_t started = call->now_ms(call->clock_ctx);
    call->cover_cache_err =
        skiff_cover_cache_load(call->storage, &call->roots, &call->cover_key, call->cover);
    call->cover_cached = call->cover_cache_err == SKIFF_OK;
    if (call->cover_cached) {
        call->cover_fetch_ms = call->now_ms(call->clock_ctx) - started;
        return SKIFF_OK;
    }
    unsigned char *bytes = NULL;
    skiff_err err = skiff_romm_get_cover(call->romm, call->cover_path, &bytes, &call->cover_bytes);
    const int64_t fetched = call->now_ms(call->clock_ctx);
    call->cover_fetch_ms = fetched - started;
    if (err == SKIFF_OK) {
        err = skiff_cover_decode_png(bytes, call->cover_bytes, call->cover);
        call->cover_decode_ms = call->now_ms(call->clock_ctx) - fetched;
    }
    free(bytes);
    if (err == SKIFF_OK) {
        call->cover_store_err =
            skiff_cover_cache_store(call->storage, &call->roots, &call->cover_key, call->cover);
    }
    return err;
}

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
    case CALL_BATCH_PAGE: {
        const skiff_romm_list_query query = {.platform_id = call->platform_id,
                                             .filter = call->filter,
                                             .collection_id = call->collection_id,
                                             .with_files = call->with_files};
        call->err =
            skiff_romm_list_roms(call->romm, &query, call->page_index * SKIFF_ROMM_PAGE_SIZE,
                                 SKIFF_ROMM_PAGE_SIZE, &call->page);
        break;
    }
    case CALL_ROM:
        call->err = skiff_romm_get_rom(call->romm, call->rom_id, &call->rom);
        break;
    case CALL_PAIRING_START:
        call->err = skiff_romm_pairing_start(call->romm, call->device_identifier, &call->pairing);
        break;
    case CALL_PAIRING_POLL:
        call->err = skiff_romm_pairing_poll(call->romm, &call->pairing, &call->pairing_result);
        break;
    case CALL_COVER:
        call->err = fetch_cover(call);
        break;
    case CALL_BATCH_SPACE:
        call->err = skiff_storage_free_space(call->storage, call->roots.app, &call->free_bytes);
        break;
    case CALL_FREE_SPACE: {
        const int64_t started = call->now_ms(call->clock_ctx);
        call->err = skiff_storage_free_space(call->storage, call->roots.app, &call->free_bytes);
        call->free_space_ms = call->now_ms(call->clock_ctx) - started;
        break;
    }
    case CALL_BATCH_QUEUE:
        call->err = app_batch_queue_run(call->app, &call->app->batch);
        break;
    case CALL_COLLECTIONS:
        call->err = skiff_romm_list_collections(call->romm, &call->collections);
        break;
    case CALL_SAVES_CHECK:
        call->err = app_saves_check_run(call);
        break;
    case CALL_SAVES_APPLY:
        call->err = app_saves_apply_run(call);
        break;
    case CALL_SAVES_COMPLETE:
        call->err = app_saves_complete_run(call);
        break;
    case CALL_SAVES_BACKUPS:
        call->err = app_saves_backups_run(call);
        break;
    case CALL_SENDS_CLAIM:
        call->err = app_sends_claim_run(call);
        break;
    case CALL_SENDS_QUEUE:
        call->err = app_sends_queue_run(call);
        break;
    case CALL_NONE:
        call->err = SKIFF_ERR_INVALID_ARG;
        break;
    }
}

/* ---- On the screen's thread ---- */

/* Whether the call's work needs the Wi-Fi: leaving a save deleted, restoring a backup, settling a
 * restore a suspend cut, listing backups and queueing "Download all" only touch the Memory Stick.
 */
static int uses_network(const app_call *call) {
    if (call->kind == CALL_BATCH_SPACE || call->kind == CALL_BATCH_QUEUE ||
        call->kind == CALL_SAVES_BACKUPS || call->kind == CALL_FREE_SPACE) {
        return 0;
    }
    return call->kind != CALL_SAVES_APPLY ||
           (call->saves.op != SAVES_OP_LEAVE_DELETED && call->saves.op != SAVES_OP_RESTORE_BACKUP &&
            call->saves.op != SAVES_OP_SETTLE);
}

/* A request that failed across a suspend failed because of it (S1.7: a save's .part write, then a
 * CA file read while the Memory Stick came back), whatever error it saw: it is a lost connection,
 * which joins the Wi-Fi again before the next request. */
static void blame_suspend(skiff_app *app, app_call *call) {
    if (call->failed_to_start || !uses_network(call) || !app_is_suspend_error(call->err) ||
        app->env.suspends(app->env.ctx) == call->suspends_at_start) {
        return;
    }
    skiff_log_write(app->log, SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG,
                    "call %d: %s (%d) across a suspend, taken as %s (%d)", (int)call->kind,
                    skiff_err_name(call->err), (int)call->err,
                    skiff_err_name(SKIFF_ERR_NET_CONNECTION_LOST),
                    (int)SKIFF_ERR_NET_CONNECTION_LOST);
    call->err = SKIFF_ERR_NET_CONNECTION_LOST;
    call->suspended = 1;
}

int app_call_busy(const skiff_app *app) { return app->call.kind != CALL_NONE; }

void app_call_start(skiff_app *app, app_call_kind kind) {
    app_call *call = &app->call;
    call->kind = kind;
    call->abandoned = 0;
    call->failed_to_start = 0;
    call->suspended = 0;
    call->suspends_at_start = app->env.suspends(app->env.ctx);
    call->err = SKIFF_OK;
    call->romm = &app->romm;
    /* Kept apart from call->err: once started, the thread may write that before call_start()
     * returns. */
    const int settle = kind == CALL_SAVES_APPLY && call->saves.op == SAVES_OP_SETTLE;
    /* A settle and the startup free-space count touch only the Memory Stick: no client (nor
     * server, which there may not be yet) is needed for them. */
    skiff_err launch = settle || kind == CALL_FREE_SPACE ? SKIFF_OK : app_ensure_client(app);
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
    /* The startup free-space count belongs to no server or screen: it is always taken. */
    if (!app_call_busy(app) || app->call.abandoned || app->call.kind == CALL_FREE_SPACE) {
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
    case CALL_COVER:
        app_cover_done(app);
        break;
    case CALL_BATCH_PAGE:
        app_batch_page_done(app);
        break;
    case CALL_BATCH_SPACE:
        app_batch_space_done(app);
        break;
    case CALL_BATCH_QUEUE:
        app_batch_queued(app);
        break;
    case CALL_COLLECTIONS:
        app_collections_done(app);
        break;
    case CALL_SAVES_CHECK:
        app_saves_checked(app);
        break;
    case CALL_SAVES_APPLY:
        app_saves_applied(app);
        break;
    case CALL_SAVES_COMPLETE:
        app_saves_completed(app);
        break;
    case CALL_SAVES_BACKUPS:
        app_saves_backups_listed(app);
        break;
    case CALL_SENDS_CLAIM:
        app_sends_claimed(app);
        break;
    case CALL_SENDS_QUEUE:
        app_sends_queued(app);
        break;
    case CALL_FREE_SPACE:
        app_free_space_done(app);
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
    blame_suspend(app, call);
    const app_call_kind kind = call->kind;
    /* Idle before the result is applied: applying may start the next call. */
    call->kind = CALL_NONE;
    app->dirty = 1;
    const int drop_after = app->drop_after_call;
    app->drop_after_call = 0;
    if (call->drop_client) {
        call->drop_client = 0;
        app_drop_client(app);
    }
    if (call->abandoned) {
        skiff_log_write(app->log, SKIFF_LOG_DEBUG, SKIFF_APP_LOG_TAG,
                        "call %d: dropped after %s (%d)", (int)kind, skiff_err_name(call->err),
                        (int)call->err);
        app_network_failed(app, call->err);
        if (kind == CALL_SENDS_CLAIM || kind == CALL_SENDS_QUEUE) {
            app_sends_dropped(app);
        }
    } else {
        apply(app, kind);
    }
    if (drop_after) {
        app_drop_client(app);
    }
    /* A pairing's codes and token are wiped whatever became of them. */
    if (kind == CALL_PAIRING_START || kind == CALL_PAIRING_POLL) {
        skiff_romm_pairing_clear(&call->pairing, &call->pairing_result);
    }
}
