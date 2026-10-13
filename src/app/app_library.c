#include <stdio.h>
#include <string.h>

#include "app_internal.h"

/* ---- The page cache ---- */

static app_page *cached_page(skiff_app *app, uint64_t index) {
    for (size_t i = 0; i < SKIFF_APP_CACHED_PAGES; i++) {
        if (app->pages[i].valid && app->pages[i].index == index) {
            app->pages[i].used = ++app->page_clock;
            return &app->pages[i];
        }
    }
    return NULL;
}

static app_page *page_to_replace(skiff_app *app) {
    app_page *oldest = &app->pages[0];
    for (size_t i = 0; i < SKIFF_APP_CACHED_PAGES; i++) {
        if (!app->pages[i].valid) {
            return &app->pages[i];
        }
        if (app->pages[i].used < oldest->used) {
            oldest = &app->pages[i];
        }
    }
    return oldest;
}

const char *app_rom_name(const skiff_romm_rom_summary *rom) {
    return rom->name[0] != '\0' ? rom->name : rom->fs_name;
}

static void fit_page(const skiff_app *app, app_page *page) {
    for (size_t i = 0; i < page->page.count; i++) {
        app_fit(app, app_rom_name(&page->page.items[i]), SKIFF_APP_LABEL_WIDTH, page->labels[i],
                sizeof page->labels[i]);
    }
}

/* The ROM at index of the library, or NULL when its page is not loaded. */
static const skiff_romm_rom_summary *rom_at(skiff_app *app, size_t index, const char **label) {
    app_page *page = cached_page(app, index / SKIFF_ROMM_PAGE_SIZE);
    const size_t at = index % SKIFF_ROMM_PAGE_SIZE;
    if (page == NULL || at >= page->page.count) {
        return NULL;
    }
    if (label != NULL) {
        *label = page->labels[at];
    }
    return &page->page.items[at];
}

void app_current_list(const skiff_app *app, skiff_romm_list_filter *filter,
                      uint64_t *collection_id) {
    *collection_id = 0;
    switch (app->source) {
    case SOURCE_FAVOURITES:
        *filter = SKIFF_ROMM_LIST_FAVOURITES;
        return;
    case SOURCE_COLLECTIONS:
        *filter =
            app->collection.smart ? SKIFF_ROMM_LIST_SMART_COLLECTION : SKIFF_ROMM_LIST_COLLECTION;
        *collection_id = app->collection.id;
        return;
    case SOURCE_ALL:
        break;
    }
    *filter = SKIFF_ROMM_LIST_ALL;
}

int app_games_listed(const skiff_app *app) {
    return app->source != SOURCE_COLLECTIONS || app->collection_open;
}

void app_list_name(skiff_romm_list_filter filter, uint64_t collection_id, char *out, size_t size) {
    switch (filter) {
    case SKIFF_ROMM_LIST_FAVOURITES:
        snprintf(out, size, "favourites");
        return;
    case SKIFF_ROMM_LIST_COLLECTION:
    case SKIFF_ROMM_LIST_SMART_COLLECTION:
        snprintf(out, size, "%scollection %llu",
                 filter == SKIFF_ROMM_LIST_SMART_COLLECTION ? "smart " : "",
                 (unsigned long long)collection_id);
        return;
    case SKIFF_ROMM_LIST_WITH_SAVES:
        snprintf(out, size, "games with saves");
        return;
    case SKIFF_ROMM_LIST_ALL:
        break;
    }
    snprintf(out, size, "every game");
}

/* The running or finished call lists the games the library lists now. */
static int call_for_listed_games(const skiff_app *app) {
    skiff_romm_list_filter filter = SKIFF_ROMM_LIST_ALL;
    uint64_t collection_id = 0;
    app_current_list(app, &filter, &collection_id);
    return app_games_listed(app) && app->call.filter == filter &&
           app->call.collection_id == collection_id;
}

/* The page is on its way: a call for it runs, and its result will be kept. */
static int page_loading(const skiff_app *app, uint64_t index) {
    return app->call.kind == CALL_PAGE && !app->call.abandoned && app->call.page_index == index &&
           call_for_listed_games(app);
}

/* Asks for the first page, or the page of a row on screen that is not loaded, unless it is on its
 * way; 0 when every row is loaded (or the library lists no games: the collections). */
static int request_missing_page(skiff_app *app) {
    if (!app->has_platform || !app_games_listed(app)) {
        return 0;
    }
    if (!app->total_known) {
        if (!page_loading(app, 0)) {
            app->request = REQUEST_PAGE;
            app->request_page = 0;
        }
        return 1;
    }
    for (size_t i = 0; i < app->library.rows && app->library.first + i < app->library.count; i++) {
        const size_t index = app->library.first + i;
        const uint64_t page = index / SKIFF_ROMM_PAGE_SIZE;
        if (cached_page(app, page) == NULL) {
            if (!page_loading(app, page)) {
                app->request = REQUEST_PAGE;
                app->request_page = page;
                app->dirty = 1;
            }
            return 1;
        }
    }
    return 0;
}

void app_library_open(skiff_app *app) {
    app_set_screen(app, SKIFF_APP_SCREEN_LIBRARY);
    if (app->has_platform && !app_games_listed(app)) {
        app_collections_request(app);
    }
    (void)request_missing_page(app);
}

void app_library_refresh_markers(skiff_app *app) { app->dirty = 1; }

void app_page_done(skiff_app *app) {
    const app_call *call = &app->call;
    const skiff_err err = call->err;
    char list[APP_LIST_NAME_MAX];
    app_list_name(call->filter, call->collection_id, list, sizeof list);
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_ERROR,
                    SKIFF_APP_LOG_TAG, "page %llu of %s: %zu of %llu: %s (%d)",
                    (unsigned long long)call->page_index, list,
                    err == SKIFF_OK ? call->page.count : 0,
                    (unsigned long long)(err == SKIFF_OK ? call->page.total : 0),
                    skiff_err_name(err), (int)err);
    /* A page of the list the player switched away from: dropped, the other list loads next. */
    if (!call_for_listed_games(app)) {
        app_network_failed(app, err);
        return;
    }
    if (err != SKIFF_OK) {
        app_network_failed(app, err);
        app->failed_request = REQUEST_PAGE;
        app->failed_page = call->page_index;
        const int network = app_is_network_error(err);
        app_show_error(app, err, NULL, MESSAGE_RETRY_REQUEST,
                       network ? MESSAGE_PICK_NETWORK : MESSAGE_SETTINGS,
                       network ? SKIFF_TEXT_CHOOSE_NETWORK : SKIFF_TEXT_SETTINGS,
                       SKIFF_APP_SCREEN_LIBRARY);
        return;
    }
    app_page *page = page_to_replace(app);
    memset(page, 0, sizeof *page);
    page->page = call->page;
    page->valid = 1;
    page->index = call->page_index;
    page->used = ++app->page_clock;
    fit_page(app, page);
    /* The library may have grown or shrunk since the first page. */
    app->total = page->page.total;
    if (!app->total_known) {
        app->total_known = 1;
        skiff_ui_list_init(&app->library, (size_t)app->total, APP_LIBRARY_ROWS);
    } else {
        skiff_ui_list_set_count(&app->library, (size_t)app->total);
    }
}

void app_rom_done(skiff_app *app) {
    /* The call's copy: a failed request empties it, and a retry still needs the ROM's id. */
    const skiff_romm_rom *fetched = &app->call.rom;
    const skiff_err err = app->call.err;
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_ERROR,
                    SKIFF_APP_LOG_TAG, "ROM %llu: %zu file(s): %s (%d)",
                    (unsigned long long)app->call.rom_id, err == SKIFF_OK ? fetched->file_count : 0,
                    skiff_err_name(err), (int)err);
    if (err != SKIFF_OK) {
        app->has_rom = 0;
        app_network_failed(app, err);
        app->failed_request = REQUEST_ROM;
        app_show_error(app, err, NULL, MESSAGE_RETRY_REQUEST, MESSAGE_BACK, SKIFF_TEXT_BACK,
                       SKIFF_APP_SCREEN_LIBRARY);
        return;
    }
    app->rom = *fetched;
    app->has_rom = 1;
    app_details_forget_description(app);
    app_saves_rom_loaded(app);
    /* The cover after the details: they decide whether the game downloads, it is decoration. */
    if (app->rom.cover_path[0] != '\0' && !app_cover_shown(app)) {
        app->request = REQUEST_COVER;
    }
}

/* The key of the details screen's ROM's cover, from the server it is browsed on. */
static skiff_cover_key current_cover_key(const skiff_app *app) {
    return skiff_cover_key_of(app->romm.base_url, app->rom.summary.id, app->rom.cover_path);
}

static int same_cover(const skiff_cover_key *a, const skiff_cover_key *b) {
    return a->rom_id == b->rom_id && a->server == b->server && a->cover_path == b->cover_path;
}

int app_cover_shown(const skiff_app *app) {
    if (!app->has_cover || app->rom.cover_path[0] == '\0') {
        return 0;
    }
    const skiff_cover_key key = current_cover_key(app);
    return same_cover(&key, &app->cover_key);
}

/* Starts the cover call for the ROM on the details screen, its inputs and results reset. */
static void start_cover(skiff_app *app) {
    app_call *call = &app->call;
    call->rom_id = app->rom.summary.id;
    snprintf(call->cover_path, sizeof call->cover_path, "%s", app->rom.cover_path);
    call->cover_key = current_cover_key(app);
    call->storage = app->config.storage;
    call->roots = app->config.roots;
    call->now_ms = app->env.now_ms;
    call->clock_ctx = app->env.ctx;
    call->cover = app->cover_spare;
    call->cover_cached = 0;
    call->cover_bytes = 0;
    call->cover_fetch_ms = 0;
    call->cover_decode_ms = 0;
    call->cover_cache_err = SKIFF_OK;
    call->cover_store_err = SKIFF_OK;
    app_call_start(app, CALL_COVER);
}

void app_cover_done(skiff_app *app) {
    const app_call *call = &app->call;
    const skiff_err err = call->err;
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG,
                    "cover: ROM %llu %s, %zu bytes, %ux%u, fetch %lld ms, decode %lld ms, cache "
                    "%s, store %s: %s (%d)",
                    (unsigned long long)call->rom_id, call->cover_cached ? "cached" : "from RomM",
                    call->cover_bytes, err == SKIFF_OK ? call->cover->width : 0U,
                    err == SKIFF_OK ? call->cover->height : 0U, (long long)call->cover_fetch_ms,
                    (long long)call->cover_decode_ms, skiff_err_name(call->cover_cache_err),
                    skiff_err_name(call->cover_store_err), skiff_err_name(err), (int)err);
    if (err != SKIFF_OK) {
        /* Shown without its cover; a lost network is joined again by the next request. */
        app_network_failed(app, err);
        return;
    }
    /* Kept while a confirmation or message covers the details screen, which it returns to; leaving
     * the details abandons the call, so only another cover of the ROM can come back here. */
    const skiff_cover_key key = current_cover_key(app);
    if (!same_cover(&call->cover_key, &key)) {
        return;
    }
    skiff_cover *shown = app->cover;
    app->cover = app->cover_spare;
    app->cover_spare = shown;
    app->cover_key = call->cover_key;
    app->has_cover = 1;
}

void app_request_run(skiff_app *app) {
    if (app->request == REQUEST_NONE || app_call_busy(app)) {
        return;
    }
    const app_request request = app->request;
    app->request = REQUEST_NONE;
    /* A cover is never worth joining the Wi-Fi again for: the game shows without it. */
    if (request == REQUEST_COVER) {
        if (app->net_joined && app->screen == SKIFF_APP_SCREEN_DETAILS) {
            start_cover(app);
        }
        return;
    }
    /* A dropped request found the network gone: join again first; connecting then makes this
     * request (app_resume_after_connect()). */
    if (!app->net_joined) {
        app->failed_request = request;
        app_connect_begin(app, CONNECT_NETWORK);
        return;
    }
    if (request == REQUEST_COLLECTIONS) {
        app_collections_run(app);
        return;
    }
    /* Asked for while another page loaded: the player may have scrolled on since, or left the
     * games for the collections. */
    if (request == REQUEST_PAGE &&
        (!app_games_listed(app) || (app->total_known && !request_missing_page(app)))) {
        return;
    }
    if (app->request == REQUEST_PAGE) {
        app->request = REQUEST_NONE;
    }
    if (request == REQUEST_PAGE) {
        app->call.platform_id = app->platform.id;
        app_current_list(app, &app->call.filter, &app->call.collection_id);
        app->call.with_files = 0;
        app->call.page_index = app->request_page;
        app_call_start(app, CALL_PAGE);
    } else {
        app->call.rom_id = app->rom.summary.id;
        app_call_start(app, CALL_ROM);
    }
}

/* ---- What a ROM shows ---- */

/* The file a PSP game downloads: its only one. NULL when RomM lists none or several. */
static const skiff_romm_file *only_file(const skiff_romm_rom *rom) {
    return rom->file_count == 1 && rom->stored_count == 1 ? &rom->files[0] : NULL;
}

/* A list item stands for its one file by its fs_name. */
static skiff_install_support support_of(const skiff_app *app, const skiff_romm_rom_summary *rom,
                                        const skiff_romm_file *file) {
    skiff_romm_file listed;
    if (file == NULL) {
        memset(&listed, 0, sizeof listed);
        snprintf(listed.file_name, sizeof listed.file_name, "%s", rom->fs_name);
        listed.name_status = rom->name_status;
        file = &listed;
    }
    return skiff_install_check(app->installer, rom, file);
}

int app_rom_downloadable(const skiff_app *app, const skiff_romm_rom_summary *rom) {
    return support_of(app, rom, NULL) == SKIFF_INSTALL_SUPPORTED;
}

void app_rom_detail(skiff_app *app, const skiff_romm_rom_summary *rom, char *out, size_t size) {
    out[0] = '\0';
    const skiff_job *job = app_queue_job_for(app, rom->id);
    if (job != NULL) {
        const skiff_text_id id = job->state == SKIFF_JOB_ACTIVE   ? SKIFF_TEXT_QUEUE_DOWNLOADING
                                 : job->state == SKIFF_JOB_FAILED ? SKIFF_TEXT_QUEUE_FAILED
                                                                  : SKIFF_TEXT_QUEUE_WAITING;
        char code[16];
        snprintf(code, sizeof code, "%d", (int)job->error);
        const char *args[] = {code};
        char text[SKIFF_TEXT_MAX];
        app_format(app, id, args, 1, text);
        app_fit(app, text, SKIFF_APP_DETAIL_WIDTH, out, size);
        return;
    }
    app_lock_manifest(app);
    const skiff_install_state state = skiff_install_state_of(app->manifest, rom);
    app_unlock_manifest(app);
    if (state == SKIFF_INSTALL_NOT_INSTALLED) {
        return;
    }
    app_fit(app,
            app_text(app, state == SKIFF_INSTALL_INSTALLED ? SKIFF_TEXT_INSTALLED
                                                           : SKIFF_TEXT_INSTALL_CHANGED),
            SKIFF_APP_DETAIL_WIDTH, out, size);
}

/* ---- The library screen ---- */

/* The games listed go: the next list starts from its first page, at its top. A page still loading
 * for the list left behind finishes and is dropped (app_page_done()), rather than cancelled with
 * its connection. */
static void forget_games(skiff_app *app) {
    memset(app->pages, 0, sizeof app->pages);
    app->total = 0;
    app->total_known = 0;
    skiff_ui_list_init(&app->library, 0, APP_LIBRARY_ROWS);
    if (app->request == REQUEST_PAGE || app->request == REQUEST_COLLECTIONS) {
        app->request = REQUEST_NONE;
    }
    if (app->failed_request == REQUEST_PAGE || app->failed_request == REQUEST_COLLECTIONS) {
        app->failed_request = REQUEST_NONE;
    }
    app->dirty = 1;
}

static void log_list(skiff_app *app) {
    char list[APP_LIST_NAME_MAX];
    skiff_romm_list_filter filter = SKIFF_ROMM_LIST_ALL;
    uint64_t collection_id = 0;
    app_current_list(app, &filter, &collection_id);
    app_list_name(filter, collection_id, list, sizeof list);
    skiff_log_write(app->log, SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG, "library: %s",
                    app_games_listed(app) ? list : "collections");
}

/* SELECT, or Back from the collections: the library lists source, from its start. */
static void show_source(skiff_app *app, app_source source) {
    app->source = source;
    app->collection_open = 0;
    forget_games(app);
    if (source == SOURCE_COLLECTIONS) {
        app_collections_enter(app);
    }
    log_list(app);
    (void)request_missing_page(app);
}

static app_source next_source(app_source source) {
    switch (source) {
    case SOURCE_ALL:
        return SOURCE_FAVOURITES;
    case SOURCE_FAVOURITES:
        return SOURCE_COLLECTIONS;
    case SOURCE_COLLECTIONS:
        break;
    }
    return SOURCE_ALL;
}

/* The collection selected in the collections list: its games, from their first page. */
static void open_collection(skiff_app *app) {
    app->collection = app->collections.items[app->collections_list.selected];
    app->collection_open = 1;
    forget_games(app);
    log_list(app);
    (void)request_missing_page(app);
}

/* Back from a collection's games: the collections list, at the row it was opened from. */
static void close_collection(skiff_app *app) {
    app->collection_open = 0;
    forget_games(app);
    log_list(app);
}

int app_batch_offered(const skiff_app *app) {
    return app->source != SOURCE_ALL && app_games_listed(app) && app->total_known && app->total > 0;
}

void app_library_update(skiff_app *app, unsigned actions) {
    app_request_run(app);
    /* While the favourites are checked for "Download all", only Back (which stops it) counts;
     * while they are queued (a moment), nothing does: the batch's room in the queue and
     * installed.json must not be taken meanwhile. */
    if (app->batch.step == BATCH_SCANNING || app->batch.step == BATCH_SPACE ||
        app->batch.step == BATCH_QUEUEING) {
        if (actions & SKIFF_UI_ACTION_BACK) {
            app_batch_cancel(app);
        }
        return;
    }
    if ((actions & SKIFF_UI_ACTION_START) && app_batch_offered(app)) {
        app_batch_start(app);
        return;
    }
    if ((actions & SKIFF_UI_ACTION_SELECT) && app->has_platform) {
        show_source(app, next_source(app->source));
        return;
    }
    /* Back walks out of the collections: from a collection's games to the list, from the list on
     * to every game, as SELECT does. */
    if ((actions & SKIFF_UI_ACTION_BACK) && app->source == SOURCE_COLLECTIONS) {
        if (app->collection_open) {
            close_collection(app);
        } else {
            show_source(app, SOURCE_ALL);
        }
        return;
    }
    if (actions & SKIFF_UI_ACTION_EXTRA) {
        app_queue_refresh(app);
        app_set_screen(app, SKIFF_APP_SCREEN_QUEUE);
        return;
    }
    if (actions & SKIFF_UI_ACTION_MENU) {
        app_set_screen(app, SKIFF_APP_SCREEN_SETTINGS);
        return;
    }
    if (!app_games_listed(app)) {
        if (app_collections_update(app, actions)) {
            open_collection(app);
        }
        return;
    }
    if (skiff_ui_list_apply(&app->library, actions)) {
        app->dirty = 1;
    }
    /* The selected row's page, and the rest of the screen's, load as they come into view. */
    if (request_missing_page(app)) {
        return;
    }
    if ((actions & SKIFF_UI_ACTION_CONFIRM) && app->library.count > 0) {
        const skiff_romm_rom_summary *rom = rom_at(app, app->library.selected, NULL);
        if (rom == NULL) {
            return;
        }
        memset(&app->rom, 0, sizeof app->rom);
        app->rom.summary = *rom;
        app->has_rom = 0;
        app_details_forget_description(app);
        app_set_screen(app, SKIFF_APP_SCREEN_DETAILS);
        app->request = REQUEST_ROM;
    }
}

/* ---- Details and downloading ---- */

skiff_text_id app_install_support_text(skiff_install_support support) {
    switch (support) {
    case SKIFF_INSTALL_MULTIPLE_FILES:
        return SKIFF_TEXT_INSTALL_MULTIPLE_FILES;
    case SKIFF_INSTALL_UNUSABLE_NAME:
        return SKIFF_TEXT_NAME_UNSUPPORTED;
    case SKIFF_INSTALL_UNKNOWN_EXTENSION:
    case SKIFF_INSTALL_SUPPORTED:
        break;
    }
    return SKIFF_TEXT_INSTALL_UNKNOWN_EXTENSION;
}

skiff_text_id app_details_refusal(const skiff_app *app) {
    if (app->rom.file_count > 1) {
        return SKIFF_TEXT_INSTALL_MULTIPLE_FILES;
    }
    const skiff_romm_file *file = only_file(&app->rom);
    if (file == NULL) {
        return SKIFF_TEXT_INSTALL_UNKNOWN_EXTENSION;
    }
    const skiff_install_support support = support_of(app, &app->rom.summary, file);
    return support == SKIFF_INSTALL_SUPPORTED ? SKIFF_TEXT_COUNT
                                              : app_install_support_text(support);
}

/* A logical path a queued download is promised: skiff_install_plan_download()'s taken hook. */
static int promised(void *ctx, const char *logical_path) {
    const skiff_app *app = ctx;
    char path[SKIFF_STORAGE_PATH_MAX];
    if (skiff_storage_resolve(&app->config.roots, logical_path, path, sizeof path) != SKIFF_OK) {
        return 1;
    }
    for (size_t i = 0; i < app->queue.count; i++) {
        const skiff_job *job = &app->queue.jobs[i];
        if (app_job_unfinished(job) && app_same_path(job->target, path)) {
            return 1;
        }
    }
    return 0;
}

static void queue_download(skiff_app *app) {
    const skiff_romm_file *file = only_file(&app->rom);
    skiff_install_plan plan;
    app_lock_manifest(app);
    skiff_err err =
        skiff_install_plan_download(app->installer, &app->rom.summary, file, &app->config.roots,
                                    app->manifest, app->config.storage, promised, app, &plan);
    app_unlock_manifest(app);
    char games[SKIFF_STORAGE_PATH_MAX];
    if (err == SKIFF_OK) {
        err = skiff_storage_resolve(&app->config.roots, SKIFF_STORAGE_ROOT_GAMES, games,
                                    sizeof games);
    }
    if (err == SKIFF_OK) {
        err = skiff_storage_mkdirs(app->config.storage, games);
    }
    uint32_t id = 0;
    if (err == SKIFF_OK) {
        const skiff_job_request request = {
            .rom_id = app->rom.summary.id,
            .title = app_rom_name(&app->rom.summary),
            .file_name = file->file_name,
            .target = plan.path,
            .size = file->size,
            .has_crc32 = file->has_crc32,
            .crc32 = file->crc32,
            .replace_target = plan.replaces_own,
            .replace_size = plan.own_size,
        };
        err = skiff_jobs_add(app->jobs, &request, &id);
    }
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "queue ROM %llu as job %u to %s (renamed %d, replaces %d): %s (%d)",
                    (unsigned long long)app->rom.summary.id, (unsigned)id, plan.logical_path,
                    plan.renamed, plan.replaces_own, skiff_err_name(err), (int)err);
    if (err != SKIFF_OK) {
        app_show_error(app, err, NULL, MESSAGE_BACK, MESSAGE_NONE, SKIFF_TEXT_OK,
                       SKIFF_APP_SCREEN_DETAILS);
        return;
    }
    app_queue_refresh(app);
    app_note_named(app, SKIFF_TEXT_QUEUE_ADDED, app_rom_name(&app->rom.summary));
    app_set_screen(app, SKIFF_APP_SCREEN_LIBRARY);
}

void app_download_confirmed(skiff_app *app) { queue_download(app); }

/* Records installed.json holds, plus one for each queued download that will need its own. Under
 * the manifest lock. */
static size_t records_taken(const skiff_app *app) {
    size_t taken = app->manifest->count;
    for (size_t i = 0; i < app->queue.count; i++) {
        const skiff_job *job = &app->queue.jobs[i];
        taken += (size_t)(app_job_unfinished(job) &&
                          skiff_install_manifest_find(app->manifest, job->rom_id, job->file_name) ==
                              NULL);
    }
    return taken;
}

int app_installed_room(skiff_app *app, uint64_t rom_id, const char *file_name) {
    app_lock_manifest(app);
    const int recorded = skiff_install_manifest_find(app->manifest, rom_id, file_name) != NULL;
    const int room = recorded || records_taken(app) < SKIFF_INSTALL_RECORDS_MAX;
    app_unlock_manifest(app);
    return room;
}

size_t app_installed_records_free(skiff_app *app) {
    app_lock_manifest(app);
    const size_t taken = records_taken(app);
    app_unlock_manifest(app);
    return taken < SKIFF_INSTALL_RECORDS_MAX ? SKIFF_INSTALL_RECORDS_MAX - taken : 0;
}

static void download(skiff_app *app) {
    if (app_details_refusal(app) != SKIFF_TEXT_COUNT) {
        return;
    }
    if (app_queue_job_for(app, app->rom.summary.id) != NULL) {
        app_queue_refresh(app);
        app_set_screen(app, SKIFF_APP_SCREEN_QUEUE);
        return;
    }
    const skiff_err worker = app_start_worker(app);
    if (worker != SKIFF_OK) {
        app_show_error(app, worker, NULL, MESSAGE_RETRY_WORKER, MESSAGE_BACK, SKIFF_TEXT_BACK,
                       SKIFF_APP_SCREEN_DETAILS);
        return;
    }
    if (app_queue_unfinished(app) >= SKIFF_JOBS_MAX) {
        app_show_text(app, SKIFF_TEXT_TITLE_NOTICE, app_text(app, SKIFF_TEXT_QUEUE_FULL),
                      MESSAGE_BACK, SKIFF_APP_SCREEN_DETAILS);
        return;
    }
    const skiff_romm_file *file = only_file(&app->rom);
    const int full = !app_installed_room(app, app->rom.summary.id, file->file_name);
    app_lock_manifest(app);
    const skiff_install_state state = skiff_install_state_of(app->manifest, &app->rom.summary);
    app_unlock_manifest(app);
    if (full) {
        app_show_text(app, SKIFF_TEXT_TITLE_NOTICE, app_text(app, SKIFF_TEXT_INSTALLED_FULL),
                      MESSAGE_BACK, SKIFF_APP_SCREEN_DETAILS);
        return;
    }
    if (state != SKIFF_INSTALL_NOT_INSTALLED) {
        app_confirm(app,
                    state == SKIFF_INSTALL_INSTALLED ? SKIFF_TEXT_CONFIRM_REPLACE
                                                     : SKIFF_TEXT_CONFIRM_REPLACE_CHANGED,
                    CONFIRM_REPLACE, 0);
        return;
    }
    queue_download(app);
}

void app_details_forget_description(skiff_app *app) {
    memset(&app->description, 0, sizeof app->description);
}

/* Up and Down move the description a line, L and R a window, stopping at its ends; only while it
 * is longer than the room the last view gave it. */
static void scroll_description(skiff_app *app, unsigned actions) {
    app_description *description = &app->description;
    const size_t window = description->window;
    if (window == 0 || description->line_count <= window) {
        return;
    }
    const size_t last_first = description->line_count - window;
    size_t first = description->first < last_first ? description->first : last_first;
    if (actions & SKIFF_UI_ACTION_UP) {
        first = first > 0 ? first - 1 : 0;
    }
    if (actions & SKIFF_UI_ACTION_DOWN) {
        first = first < last_first ? first + 1 : last_first;
    }
    if (actions & SKIFF_UI_ACTION_PAGE_UP) {
        first = first > window ? first - window : 0;
    }
    if (actions & SKIFF_UI_ACTION_PAGE_DOWN) {
        first = last_first - first > window ? first + window : last_first;
    }
    if (first != description->first) {
        description->first = first;
        app->dirty = 1;
    }
}

void app_details_update(skiff_app *app, unsigned actions) {
    app_request_run(app);
    scroll_description(app, actions);
    if (actions & SKIFF_UI_ACTION_BACK) {
        /* Leaving while the ROM or its cover loads: the request is dropped, or never made. */
        if (app->call.kind == CALL_ROM || app->call.kind == CALL_COVER) {
            app_call_abandon(app);
        }
        if (app->request == REQUEST_ROM || app->request == REQUEST_COVER) {
            app->request = REQUEST_NONE;
        }
        app_set_screen(app, SKIFF_APP_SCREEN_LIBRARY);
        return;
    }
    /* Not while games sent from RomM are queued (well under a second): their room is counted. */
    if ((actions & SKIFF_UI_ACTION_CONFIRM) && app->has_rom && !app_sends_queueing(app)) {
        download(app);
        return;
    }
    if (actions & SKIFF_UI_ACTION_EXTRA) {
        app_saves_details_sync(app);
    }
}
