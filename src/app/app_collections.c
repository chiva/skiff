/*
 * The collections list, the library's third source (SELECT after the favourites): the RomM user's
 * collections and every other user's public ones, regular then smart, each with RomM's ROM count.
 * Opening one lists its PSP games in the library (app_library.c), and Back comes back here.
 *
 * Listing collections needs collections.read, which pairing asks for as an optional scope: a token
 * without it in [auth] scopes (or one RomM answers 403) shows "Pair again" instead. The list loads
 * each time it is entered, on the browsing thread like every request; when it cannot load (too
 * large for the PSP, a lost connection), the screen says why and offers Retry, and SELECT still
 * leaves it.
 */
#include <stdio.h>
#include <string.h>

#include "app_internal.h"

int app_collections_denied(const skiff_app *app) {
    return app->collections_forbidden ||
           !skiff_config_has_scope(&app->settings, SKIFF_ROMM_SCOPE_COLLECTIONS_READ);
}

/* The list's call runs, and its result will be kept. */
static int collections_loading(const skiff_app *app) {
    return app->call.kind == CALL_COLLECTIONS && !app->call.abandoned;
}

void app_collections_request(skiff_app *app) {
    if (app->source != SOURCE_COLLECTIONS || app->collection_open || app->collections_known ||
        app->collections_error != SKIFF_OK || app_collections_denied(app) ||
        collections_loading(app)) {
        return;
    }
    app->request = REQUEST_COLLECTIONS;
    app->dirty = 1;
}

void app_collections_enter(skiff_app *app) {
    app->collections_known = 0;
    app->collections.count = 0;
    app->collections_error = SKIFF_OK;
    skiff_ui_list_init(&app->collections_list, 0, APP_LIBRARY_ROWS);
    app_collections_request(app);
}

void app_collections_forget(skiff_app *app) {
    app->collection_open = 0;
    app->collections_known = 0;
    app->collections.count = 0;
    app->collections_error = SKIFF_OK;
    app->collections_forbidden = 0;
    skiff_ui_list_init(&app->collections_list, 0, APP_LIBRARY_ROWS);
}

void app_collections_run(skiff_app *app) {
    /* Asked for, then left (or loaded meanwhile): nothing to do. */
    if (app->source != SOURCE_COLLECTIONS || app->collection_open || app->collections_known ||
        app_collections_denied(app) || collections_loading(app)) {
        return;
    }
    app_call_start(app, CALL_COLLECTIONS);
}

const char *app_collection_name(const skiff_app *app, const skiff_romm_collection *collection) {
    return collection->name[0] != '\0' ? collection->name
                                       : app_text(app, SKIFF_TEXT_COLLECTION_UNNAMED);
}

void app_collections_done(skiff_app *app) {
    const app_call *call = &app->call;
    const skiff_err err = call->err;
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_ERROR,
                    SKIFF_APP_LOG_TAG, "collections: %zu: %s (%d)",
                    err == SKIFF_OK ? call->collections.count : 0, skiff_err_name(err), (int)err);
    /* The player left the list meanwhile: dropped, and loaded again when it is entered again. */
    if (app->source != SOURCE_COLLECTIONS || app->collection_open || app->collections_known) {
        app_network_failed(app, err);
        return;
    }
    app->dirty = 1;
    if (err == SKIFF_ERR_ROMM_FORBIDDEN) {
        app->collections_forbidden = 1;
        return;
    }
    if (err != SKIFF_OK) {
        app_network_failed(app, err);
        app->collections_error = err;
        return;
    }
    app->collections = call->collections;
    for (size_t i = 0; i < app->collections.count; i++) {
        app_fit(app, app_collection_name(app, &app->collections.items[i]), SKIFF_APP_LABEL_WIDTH,
                app->collection_labels[i], sizeof app->collection_labels[i]);
    }
    app->collections_known = 1;
    skiff_ui_list_init(&app->collections_list, app->collections.count, APP_LIBRARY_ROWS);
}

int app_collections_update(skiff_app *app, unsigned actions) {
    const int confirm = (actions & SKIFF_UI_ACTION_CONFIRM) != 0;
    if (app_collections_denied(app)) {
        /* A new pairing replaces the browse client: not under a running request. It keeps the
         * address, the downloads and what Skiff installed. */
        if (confirm && !app_call_busy(app)) {
            app_pair_begin(app);
        }
        return 0;
    }
    if (app->collections_error != SKIFF_OK) {
        if (confirm) {
            app->collections_error = SKIFF_OK;
            app->dirty = 1;
            app_collections_request(app);
        }
        return 0;
    }
    if (!app->collections_known) {
        return 0;
    }
    if (skiff_ui_list_apply(&app->collections_list, actions)) {
        app->dirty = 1;
    }
    return confirm && app->collections.count > 0;
}
