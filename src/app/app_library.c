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

static const char *display_name(const skiff_romm_rom_summary *rom) {
    return rom->name[0] != '\0' ? rom->name : rom->fs_name;
}

static void fit_page(const skiff_app *app, app_page *page) {
    for (size_t i = 0; i < page->page.count; i++) {
        app_fit(app, display_name(&page->page.items[i]), SKIFF_APP_LABEL_WIDTH, page->labels[i],
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

/* The page is on its way: a call for it runs, and its result will be kept. */
static int page_loading(const skiff_app *app, uint64_t index) {
    return app->call.kind == CALL_PAGE && !app->call.abandoned && app->call.page_index == index;
}

/* Asks for the first page, or the page of a row on screen that is not loaded, unless it is on its
 * way; 0 when every row is loaded. */
static int request_missing_page(skiff_app *app) {
    if (!app->has_platform) {
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
    (void)request_missing_page(app);
}

void app_library_refresh_markers(skiff_app *app) { app->dirty = 1; }

void app_page_done(skiff_app *app) {
    const app_call *call = &app->call;
    const skiff_err err = call->err;
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_ERROR,
                    SKIFF_APP_LOG_TAG, "page %llu: %zu of %llu: %s (%d)",
                    (unsigned long long)call->page_index, err == SKIFF_OK ? call->page.count : 0,
                    (unsigned long long)(err == SKIFF_OK ? call->page.total : 0),
                    skiff_err_name(err), (int)err);
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
}

void app_request_run(skiff_app *app) {
    if (app->request == REQUEST_NONE || app_call_busy(app)) {
        return;
    }
    const app_request request = app->request;
    app->request = REQUEST_NONE;
    /* A dropped request found the network gone: join again first; connecting then makes this
     * request (app_resume_after_connect()). */
    if (!app->net_joined) {
        app->failed_request = request;
        app_connect_begin(app, CONNECT_NETWORK);
        return;
    }
    /* Asked for while another page loaded: the player may have scrolled on since. */
    if (request == REQUEST_PAGE && app->total_known && !request_missing_page(app)) {
        return;
    }
    if (app->request == REQUEST_PAGE) {
        app->request = REQUEST_NONE;
    }
    if (request == REQUEST_PAGE) {
        app->call.platform_id = app->platform.id;
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

void app_library_update(skiff_app *app, unsigned actions) {
    app_request_run(app);
    if (actions & SKIFF_UI_ACTION_EXTRA) {
        app_queue_refresh(app);
        app_set_screen(app, SKIFF_APP_SCREEN_QUEUE);
        return;
    }
    if (actions & SKIFF_UI_ACTION_MENU) {
        app_set_screen(app, SKIFF_APP_SCREEN_SETTINGS);
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
        app_set_screen(app, SKIFF_APP_SCREEN_DETAILS);
        app->request = REQUEST_ROM;
    }
}

/* ---- Details and downloading ---- */

static skiff_text_id reason_text(skiff_install_support support) {
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
    return support == SKIFF_INSTALL_SUPPORTED ? SKIFF_TEXT_COUNT : reason_text(support);
}

static int same_path_ignoring_case(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        const char lower_a = (char)(*a >= 'A' && *a <= 'Z' ? *a - 'A' + 'a' : *a);
        const char lower_b = (char)(*b >= 'A' && *b <= 'Z' ? *b - 'A' + 'a' : *b);
        if (lower_a != lower_b) {
            return 0;
        }
    }
    return *a == *b;
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
        const int unfinished = job->state == SKIFF_JOB_QUEUED || job->state == SKIFF_JOB_ACTIVE ||
                               job->state == SKIFF_JOB_FAILED;
        if (unfinished && same_path_ignoring_case(job->target, path)) {
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
            .title = display_name(&app->rom.summary),
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
    const char *args[] = {display_name(&app->rom.summary)};
    char text[SKIFF_TEXT_MAX];
    app_format(app, SKIFF_TEXT_QUEUE_ADDED, args, 1, text);
    app_note(app, text);
    app_set_screen(app, SKIFF_APP_SCREEN_LIBRARY);
}

void app_download_confirmed(skiff_app *app) { queue_download(app); }

int app_installed_room(skiff_app *app, uint64_t rom_id, const char *file_name) {
    app_lock_manifest(app);
    const int recorded = skiff_install_manifest_find(app->manifest, rom_id, file_name) != NULL;
    /* Downloads already queued that will each need a record of their own count as taken. */
    size_t promised_records = 0;
    for (size_t i = 0; i < app->queue.count; i++) {
        const skiff_job *job = &app->queue.jobs[i];
        const int unfinished = job->state == SKIFF_JOB_QUEUED || job->state == SKIFF_JOB_ACTIVE ||
                               job->state == SKIFF_JOB_FAILED;
        promised_records +=
            (size_t)(unfinished && skiff_install_manifest_find(app->manifest, job->rom_id,
                                                               job->file_name) == NULL);
    }
    const int room =
        recorded || app->manifest->count + promised_records < SKIFF_INSTALL_RECORDS_MAX;
    app_unlock_manifest(app);
    return room;
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

void app_details_update(skiff_app *app, unsigned actions) {
    app_request_run(app);
    if (actions & SKIFF_UI_ACTION_BACK) {
        /* Leaving while the ROM loads: its request is dropped, or never made. */
        if (app->call.kind == CALL_ROM) {
            app_call_abandon(app);
        }
        if (app->request == REQUEST_ROM) {
            app->request = REQUEST_NONE;
        }
        app_set_screen(app, SKIFF_APP_SCREEN_LIBRARY);
        return;
    }
    if ((actions & SKIFF_UI_ACTION_CONFIRM) && app->has_rom) {
        download(app);
    }
}
