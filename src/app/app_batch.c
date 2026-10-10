/*
 * "Download all favourites": START on the favourites checks every one of them, then asks to queue
 * what fits. The pages, the free space and the queueing run on the browsing thread, one call at a
 * time, so the screen never waits for RomM or the Memory Stick; the screen's thread only sorts each
 * page's games as it arrives (memory lookups) and asks.
 *
 * A game is left out when it is installed, changed in RomM (replacing stays a choice made game by
 * game), already in Downloads, or one Skiff cannot install. The rest are taken in name order while
 * they fit the queue (SKIFF_JOBS_MAX unfinished), installed.json (SKIFF_INSTALL_RECORDS_MAX
 * records, those queued downloads will need included) and the free space (with the downloads still
 * to come and SKIFF_STORAGE_FREE_MARGIN_BYTES); what does not fit is counted and said, never
 * queued.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app_internal.h"

/* The placeholders a confirmation line fills, as text. */
#define NUMBER_TEXT_MAX 24

static void end_batch(skiff_app *app) {
    app->batch.step = BATCH_NONE;
    app->dirty = 1;
}

void app_batch_start(skiff_app *app) {
    if (app->batch.step != BATCH_NONE) {
        return;
    }
    app_batch *batch = &app->batch;
    memset(batch, 0, offsetof(app_batch, games));
    app_queue_refresh(app);
    const size_t unfinished = app_queue_unfinished(app);
    batch->queue_room = unfinished < SKIFF_JOBS_MAX ? SKIFF_JOBS_MAX - unfinished : 0;
    batch->record_room = app_installed_records_free(app);
    batch->pending_bytes = app_queue_unfinished_bytes(app);
    batch->step = BATCH_SCANNING;
    app->dirty = 1;
    skiff_log_write(app->log, SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG,
                    "download all favourites: room for %zu downloads, %zu records",
                    batch->queue_room, batch->record_room);
}

void app_batch_cancel(skiff_app *app) {
    if (app->batch.step == BATCH_NONE || app->batch.step == BATCH_QUEUEING) {
        return;
    }
    if (app_call_busy(app) &&
        (app->call.kind == CALL_BATCH_PAGE || app->call.kind == CALL_BATCH_SPACE)) {
        app_call_abandon(app);
    }
    skiff_log_write(app->log, SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG,
                    "download all favourites: stopped");
    end_batch(app);
}

void app_batch_update(skiff_app *app) {
    app_batch *batch = &app->batch;
    if (app_call_busy(app) || (batch->step != BATCH_SCANNING && batch->step != BATCH_SPACE)) {
        return;
    }
    /* A lost network is joined again; the player starts the batch again from there. */
    if (!app->net_joined) {
        end_batch(app);
        app_connect_begin(app, CONNECT_NETWORK);
        return;
    }
    if (batch->step == BATCH_SCANNING) {
        app->call.platform_id = app->platform.id;
        app->call.favourites = 1;
        app->call.with_files = 1;
        app->call.page_index = batch->next_page;
        app_call_start(app, CALL_BATCH_PAGE);
        return;
    }
    app->call.storage = app->config.storage;
    app->call.roots = app->config.roots;
    app->call.free_bytes = 0;
    app_call_start(app, CALL_BATCH_SPACE);
}

/* Sorts one favourite: counted as left out, or chosen while the queue and installed.json have
 * room. */
static void take(skiff_app *app, const skiff_romm_rom_summary *rom) {
    app_batch *batch = &app->batch;
    /* Offset pages can repeat a game when the favourites change while they are read: once is
     * enough, and a second copy must not take room another game could use. */
    for (size_t i = 0; i < batch->count; i++) {
        if (batch->games[i].id == rom->id) {
            return;
        }
    }
    const int installable =
        rom->has_file && rom->file.size <= SKIFF_STORAGE_MAX_FILE_BYTES &&
        skiff_install_check(app->installer, rom, &rom->file) == SKIFF_INSTALL_SUPPORTED;
    if (!installable) {
        batch->refused++;
        return;
    }
    if (app_queue_job_for(app, rom->id) != NULL) {
        batch->queued++;
        return;
    }
    app_lock_manifest(app);
    const skiff_install_state state = skiff_install_state_of(app->manifest, rom);
    app_unlock_manifest(app);
    if (state == SKIFF_INSTALL_INSTALLED) {
        batch->installed++;
    } else if (state == SKIFF_INSTALL_CHANGED) {
        batch->changed++;
    } else if (batch->count >= batch->queue_room) {
        batch->over_queue++;
    } else if (batch->count >= batch->record_room) {
        batch->over_records++;
    } else {
        batch->games[batch->count++] = *rom;
    }
}

void app_batch_page_done(skiff_app *app) {
    app_batch *batch = &app->batch;
    const app_call *call = &app->call;
    const skiff_err err = call->err;
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_ERROR,
                    SKIFF_APP_LOG_TAG, "download all favourites: page %llu, %zu of %llu: %s (%d)",
                    (unsigned long long)call->page_index, err == SKIFF_OK ? call->page.count : 0,
                    (unsigned long long)(err == SKIFF_OK ? call->page.total : 0),
                    skiff_err_name(err), (int)err);
    if (batch->step != BATCH_SCANNING) {
        return;
    }
    if (err != SKIFF_OK) {
        end_batch(app);
        app_network_failed(app, err);
        app_show_error(app, err, NULL, MESSAGE_BACK, MESSAGE_NONE, SKIFF_TEXT_OK,
                       SKIFF_APP_SCREEN_LIBRARY);
        return;
    }
    for (size_t i = 0; i < call->page.count; i++) {
        take(app, &call->page.items[i]);
    }
    batch->total = call->page.total;
    batch->seen += call->page.count;
    batch->next_page++;
    /* The favourites may change meanwhile: an empty page ends the list too. */
    if (call->page.count == 0 || batch->seen >= batch->total) {
        batch->step = BATCH_SPACE;
    }
    app->dirty = 1;
}

/* Keeps the longest run of chosen games, in order, that fits the free space. */
static void fit_space(app_batch *batch) {
    batch->bytes = 0;
    if (!batch->free_known) {
        for (size_t i = 0; i < batch->count; i++) {
            batch->bytes += batch->games[i].file.size;
        }
        return;
    }
    const uint64_t reserved = batch->pending_bytes + SKIFF_STORAGE_FREE_MARGIN_BYTES;
    const uint64_t room = batch->free_bytes > reserved ? batch->free_bytes - reserved : 0;
    size_t fits = 0;
    while (fits < batch->count && batch->games[fits].file.size <= room - batch->bytes) {
        batch->bytes += batch->games[fits].file.size;
        fits++;
    }
    batch->over_space = batch->count - fits;
    batch->count = fits;
}

/* "{1} ..." with count, and a second placeholder when the line has one. */
static size_t add_line(const skiff_app *app, char lines[][SKIFF_TEXT_MAX], size_t capacity,
                       size_t used, skiff_text_id id, size_t count, const char *second) {
    if (count == 0 || used >= capacity) {
        return used;
    }
    char number[NUMBER_TEXT_MAX];
    snprintf(number, sizeof number, "%zu", count);
    const char *args[] = {number, second};
    app_format(app, id, args, second != NULL ? 2 : 1, lines[used]);
    return used + 1;
}

size_t app_batch_lines(const skiff_app *app, char lines[][SKIFF_TEXT_MAX], size_t capacity) {
    const app_batch *batch = &app->batch;
    size_t used = 0;
    char size[SKIFF_APP_DETAIL_MAX];
    if (batch->count > 0 && capacity > 0) {
        char number[NUMBER_TEXT_MAX];
        snprintf(number, sizeof number, "%zu", batch->count);
        app_format_bytes(app, batch->bytes, size, sizeof size);
        const char *args[] = {number, size};
        app_format(app, SKIFF_TEXT_BATCH_QUESTION, args, 2, lines[used++]);
    } else if (capacity > 0) {
        snprintf(lines[used++], SKIFF_TEXT_MAX, "%s", app_text(app, SKIFF_TEXT_BATCH_NOTHING));
    }
    char queue_max[NUMBER_TEXT_MAX];
    snprintf(queue_max, sizeof queue_max, "%d", SKIFF_JOBS_MAX);
    char records_max[NUMBER_TEXT_MAX];
    snprintf(records_max, sizeof records_max, "%d", SKIFF_INSTALL_RECORDS_MAX);
    app_format_bytes(app, batch->free_bytes, size, sizeof size);
    used = add_line(app, lines, capacity, used, SKIFF_TEXT_BATCH_INSTALLED, batch->installed, NULL);
    used = add_line(app, lines, capacity, used, SKIFF_TEXT_BATCH_QUEUED, batch->queued, NULL);
    used = add_line(app, lines, capacity, used, SKIFF_TEXT_BATCH_CHANGED, batch->changed, NULL);
    used = add_line(app, lines, capacity, used, SKIFF_TEXT_BATCH_REFUSED, batch->refused, NULL);
    used = add_line(app, lines, capacity, used, SKIFF_TEXT_BATCH_OVER_QUEUE, batch->over_queue,
                    queue_max);
    used = add_line(app, lines, capacity, used, SKIFF_TEXT_BATCH_OVER_RECORDS, batch->over_records,
                    records_max);
    used =
        add_line(app, lines, capacity, used, SKIFF_TEXT_BATCH_OVER_SPACE, batch->over_space, size);
    return used;
}

void app_batch_space_done(skiff_app *app) {
    app_batch *batch = &app->batch;
    const app_call *call = &app->call;
    if (batch->step != BATCH_SPACE) {
        return;
    }
    /* A Memory Stick that cannot say is not held against the batch: each download checks room. */
    batch->free_known = call->err == SKIFF_OK;
    batch->free_bytes = batch->free_known ? call->free_bytes : 0;
    fit_space(batch);
    skiff_log_write(app->log, SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG,
                    "download all favourites: %llu checked, %zu to download (%llu bytes); "
                    "installed %zu, queued %zu, changed %zu, refused %zu, over queue %zu, over "
                    "records %zu, over space %zu; free space %s",
                    (unsigned long long)batch->seen, batch->count, (unsigned long long)batch->bytes,
                    batch->installed, batch->queued, batch->changed, batch->refused,
                    batch->over_queue, batch->over_records, batch->over_space,
                    skiff_err_name(call->err));
    if (batch->count == 0) {
        char lines[SKIFF_APP_LINES_MAX][SKIFF_TEXT_MAX];
        const size_t count = app_batch_lines(app, lines, SKIFF_APP_LINES_MAX);
        end_batch(app);
        app_show_text(app, SKIFF_TEXT_TITLE_NOTICE, lines[0], MESSAGE_BACK,
                      SKIFF_APP_SCREEN_LIBRARY);
        for (size_t i = 1; i < count; i++) {
            app_message_add(app, lines[i]);
        }
        return;
    }
    batch->step = BATCH_CONFIRMING;
    app_confirm(app, SKIFF_TEXT_BATCH_QUESTION, CONFIRM_DOWNLOAD_ALL, 0);
}

void app_batch_confirmed(skiff_app *app) {
    app_batch *batch = &app->batch;
    if (batch->step != BATCH_CONFIRMING || app_call_busy(app)) {
        return;
    }
    const skiff_err worker = app_start_worker(app);
    if (worker != SKIFF_OK) {
        end_batch(app);
        app_show_error(app, worker, NULL, MESSAGE_BACK, MESSAGE_NONE, SKIFF_TEXT_OK,
                       SKIFF_APP_SCREEN_LIBRARY);
        return;
    }
    batch->step = BATCH_QUEUEING;
    app_set_screen(app, SKIFF_APP_SCREEN_LIBRARY);
    app->call.app = app;
    app_call_start(app, CALL_BATCH_QUEUE);
}

/* ---- On the browsing thread ---- */

/* skiff_install_plan_download()'s taken hook: a target an unfinished job uses, or one planned for
 * an earlier game of this batch (two favourites whose safe names are the same). */
static int promised(void *ctx, const char *logical_path) {
    const app_call *call = ctx;
    const skiff_app *app = call->app;
    const app_batch *batch = &app->batch;
    char path[SKIFF_STORAGE_PATH_MAX];
    if (skiff_storage_resolve(&app->config.roots, logical_path, path, sizeof path) != SKIFF_OK) {
        return 1;
    }
    for (size_t i = 0; i < batch->listed_count; i++) {
        if (app_job_unfinished(&batch->listed[i]) && app_same_path(batch->listed[i].target, path)) {
            return 1;
        }
    }
    for (size_t i = 0; i < batch->request_count; i++) {
        if (app_same_path(batch->requests[i].target, path)) {
            return 1;
        }
    }
    return 0;
}

skiff_err app_batch_queue_run(app_call *call) {
    skiff_app *app = call->app;
    app_batch *batch = &app->batch;
    /* The queue first: a job that finishes after this is listed as unfinished (its target stays
     * promised), and its record is in the snapshot below. */
    batch->listed_count = skiff_jobs_list(app->jobs, batch->listed, SKIFF_JOBS_MAX);
    batch->request_count = 0;
    batch->added = 0;
    batch->plan_failures = 0;
    batch->plan_error = SKIFF_OK;
    char games[SKIFF_STORAGE_PATH_MAX];
    skiff_err err =
        skiff_storage_resolve(&app->config.roots, SKIFF_STORAGE_ROOT_GAMES, games, sizeof games);
    if (err == SKIFF_OK) {
        err = skiff_storage_mkdirs(app->config.storage, games);
    }
    /* Planned against a copy: planning checks the Memory Stick for every game, and the screen's
     * thread takes the manifest lock to draw the library. */
    skiff_install_manifest *snapshot = NULL;
    if (err == SKIFF_OK) {
        err = skiff_install_manifest_create(&snapshot);
    }
    if (err != SKIFF_OK) {
        return err;
    }
    app_lock_manifest(app);
    *snapshot = *app->manifest;
    app_unlock_manifest(app);
    /* The requests point into games[] and plans[]. */
    for (size_t i = 0; i < batch->count; i++) {
        const skiff_romm_rom_summary *rom = &batch->games[i];
        skiff_install_plan *plan = &batch->plans[i];
        const skiff_err planned =
            skiff_install_plan_download(app->installer, rom, &rom->file, &app->config.roots,
                                        snapshot, app->config.storage, promised, call, plan);
        batch->planned[i] = planned == SKIFF_OK;
        if (!batch->planned[i]) {
            if (batch->plan_failures++ == 0) {
                batch->plan_error = planned;
            }
            continue;
        }
        batch->requests[batch->request_count++] = (skiff_job_request){
            .rom_id = rom->id,
            .title = app_rom_name(rom),
            .file_name = rom->file.file_name,
            .target = plan->path,
            .size = rom->file.size,
            .has_crc32 = rom->file.has_crc32,
            .crc32 = rom->file.crc32,
            .replace_target = plan->replaces_own,
            .replace_size = plan->own_size,
        };
    }
    skiff_install_manifest_destroy(snapshot);
    return skiff_jobs_add_many(app->jobs, batch->requests, batch->request_count, batch->ids,
                               &batch->added);
}

/* ---- Back on the screen's thread ---- */

void app_batch_queued(skiff_app *app) {
    app_batch *batch = &app->batch;
    const skiff_err err = app->call.err;
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "download all favourites: queued %zu of %zu (%zu planned, first failure %s): "
                    "%s (%d)",
                    batch->added, batch->count, batch->request_count,
                    skiff_err_name(batch->plan_error), skiff_err_name(err), (int)err);
    end_batch(app);
    app_queue_refresh(app);
    if (err != SKIFF_OK) {
        app_show_error(app, err, NULL, MESSAGE_BACK, MESSAGE_NONE, SKIFF_TEXT_OK,
                       SKIFF_APP_SCREEN_LIBRARY);
        return;
    }
    char added[NUMBER_TEXT_MAX];
    snprintf(added, sizeof added, "%zu", batch->added);
    char chosen[NUMBER_TEXT_MAX];
    snprintf(chosen, sizeof chosen, "%zu", batch->count);
    const char *args[] = {added, chosen};
    char text[SKIFF_TEXT_MAX];
    if (batch->added == batch->count) {
        app_format(app, SKIFF_TEXT_BATCH_ADDED, args, 1, text);
        app_note(app, text);
        return;
    }
    /* Each reason a game was left out, so a full queue is never blamed for another failure. */
    app_format(app, SKIFF_TEXT_BATCH_ADDED_SOME, args, 2, text);
    app_show_text(app, SKIFF_TEXT_TITLE_NOTICE, text, MESSAGE_BACK, SKIFF_APP_SCREEN_LIBRARY);
    char lines[2][SKIFF_TEXT_MAX];
    char sentence[SKIFF_TEXT_MAX];
    (void)skiff_error_line(app->config.language, batch->plan_error, sentence, sizeof sentence);
    size_t used = add_line(app, lines, 2, 0, SKIFF_TEXT_BATCH_LEFT_FULL,
                           batch->request_count - batch->added, NULL);
    used =
        add_line(app, lines, 2, used, SKIFF_TEXT_BATCH_LEFT_ERROR, batch->plan_failures, sentence);
    for (size_t i = 0; i < used; i++) {
        app_message_add(app, lines[i]);
    }
}
