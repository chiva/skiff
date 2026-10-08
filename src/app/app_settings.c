#include <stdio.h>
#include <string.h>

#include "app_internal.h"

/* The rows of the settings screen, in order. */
enum { SETTINGS_SERVER, SETTINGS_PAIR, SETTINGS_ROWS };

static void open_keyboard(skiff_app *app, skiff_app_screen from) {
    app->keyboard_from = from;
    app->dialog = SKIFF_APP_DIALOG_KEYBOARD;
    app->dirty = 1;
}

void app_settings_update(skiff_app *app, unsigned actions) {
    skiff_ui_list_set_count(&app->settings_list, SETTINGS_ROWS);
    if (actions & SKIFF_UI_ACTION_BACK) {
        if (app->connect == CONNECT_DONE) {
            app_set_screen(app, SKIFF_APP_SCREEN_LIBRARY);
        } else if (app->settings.server_url[0] == '\0') {
            app_set_screen(app, SKIFF_APP_SCREEN_SERVER);
        } else {
            /* Back to where the walk to the library stopped. */
            app_connect_begin(app, app->connect);
        }
        return;
    }
    if (skiff_ui_list_apply(&app->settings_list, actions)) {
        app->dirty = 1;
    }
    if (!(actions & SKIFF_UI_ACTION_CONFIRM)) {
        return;
    }
    if (app->settings_list.selected == SETTINGS_SERVER) {
        open_keyboard(app, SKIFF_APP_SCREEN_SETTINGS);
    } else if (app->settings.server_url[0] != '\0') {
        app_pair_begin(app);
    }
}

/* config.ini with a new address: the token and RomM's device id were the old server's, so they go
 * (this PSP's own identifier stays). The text is parsed back, so a bad address is refused here. */
static skiff_err server_text(skiff_app *app, const char *url, char *text, size_t *text_length,
                             skiff_config_issue *issue) {
    const char *const keys[][3] = {
        {SKIFF_CONFIG_SECTION_SERVER, SKIFF_CONFIG_KEY_URL, url},
        {SKIFF_CONFIG_SECTION_AUTH, SKIFF_CONFIG_KEY_TOKEN, ""},
        {SKIFF_CONFIG_SECTION_AUTH, SKIFF_CONFIG_KEY_DEVICE_ID, ""},
    };
    memcpy(text, app->config_text, app->config_length + 1);
    *text_length = app->config_length;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        size_t length = 0;
        const skiff_err err =
            skiff_config_set(text, *text_length, keys[i][0], keys[i][1], keys[i][2], app->edit_text,
                             sizeof app->edit_text, &length, issue);
        if (err != SKIFF_OK) {
            return err;
        }
        memcpy(text, app->edit_text, length + 1);
        *text_length = length;
    }
    return SKIFF_OK;
}

/* The old server's downloads cannot run against the new one (other ROM ids, other files): they
 * are cancelled, their partial files deleted, once the worker has stopped. The first failure is
 * returned: the address then stays the old one. */
static skiff_err cancel_old_downloads(skiff_app *app) {
    app_queue_refresh(app);
    skiff_err err = SKIFF_OK;
    size_t cancelled = 0;
    for (size_t i = 0; i < app->queue.count && err == SKIFF_OK; i++) {
        const skiff_job *job = &app->queue.jobs[i];
        if (job->state == SKIFF_JOB_QUEUED || job->state == SKIFF_JOB_ACTIVE ||
            job->state == SKIFF_JOB_FAILED) {
            err = skiff_jobs_cancel(app->jobs, job->id);
            cancelled += err == SKIFF_OK;
        }
    }
    if (err == SKIFF_OK) {
        err = skiff_jobs_clear_finished(app->jobs);
    }
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "new server: %zu download(s) cancelled, queue cleared: %s (%d)", cancelled,
                    skiff_err_name(err), (int)err);
    return err;
}

/* A step of the change failed after the worker stopped: the old server stays, and connecting to it
 * again starts a new worker on the queue as the file now holds it. */
static void keep_old_server(skiff_app *app, skiff_err err, const skiff_config_issue *issue) {
    const skiff_err reset = app_reset_server(app);
    app->connect = CONNECT_HEARTBEAT;
    app_show_error(app, reset != SKIFF_OK ? reset : err, issue,
                   reset != SKIFF_OK ? MESSAGE_QUIT : MESSAGE_RESUME, MESSAGE_NONE, SKIFF_TEXT_OK,
                   SKIFF_APP_SCREEN_SETTINGS);
}

/* In this order, so the new address is saved only once nothing of the old server's can run: stop
 * the worker, cancel the old downloads, save config.ini, start over with the new server. */
static void apply_server(skiff_app *app, const char *url) {
    const skiff_app_screen from = app->keyboard_from;
    char text[SKIFF_CONFIG_TEXT_MAX + 1];
    size_t length = 0;
    skiff_config_issue issue;
    memset(&issue, 0, sizeof issue);
    skiff_err err = server_text(app, url, text, &length, &issue);
    if (err != SKIFF_OK) {
        app_show_error(app, err, &issue, MESSAGE_BACK, MESSAGE_NONE, SKIFF_TEXT_OK, from);
        return;
    }
    if (!app_stop_worker(app)) {
        app_show_text(app, SKIFF_TEXT_TITLE_NOTICE, app_text(app, SKIFF_TEXT_STOP_FAILED),
                      MESSAGE_BACK, from);
        return;
    }
    err = cancel_old_downloads(app);
    if (err == SKIFF_OK) {
        err = app_config_replace(app, text, length);
    }
    if (err != SKIFF_OK) {
        keep_old_server(app, err, &issue);
        return;
    }
    err = app_reset_server(app);
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "server address changed: %s (%d)", skiff_err_name(err), (int)err);
    if (err != SKIFF_OK) {
        app_show_error(app, err, NULL, MESSAGE_QUIT, MESSAGE_NONE, SKIFF_TEXT_OK, from);
        return;
    }
    app_connect_begin(app, CONNECT_NETWORK);
}

void app_server_entered(skiff_app *app, const char *url) {
    const skiff_app_screen from = app->keyboard_from;
    if (strcmp(url, app->settings.server_url) == 0) {
        app_set_screen(app, from);
        return;
    }
    char text[SKIFF_CONFIG_TEXT_MAX + 1];
    size_t length = 0;
    skiff_config_issue issue;
    memset(&issue, 0, sizeof issue);
    const skiff_err err = server_text(app, url, text, &length, &issue);
    if (err != SKIFF_OK) {
        app_show_error(app, err, &issue, MESSAGE_BACK, MESSAGE_NONE, SKIFF_TEXT_OK, from);
        return;
    }
    app_queue_refresh(app);
    if (app_queue_unfinished(app) > 0) {
        snprintf(app->pending_url, sizeof app->pending_url, "%s", url);
        app_confirm(app, SKIFF_TEXT_CONFIRM_SERVER_CHANGE, CONFIRM_SERVER_CHANGE, 0);
        return;
    }
    apply_server(app, url);
}

void app_server_change_confirmed(skiff_app *app) {
    char url[SKIFF_CONFIG_URL_MAX];
    snprintf(url, sizeof url, "%s", app->pending_url);
    app->pending_url[0] = '\0';
    apply_server(app, url);
}
