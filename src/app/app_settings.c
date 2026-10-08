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
        app_set_screen(app, app->connect == CONNECT_DONE ? SKIFF_APP_SCREEN_LIBRARY
                                                         : SKIFF_APP_SCREEN_SERVER);
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
        app->connect = CONNECT_PAIRING;
        app_pair_begin(app);
    }
}

/* A new address: the token was RomM's for the old server, so it goes, and the app starts over
 * from joining (the worker and the queue too). */
void app_server_entered(skiff_app *app, const char *url) {
    const skiff_app_screen from = app->keyboard_from;
    if (strcmp(url, app->settings.server_url) == 0) {
        app_set_screen(app, from);
        return;
    }
    size_t length = 0;
    skiff_config_issue issue;
    memset(&issue, 0, sizeof issue);
    /* Three keys, one save: each edit goes to edit_text and back to config_text. */
    const char *const keys[][3] = {
        {SKIFF_CONFIG_SECTION_SERVER, SKIFF_CONFIG_KEY_URL, url},
        {SKIFF_CONFIG_SECTION_AUTH, SKIFF_CONFIG_KEY_TOKEN, ""},
        {SKIFF_CONFIG_SECTION_AUTH, SKIFF_CONFIG_KEY_DEVICE_ID, ""},
    };
    char text[SKIFF_CONFIG_TEXT_MAX + 1];
    memcpy(text, app->config_text, app->config_length + 1);
    size_t text_length = app->config_length;
    skiff_err err = SKIFF_OK;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0] && err == SKIFF_OK; i++) {
        err = skiff_config_set(text, text_length, keys[i][0], keys[i][1], keys[i][2],
                               app->edit_text, sizeof app->edit_text, &length, &issue);
        if (err == SKIFF_OK) {
            memcpy(text, app->edit_text, length + 1);
            text_length = length;
        }
    }
    const int was_running = app->worker_running;
    if (err == SKIFF_OK && !app_stop_worker(app)) {
        /* Saved, but the worker still uses the old server: the next launch uses the new one. */
        (void)app_config_replace(app, text, text_length);
        app_show_text(app, SKIFF_TEXT_TITLE_NOTICE, app_text(app, SKIFF_TEXT_RESTART_TO_APPLY),
                      MESSAGE_QUIT, from);
        return;
    }
    if (err == SKIFF_OK) {
        err = app_config_replace(app, text, text_length);
    }
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "server address changed (worker was %s): %s (%d)",
                    was_running ? "running" : "stopped", skiff_err_name(err), (int)err);
    if (err != SKIFF_OK) {
        app_show_error(app, err, &issue, MESSAGE_BACK, MESSAGE_NONE, SKIFF_TEXT_OK, from);
        return;
    }
    err = app_reset_server(app);
    if (err != SKIFF_OK) {
        app_show_error(app, err, NULL, MESSAGE_QUIT, MESSAGE_NONE, SKIFF_TEXT_OK, from);
        return;
    }
    app_connect_begin(app, CONNECT_NETWORK);
}
