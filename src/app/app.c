#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/version.h"

#include "app_internal.h"

/* ---- Small helpers ---- */

int64_t app_now(const skiff_app *app) { return app->env.now_ms(app->env.ctx); }

static int has_locks(const skiff_app *app) { return app->config.lock != NULL; }

void app_lock_manifest(skiff_app *app) {
    if (has_locks(app)) {
        app->config.lock(app->config.manifest_lock);
    }
}

void app_unlock_manifest(skiff_app *app) {
    if (has_locks(app)) {
        app->config.unlock(app->config.manifest_lock);
    }
}

void app_set_screen(skiff_app *app, skiff_app_screen screen) {
    app->screen = screen;
    app->announced = 0;
    app->dirty = 1;
    if (screen == SKIFF_APP_SCREEN_DETAILS || screen == SKIFF_APP_SCREEN_SETTINGS) {
        app->free_known = skiff_storage_free_space(app->config.storage, app->config.roots.app,
                                                   &app->free_bytes) == SKIFF_OK;
    }
}

void app_note(skiff_app *app, const char *text) {
    snprintf(app->note, sizeof app->note, "%s", text);
    app->note_until_ms = app_now(app) + SKIFF_APP_NOTE_MS;
    app->dirty = 1;
}

static size_t wrap_into(skiff_app *app, const char *text) {
    app_message *message = &app->message;
    const size_t room = SKIFF_APP_LINES_MAX - message->line_count;
    const size_t added = skiff_app_wrap(text, SKIFF_APP_TEXT_WIDTH, app->env.measure, app->env.ctx,
                                        &message->lines[message->line_count], room);
    message->line_count += added;
    return added;
}

static void message_begin(skiff_app *app, skiff_text_id title, app_message_action ok,
                          app_message_action other, skiff_text_id other_label,
                          skiff_app_screen back) {
    memset(&app->message, 0, sizeof app->message);
    app->message.title = title;
    app->message.ok = ok;
    app->message.other = other;
    app->message.other_label = other_label;
    app->message.back = back;
}

void app_show_text(skiff_app *app, skiff_text_id title, const char *text, app_message_action ok,
                   skiff_app_screen back) {
    message_begin(app, title, ok, MESSAGE_NONE, SKIFF_TEXT_OK, back);
    wrap_into(app, text);
    app_set_screen(app, SKIFF_APP_SCREEN_MESSAGE);
}

void app_show_error(skiff_app *app, skiff_err err, const skiff_config_issue *issue,
                    app_message_action ok, app_message_action other, skiff_text_id other_label,
                    skiff_app_screen back) {
    message_begin(app, SKIFF_TEXT_TITLE_ERROR, ok, other, other_label, back);
    char line[SKIFF_TEXT_MAX];
    (void)skiff_error_line(app->config.language, err, line, sizeof line);
    wrap_into(app, line);
    if (issue != NULL && issue->key[0] != '\0') {
        char number[16];
        snprintf(number, sizeof number, "%d", issue->line);
        if (issue->line > 0) {
            const char *args[] = {number, issue->section, issue->key};
            app_format(app, SKIFF_TEXT_CONFIG_ISSUE, args, 3, line);
        } else {
            const char *args[] = {issue->section, issue->key};
            app_format(app, SKIFF_TEXT_CONFIG_MISSING, args, 2, line);
        }
        wrap_into(app, line);
    }
    skiff_log_write(app->log, SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG, "shown: %s (%d)",
                    skiff_err_name(err), (int)err);
    app_set_screen(app, SKIFF_APP_SCREEN_MESSAGE);
}

void app_confirm(skiff_app *app, skiff_text_id question, app_confirm_action action, uint32_t job) {
    app->confirm = action;
    app->confirm_text = question;
    app->confirm_job = job;
    app_set_screen(app, SKIFF_APP_SCREEN_CONFIRM);
}

/* ---- config.ini ---- */

static skiff_err config_path(const skiff_app *app, char *out, size_t size) {
    return skiff_storage_resolve(&app->config.roots, APP_CONFIG_FILE, out, size);
}

static void register_secrets(skiff_app *app) {
    if (strlen(app->settings.token) >= SKIFF_LOG_SECRET_MIN) {
        (void)skiff_log_add_secret(app->log, app->settings.token);
    }
    /* A shorter value cannot be found reliably; registering it would withhold every line. */
    for (size_t i = 0; i < app->settings.header_count; i++) {
        if (strlen(app->settings.headers[i].value) >= SKIFF_LOG_SECRET_MIN) {
            (void)skiff_log_add_secret(app->log, app->settings.headers[i].value);
        }
    }
}

skiff_err app_config_replace(skiff_app *app, const char *text, size_t length) {
    skiff_config parsed;
    skiff_err err = skiff_config_parse(text, length, &parsed, NULL);
    char path[SKIFF_STORAGE_PATH_MAX];
    if (err == SKIFF_OK) {
        err = config_path(app, path, sizeof path);
    }
    if (err == SKIFF_OK) {
        err = skiff_config_save(app->config.storage, path, text, length);
    }
    if (err != SKIFF_OK) {
        skiff_log_write(app->log, SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                        "config.ini not saved: %s (%d)", skiff_err_name(err), (int)err);
        return err;
    }
    if (text != app->config_text) {
        memmove(app->config_text, text, length);
    }
    app->config_text[length] = '\0';
    app->config_length = length;
    app->settings = parsed;
    register_secrets(app);
    return SKIFF_OK;
}

skiff_err app_config_set(skiff_app *app, const char *section, const char *key, const char *value,
                         skiff_config_issue *issue) {
    size_t length = 0;
    const skiff_err err =
        skiff_config_set(app->config_text, app->config_length, section, key, value, app->edit_text,
                         sizeof app->edit_text, &length, issue);
    if (err != SKIFF_OK) {
        return err;
    }
    return app_config_replace(app, app->edit_text, length);
}

/* ---- Startup ---- */

static skiff_err start_log(skiff_app *app) {
    char path[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = skiff_storage_resolve(&app->config.roots, APP_LOG_FILE, path, sizeof path);
    if (err != SKIFF_OK) {
        return err;
    }
    const skiff_log_config log_config = {
        .storage = app->config.storage,
        .path = path,
        .level = app->settings.log_level,
        .clock = app->config.clock,
        .clock_ctx = app->config.clock_ctx,
        .lock = app->config.lock,
        .unlock = app->config.unlock,
        .lock_ctx = app->config.log_lock,
    };
    return skiff_log_create(&log_config, &app->log);
}

/*
 * Records a finished install and saves installed.json. The UI reads the manifest under its lock
 * while it draws, so the lock is held only to change it and copy it; the copy is saved without it
 * (only this thread saves while the worker runs). Without memory for the copy the record stays in
 * memory and the next save writes it.
 */
static skiff_err record_and_save(skiff_app *app, const skiff_install_record *record,
                                 const char *path) {
    skiff_install_manifest *snapshot = NULL;
    const skiff_err copy_err = skiff_install_manifest_create(&snapshot);
    app_lock_manifest(app);
    skiff_err err = skiff_install_manifest_record(app->manifest, record);
    if (err == SKIFF_OK && copy_err == SKIFF_OK) {
        *snapshot = *app->manifest;
    }
    app_unlock_manifest(app);
    if (err == SKIFF_OK) {
        err = copy_err;
    }
    if (err == SKIFF_OK) {
        err = skiff_install_manifest_save(snapshot, app->config.storage, path);
    }
    if (err == SKIFF_OK) {
        /* A damaged file set aside by this save is not set aside again. */
        app_lock_manifest(app);
        app->manifest->set_aside_damaged = snapshot->set_aside_damaged;
        app_unlock_manifest(app);
    }
    skiff_install_manifest_destroy(snapshot);
    return err;
}

static void on_downloaded(void *ctx, const skiff_job *job) {
    skiff_app *app = ctx;
    skiff_install_record record;
    memset(&record, 0, sizeof record);
    skiff_err err = skiff_storage_logical_path(&app->config.roots, job->target, record.path,
                                               sizeof record.path);
    char manifest_path[SKIFF_STORAGE_PATH_MAX];
    if (err == SKIFF_OK) {
        err = skiff_storage_resolve(&app->config.roots, APP_MANIFEST_FILE, manifest_path,
                                    sizeof manifest_path);
    }
    if (err == SKIFF_OK) {
        record.rom_id = job->rom_id;
        snprintf(record.file_name, sizeof record.file_name, "%s", job->file_name);
        record.size = job->size;
        record.has_crc32 = job->has_crc32;
        record.crc32 = job->crc32;
        if (app->config.clock == NULL ||
            app->config.clock(app->config.clock_ctx, &record.installed_ms) == 0) {
            record.installed_ms = 0;
        }
        err = record_and_save(app, &record, manifest_path);
    }
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG,
                    "job %u installed as %s: %s (%d)", (unsigned)job->id, record.path,
                    skiff_err_name(err), (int)err);
}

static skiff_err start_queue(skiff_app *app) {
    char path[SKIFF_STORAGE_PATH_MAX];
    const skiff_err err =
        skiff_storage_resolve(&app->config.roots, APP_QUEUE_FILE, path, sizeof path);
    if (err != SKIFF_OK) {
        return err;
    }
    const skiff_jobs_config jobs_config = {
        .storage = app->config.storage,
        .path = path,
        .log = app->log,
        .lock = app->config.lock,
        .unlock = app->config.unlock,
        .lock_ctx = app->config.jobs_lock,
        .save_lock_ctx = app->config.jobs_save_lock,
        .downloaded = on_downloaded,
        .downloaded_ctx = app,
    };
    return skiff_jobs_create(&jobs_config, &app->jobs);
}

static void start_manifest(skiff_app *app) {
    char path[SKIFF_STORAGE_PATH_MAX];
    skiff_install_load_result result = SKIFF_INSTALL_NO_MANIFEST;
    skiff_err err = skiff_storage_resolve(&app->config.roots, APP_MANIFEST_FILE, path, sizeof path);
    if (err == SKIFF_OK) {
        err = skiff_install_manifest_load(app->manifest, app->config.storage, path, &result);
    }
    size_t forgotten = 0;
    if (err == SKIFF_OK) {
        err = skiff_install_manifest_reconcile(app->manifest, app->config.storage,
                                               &app->config.roots, &forgotten);
    }
    if (err == SKIFF_OK && forgotten > 0) {
        err = skiff_install_manifest_save(app->manifest, app->config.storage, path);
    }
    if (result == SKIFF_INSTALL_DAMAGED) {
        skiff_log_write(app->log, SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG,
                        "installed.json could not be read; it is kept as %s%s",
                        SKIFF_INSTALL_MANIFEST_NAME, SKIFF_INSTALL_DAMAGED_SUFFIX);
    }
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG,
                    "installed.json: %zu game(s), %zu gone: %s (%d)", app->manifest->count,
                    forgotten, skiff_err_name(err), (int)err);
}

static void start(skiff_app *app) {
    char path[SKIFF_STORAGE_PATH_MAX];
    skiff_config_issue issue;
    memset(&issue, 0, sizeof issue);
    skiff_err config_err = config_path(app, path, sizeof path);
    if (config_err == SKIFF_OK) {
        config_err = skiff_config_load(app->config.storage, path, app->config_text,
                                       sizeof app->config_text, &app->config_length);
    }
    if (config_err == SKIFF_OK) {
        config_err =
            skiff_config_parse(app->config_text, app->config_length, &app->settings, &issue);
    }
    if (config_err != SKIFF_OK) {
        /* Parsing leaves the defaults, so the log still starts at its usual level. */
        (void)skiff_config_parse("", 0, &app->settings, NULL);
    }
    skiff_err err = start_log(app);
    if (err != SKIFF_OK) {
        app_show_error(app, err, NULL, MESSAGE_QUIT, MESSAGE_NONE, SKIFF_TEXT_OK,
                       SKIFF_APP_SCREEN_STARTING);
        return;
    }
    skiff_log_write(app->log, SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG, "Skiff %s, language %d, log %s",
                    skiff_version_string(), (int)app->config.language,
                    skiff_log_level_name(app->settings.log_level));
    if (config_err != SKIFF_OK) {
        skiff_log_write(app->log, SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                        "config.ini line %d [%s] %s: %s (%d)", issue.line, issue.section, issue.key,
                        skiff_err_name(config_err), (int)config_err);
        app_show_error(app, config_err, &issue, MESSAGE_QUIT, MESSAGE_NONE, SKIFF_TEXT_OK,
                       SKIFF_APP_SCREEN_STARTING);
        return;
    }
    if (app->settings.unknown_count > 0) {
        skiff_log_write(app->log, SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG,
                        "config.ini: %d unknown key(s), the first line %d [%s] %s",
                        app->settings.unknown_count, app->settings.first_unknown.line,
                        app->settings.first_unknown.section, app->settings.first_unknown.key);
    }
    register_secrets(app);
    app->profile = app->settings.network_profile;
    err = start_queue(app);
    if (err == SKIFF_OK) {
        err = skiff_install_manifest_create(&app->manifest);
    }
    if (err != SKIFF_OK) {
        app_show_error(app, err, NULL, MESSAGE_QUIT, MESSAGE_NONE, SKIFF_TEXT_OK,
                       SKIFF_APP_SCREEN_STARTING);
        return;
    }
    start_manifest(app);
    app_queue_refresh(app);
    if (app->settings.server_url[0] == '\0') {
        app_set_screen(app, SKIFF_APP_SCREEN_SERVER);
        return;
    }
    app_connect_begin(app, CONNECT_NETWORK);
}

/* ---- The worker ---- */

/* The worker's client only builds download URLs and the Authorization header (skiff/jobs.h): it
 * never sends a request. It gets this transport, which refuses one and lives for the program,
 * rather than borrowing the browse transport, which a lost connection replaces. */
static skiff_err refuse_request(skiff_transport *transport, const skiff_http_request *request,
                                skiff_http_response *response) {
    (void)transport;
    (void)request;
    (void)response;
    return SKIFF_ERR_NOT_IMPLEMENTED;
}

static void keep_transport(skiff_transport *transport) { (void)transport; }

static const skiff_transport_ops NO_REQUEST_OPS = {refuse_request, keep_transport};
static skiff_transport no_requests = {&NO_REQUEST_OPS};

skiff_err app_start_worker(skiff_app *app) {
    if (app->worker_running) {
        return SKIFF_OK;
    }
    if (app->jobs == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    app->worker_settings = app->settings;
    skiff_err err = skiff_app_transport_settings_from(&app->worker_settings, &app->config.roots,
                                                      &app->worker_transport);
    if (err == SKIFF_OK) {
        err = skiff_romm_client_init(&app->worker_romm, &no_requests,
                                     app->worker_settings.server_url, app->worker_settings.token);
    }
    if (err == SKIFF_OK) {
        const skiff_app_worker_spec spec = {app->jobs, &app->worker_romm, &app->worker_transport,
                                            app->profile, app->log};
        err = app->env.start_worker(app->env.ctx, &spec);
    }
    app->worker_running = err == SKIFF_OK;
    app->worker_profile = app->profile;
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "worker start: %s (%d)", skiff_err_name(err), (int)err);
    return err;
}

int app_stop_worker(skiff_app *app) {
    if (!app->worker_running) {
        return 1;
    }
    const skiff_err err = app->env.stop_worker(app->env.ctx);
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "worker stop: %s (%d)", skiff_err_name(err), (int)err);
    if (err != SKIFF_OK) {
        return 0;
    }
    app->worker_running = 0;
    skiff_romm_client_clear(&app->worker_romm);
    return 1;
}

skiff_err app_reset_server(skiff_app *app) {
    skiff_jobs_destroy(app->jobs);
    app->jobs = NULL;
    skiff_romm_client_clear(&app->romm);
    skiff_transport_destroy(app->transport);
    app->transport = NULL;
    app->has_server = 0;
    app->has_platform = 0;
    app->total_known = 0;
    app->has_rom = 0;
    app->request = REQUEST_NONE;
    app->failed_request = REQUEST_NONE;
    memset(app->pages, 0, sizeof app->pages);
    const skiff_err err = start_queue(app);
    app_queue_refresh(app);
    return err;
}

/* ---- Public API ---- */

static int env_valid(const skiff_app_env *env) {
    return env->now_ms != NULL && env->measure != NULL && env->switch_on != NULL &&
           env->net_start != NULL && env->net_poll != NULL && env->net_profile != NULL &&
           env->net_profile_name != NULL && env->tls_start != NULL && env->open_transport != NULL &&
           env->random != NULL && env->start_worker != NULL && env->stop_worker != NULL;
}

static int locks_valid(const skiff_app_config *config) {
    const int any = config->lock != NULL || config->unlock != NULL || config->log_lock != NULL ||
                    config->jobs_lock != NULL || config->jobs_save_lock != NULL ||
                    config->manifest_lock != NULL;
    if (!any) {
        return 1;
    }
    const void *const mutexes[] = {config->log_lock, config->jobs_lock, config->jobs_save_lock,
                                   config->manifest_lock};
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = i + 1; j < 4; j++) {
            if (mutexes[i] == mutexes[j]) {
                return 0;
            }
        }
    }
    return config->lock != NULL && config->unlock != NULL && config->log_lock != NULL &&
           config->jobs_lock != NULL && config->jobs_save_lock != NULL &&
           config->manifest_lock != NULL;
}

skiff_err skiff_app_create(const skiff_app_config *config, const skiff_app_env *env,
                           skiff_app **out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (config == NULL || env == NULL || config->storage == NULL || !env_valid(env) ||
        !locks_valid(config)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_app *app = calloc(1, sizeof *app);
    if (app == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    app->config = *config;
    app->env = *env;
    app->installer = skiff_install_find_installer(SKIFF_APP_PLATFORM_SLUG);
    app->dirty = 1;
    app->screen = SKIFF_APP_SCREEN_STARTING;
    skiff_ui_list_init(&app->library, 0, SKIFF_APP_ROWS_MAX);
    skiff_ui_list_init(&app->queue_list, 0, SKIFF_APP_ROWS_MAX);
    skiff_ui_list_init(&app->settings_list, 0, SKIFF_APP_ROWS_MAX);
    *out = app;
    return SKIFF_OK;
}

void skiff_app_destroy(skiff_app *app) {
    if (app == NULL) {
        return;
    }
    skiff_romm_pairing_clear(&app->pairing.pairing, NULL);
    skiff_romm_client_clear(&app->romm);
    skiff_romm_client_clear(&app->worker_romm);
    skiff_transport_destroy(app->transport);
    skiff_jobs_destroy(app->jobs);
    skiff_install_manifest_destroy(app->manifest);
    skiff_log_destroy(app->log);
    free(app);
}

static void take_events(skiff_app *app) {
    if (app->jobs == NULL) {
        return;
    }
    skiff_jobs_event event;
    while (skiff_jobs_next_event(app->jobs, &event)) {
        app_queue_event(app, &event);
    }
}

static void message_update(skiff_app *app, unsigned actions) {
    app_message_action action = MESSAGE_NONE;
    if (actions & SKIFF_UI_ACTION_CONFIRM) {
        action = app->message.ok;
    } else if ((actions & SKIFF_UI_ACTION_MENU) && app->message.other != MESSAGE_NONE) {
        action = app->message.other;
    } else if ((actions & SKIFF_UI_ACTION_BACK) &&
               (app->message.ok == MESSAGE_BACK || app->message.other == MESSAGE_BACK)) {
        action = MESSAGE_BACK;
    }
    switch (action) {
    case MESSAGE_BACK:
        /* What failed is given up. */
        app->failed_request = REQUEST_NONE;
        app_set_screen(app, app->message.back);
        break;
    case MESSAGE_RETRY_CONNECT:
        app_connect_begin(app, app->connect);
        break;
    case MESSAGE_RETRY_REQUEST:
        if (!app->net_joined) {
            app_connect_begin(app, CONNECT_NETWORK);
        } else if (app->failed_request == REQUEST_ROM) {
            app_set_screen(app, SKIFF_APP_SCREEN_DETAILS);
            app->request = REQUEST_ROM;
        } else {
            app_set_screen(app, SKIFF_APP_SCREEN_LIBRARY);
            app->request = REQUEST_PAGE;
            app->request_page = app->failed_page;
        }
        break;
    case MESSAGE_RETRY_WORKER: {
        const skiff_err err = app_start_worker(app);
        if (err != SKIFF_OK) {
            app_show_error(app, err, NULL, MESSAGE_RETRY_WORKER, MESSAGE_QUIT, SKIFF_TEXT_QUIT,
                           SKIFF_APP_SCREEN_LIBRARY);
        } else {
            app_resume_after_connect(app);
        }
        break;
    }
    case MESSAGE_RESUME:
        app_connect_begin(app, app->connect);
        break;
    case MESSAGE_PICK_NETWORK:
        app->profile = 0;
        app_connect_begin(app, CONNECT_NETWORK);
        break;
    case MESSAGE_NOTICE_SEEN: {
        char seen[SKIFF_CONFIG_NOTICE_MAX];
        if (app->server.version_known) {
            snprintf(seen, sizeof seen, "%d.%d", app->server.major, app->server.minor);
        } else {
            snprintf(seen, sizeof seen, "%s", app->server.version);
        }
        const skiff_err err = app_config_set(app, SKIFF_CONFIG_SECTION_SKIFF,
                                             SKIFF_CONFIG_KEY_ROMM_NOTICE, seen, NULL);
        skiff_log_write(app->log, SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG, "RomM %s notice seen: %s (%d)",
                        app->server.version, skiff_err_name(err), (int)err);
        app_connect_begin(app, CONNECT_PAIRING);
        break;
    }
    case MESSAGE_NEW_PAIRING:
        if (app->net_joined) {
            app_pair_begin(app);
        } else {
            app->pairing_requested = 1;
            app_connect_begin(app, CONNECT_NETWORK);
        }
        break;
    case MESSAGE_SETTINGS:
        app_set_screen(app, SKIFF_APP_SCREEN_SETTINGS);
        break;
    case MESSAGE_QUIT:
        app->quit = 1;
        break;
    case MESSAGE_NONE:
        break;
    }
}

static void confirm_update(skiff_app *app, unsigned actions) {
    if (actions & SKIFF_UI_ACTION_BACK) {
        app_set_screen(app, app->confirm == CONFIRM_REPLACE      ? SKIFF_APP_SCREEN_DETAILS
                            : app->confirm == CONFIRM_CANCEL_JOB ? SKIFF_APP_SCREEN_QUEUE
                                                                 : app->keyboard_from);
        return;
    }
    if (!(actions & SKIFF_UI_ACTION_CONFIRM)) {
        return;
    }
    switch (app->confirm) {
    case CONFIRM_REPLACE:
        app_download_confirmed(app);
        break;
    case CONFIRM_CANCEL_JOB:
        app_cancel_confirmed(app, app->confirm_job);
        break;
    case CONFIRM_SERVER_CHANGE:
        app_server_change_confirmed(app);
        break;
    }
}

static void server_update(skiff_app *app, unsigned actions) {
    if (actions & SKIFF_UI_ACTION_CONFIRM) {
        app->keyboard_from = SKIFF_APP_SCREEN_SERVER;
        app->dialog = SKIFF_APP_DIALOG_KEYBOARD;
        app->dirty = 1;
    }
}

void skiff_app_update(skiff_app *app, unsigned actions) {
    if (app == NULL || app->quit) {
        return;
    }
    if (app->dialog != SKIFF_APP_DIALOG_NONE) {
        actions = 0;
    }
    if ((actions & SKIFF_UI_ACTION_START) && app->screen != SKIFF_APP_SCREEN_STARTING) {
        app->quit = 1;
        return;
    }
    take_events(app);
    if (app->note[0] != '\0' && app_now(app) >= app->note_until_ms) {
        app->note[0] = '\0';
        app->dirty = 1;
    }
    switch (app->screen) {
    case SKIFF_APP_SCREEN_STARTING:
        if (!app->announced) {
            app->announced = 1;
            app->dirty = 1;
        } else {
            start(app);
        }
        break;
    case SKIFF_APP_SCREEN_SERVER:
        server_update(app, actions);
        break;
    case SKIFF_APP_SCREEN_CONNECTING:
        app_connect_update(app, actions);
        break;
    case SKIFF_APP_SCREEN_PAIR:
        app_pair_update(app, actions);
        break;
    case SKIFF_APP_SCREEN_LIBRARY:
        app_library_update(app, actions);
        break;
    case SKIFF_APP_SCREEN_DETAILS:
        app_details_update(app, actions);
        break;
    case SKIFF_APP_SCREEN_QUEUE:
        app_queue_update(app, actions);
        break;
    case SKIFF_APP_SCREEN_SETTINGS:
        app_settings_update(app, actions);
        break;
    case SKIFF_APP_SCREEN_MESSAGE:
        message_update(app, actions);
        break;
    case SKIFF_APP_SCREEN_CONFIRM:
        confirm_update(app, actions);
        break;
    }
    const int64_t now = app_now(app);
    const int shows_progress =
        app->screen == SKIFF_APP_SCREEN_QUEUE || app->screen == SKIFF_APP_SCREEN_LIBRARY;
    if (shows_progress && app->queue.progress_job != 0 &&
        now - app->last_refresh_ms >= SKIFF_APP_PROGRESS_REFRESH_MS) {
        app->dirty = 1;
    }
    if (app->dirty) {
        app_view_build(app);
        app->last_refresh_ms = now;
        app->dirty = 0;
    }
}

const skiff_app_view *skiff_app_view_now(const skiff_app *app) {
    return app != NULL ? &app->view : NULL;
}

void skiff_app_dialog_done(skiff_app *app, skiff_app_dialog_result result, const char *text) {
    if (app == NULL || app->dialog == SKIFF_APP_DIALOG_NONE) {
        return;
    }
    const skiff_app_dialog dialog = app->dialog;
    app->dialog = SKIFF_APP_DIALOG_NONE;
    app->dirty = 1;
    if (dialog == SKIFF_APP_DIALOG_NETWORK) {
        app_network_picked(app, result);
        return;
    }
    if (result == SKIFF_APP_DIALOG_ACCEPTED && text != NULL) {
        app_server_entered(app, text);
    }
}

int skiff_app_quit_requested(const skiff_app *app) { return app != NULL && app->quit; }

skiff_log *skiff_app_log(const skiff_app *app) { return app != NULL ? app->log : NULL; }

skiff_err skiff_app_transport_settings_from(const skiff_config *config,
                                            const skiff_storage_roots *roots,
                                            skiff_app_transport_settings *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (config == NULL || roots == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const char *const ca_file =
        config->ca_file[0] != '\0' ? config->ca_file : SKIFF_APP_DEFAULT_CA_FILE;
    const char *const names[] = {ca_file, config->cert_file, config->key_file};
    char *const paths[] = {out->ca_file, out->client_cert, out->client_key};
    for (size_t i = 0; i < 3; i++) {
        if (names[i][0] == '\0') {
            continue;
        }
        char logical[SKIFF_STORAGE_PATH_MAX];
        snprintf(logical, sizeof logical, "%s/%s", SKIFF_STORAGE_ROOT_APP, names[i]);
        const skiff_err err =
            skiff_storage_resolve(roots, logical, paths[i], SKIFF_STORAGE_PATH_MAX);
        if (err != SKIFF_OK) {
            memset(out, 0, sizeof *out);
            return err;
        }
    }
    out->header_count = skiff_config_headers(config, out->headers, SKIFF_CONFIG_HEADERS_MAX);
    return SKIFF_OK;
}
