#include <stdio.h>
#include <string.h>

#include "app_internal.h"

/* Errors that say the network, not RomM, failed: the next try joins the Wi-Fi again. */
int app_is_network_error(skiff_err err) {
    return err == SKIFF_ERR_NET_UNAVAILABLE || err == SKIFF_ERR_NET_DNS ||
           err == SKIFF_ERR_NET_CONNECT || err == SKIFF_ERR_NET_TIMEOUT ||
           err == SKIFF_ERR_NET_WIFI_JOIN || err == SKIFF_ERR_NET_CONNECTION_LOST;
}

void app_network_failed(skiff_app *app, skiff_err err) {
    if (!app_is_network_error(err)) {
        return;
    }
    /* A connection kept from before is likely dead too. */
    app->net_joined = 0;
    app_drop_client(app);
}

void app_drop_client(skiff_app *app) {
    if (app_call_busy(app)) {
        app->call.drop_client = 1;
        return;
    }
    skiff_romm_client_clear(&app->romm);
    skiff_transport_destroy(app->transport);
    app->transport = NULL;
}

void app_resume_after_connect(skiff_app *app) {
    if (app->failed_request == REQUEST_ROM) {
        app->failed_request = REQUEST_NONE;
        app_set_screen(app, SKIFF_APP_SCREEN_DETAILS);
        app->request = REQUEST_ROM;
        return;
    }
    app->failed_request = REQUEST_NONE;
    app_library_open(app);
}

void app_connect_begin(skiff_app *app, app_connect_step from) {
    app->connect = from;
    app->waiting_switch = 0;
    app_set_screen(app, SKIFF_APP_SCREEN_CONNECTING);
}

static void connect_failed(skiff_app *app, skiff_err err) {
    app_network_failed(app, err);
    if (app_is_network_error(err)) {
        app->connect = CONNECT_NETWORK;
        app_show_error(app, err, NULL, MESSAGE_RETRY_CONNECT, MESSAGE_PICK_NETWORK,
                       SKIFF_TEXT_CHOOSE_NETWORK, SKIFF_APP_SCREEN_CONNECTING);
        return;
    }
    app_show_error(app, err, NULL, MESSAGE_RETRY_CONNECT, MESSAGE_SETTINGS, SKIFF_TEXT_SETTINGS,
                   SKIFF_APP_SCREEN_CONNECTING);
}

skiff_err app_ensure_client(skiff_app *app) {
    if (app->transport != NULL) {
        return SKIFF_OK;
    }
    skiff_err err = skiff_app_transport_settings_from(&app->settings, &app->config.roots,
                                                      &app->transport_settings);
    if (err == SKIFF_OK) {
        err = app->env.open_transport(app->env.ctx, &app->transport_settings, &app->transport);
    }
    if (err == SKIFF_OK) {
        err = skiff_romm_client_init(&app->romm, app->transport, app->settings.server_url,
                                     app->settings.token);
    }
    if (err != SKIFF_OK) {
        skiff_transport_destroy(app->transport);
        app->transport = NULL;
    }
    return err;
}

/* ---- Joining the Wi-Fi ---- */

static void start_join(skiff_app *app) {
    if (app_now(app) < app->join_retry_ms) {
        return;
    }
    if (!app->env.switch_on(app->env.ctx)) {
        if (!app->waiting_switch) {
            app->waiting_switch = 1;
            app->dirty = 1;
        }
        return;
    }
    app->waiting_switch = 0;
    if (app->profile == 0) {
        app->dialog = SKIFF_APP_DIALOG_NETWORK;
        app->dirty = 1;
        return;
    }
    if (app->env.net_profile_name(app->env.ctx, app->profile, app->profile_name,
                                  sizeof app->profile_name) != SKIFF_OK) {
        snprintf(app->profile_name, sizeof app->profile_name, "%d", app->profile);
    }
    const skiff_err err = app->env.net_start(app->env.ctx, app->profile);
    skiff_log_write(app->log, SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG, "joining connection %d: %s (%d)",
                    app->profile, skiff_err_name(err), (int)err);
    if (err != SKIFF_OK) {
        connect_failed(app, err);
        return;
    }
    app->connect = CONNECT_JOINING;
    app->dirty = 1;
}

static void poll_join(skiff_app *app) {
    int joined = 0;
    const skiff_err err = app->env.net_poll(app->env.ctx, &joined);
    if (err == SKIFF_ERR_NET_WIFI_JOIN && app->join_retries < SKIFF_APP_JOIN_RETRIES) {
        app->join_retries++;
        app->join_retry_ms = app_now(app) + SKIFF_APP_JOIN_RETRY_MS;
        skiff_log_write(app->log, SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG,
                        "join: %s (%d), trying again (%d of %d)", skiff_err_name(err), (int)err,
                        app->join_retries, SKIFF_APP_JOIN_RETRIES);
        app->connect = CONNECT_NETWORK;
        return;
    }
    if (err != SKIFF_OK) {
        app->join_retries = 0;
        skiff_log_write(app->log, SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG, "join: %s (%d)",
                        skiff_err_name(err), (int)err);
        connect_failed(app, err);
        return;
    }
    if (joined) {
        app->join_retries = 0;
        app->net_joined = 1;
        app->connect = CONNECT_TLS;
        app->dirty = 1;
    }
}

void app_network_picked(skiff_app *app, skiff_app_dialog_result result) {
    if (result != SKIFF_APP_DIALOG_ACCEPTED) {
        skiff_log_write(app->log, SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG, "network picker: %s",
                        result == SKIFF_APP_DIALOG_CANCELLED ? "cancelled" : "failed");
        app_show_error(app, SKIFF_ERR_NET_UNAVAILABLE, NULL, MESSAGE_PICK_NETWORK, MESSAGE_SETTINGS,
                       SKIFF_TEXT_SETTINGS, SKIFF_APP_SCREEN_CONNECTING);
        return;
    }
    int profile = 0;
    skiff_err err = app->env.net_profile(app->env.ctx, &profile);
    if (err == SKIFF_OK) {
        char value[16];
        snprintf(value, sizeof value, "%d", profile);
        err = app_config_set(app, SKIFF_CONFIG_SECTION_NETWORK, SKIFF_CONFIG_KEY_PROFILE, value,
                             NULL);
        app->profile = profile;
    }
    /* Joined either way; only the next launch would ask again. */
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG,
                    "network picker joined connection %d, saved: %s (%d)", profile,
                    skiff_err_name(err), (int)err);
    app->net_joined = 1;
    /* The worker rejoins the connection it was started with: a new one means a new worker. */
    if (app->worker_running && app->worker_profile != app->profile) {
        if (!app_stop_worker(app)) {
            app_show_text(app, SKIFF_TEXT_TITLE_NOTICE, app_text(app, SKIFF_TEXT_RESTART_TO_APPLY),
                          MESSAGE_QUIT, SKIFF_APP_SCREEN_CONNECTING);
            return;
        }
        const skiff_err reset = app_reset_server(app);
        if (reset != SKIFF_OK) {
            app_show_error(app, reset, NULL, MESSAGE_QUIT, MESSAGE_NONE, SKIFF_TEXT_OK,
                           SKIFF_APP_SCREEN_CONNECTING);
            return;
        }
    }
    app_connect_begin(app, CONNECT_TLS);
}

/* ---- RomM ---- */

static int notice_needed(const skiff_app *app) {
    if (app->server.version_known && !app->server.newer_than_tested) {
        return 0;
    }
    char seen[SKIFF_CONFIG_NOTICE_MAX];
    if (app->server.version_known) {
        snprintf(seen, sizeof seen, "%d.%d", app->server.major, app->server.minor);
    } else {
        snprintf(seen, sizeof seen, "%s", app->server.version);
    }
    return strcmp(seen, app->settings.romm_notice) != 0;
}

void app_heartbeat_done(skiff_app *app) {
    const skiff_err err = app->call.err;
    app->server = app->call.server;
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "RomM %s: %s (%d)", app->server.version, skiff_err_name(err), (int)err);
    if (err == SKIFF_ERR_ROMM_UNSUPPORTED_VERSION) {
        app_show_error(app, err, NULL, MESSAGE_QUIT, MESSAGE_SETTINGS, SKIFF_TEXT_SETTINGS,
                       SKIFF_APP_SCREEN_CONNECTING);
        return;
    }
    if (err != SKIFF_OK) {
        connect_failed(app, err);
        return;
    }
    app->has_server = 1;
    app->connect = CONNECT_PAIRING;
    if (notice_needed(app)) {
        char tested[16];
        snprintf(tested, sizeof tested, "%d.%d", SKIFF_ROMM_TESTED_MAJOR, SKIFF_ROMM_TESTED_MINOR);
        const char *args[] = {app->server.version, tested};
        char text[SKIFF_TEXT_MAX];
        app_format(app, SKIFF_TEXT_NEWER_ROMM, args, 2, text);
        app_show_text(app, SKIFF_TEXT_TITLE_NOTICE, text, MESSAGE_NOTICE_SEEN,
                      SKIFF_APP_SCREEN_CONNECTING);
    }
}

void app_platform_done(skiff_app *app) {
    skiff_err err = app->call.err;
    app->platform = app->call.platform;
    skiff_log_write(app->log, SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG, "platform %s: %s (%d)",
                    SKIFF_APP_PLATFORM_SLUG, skiff_err_name(err), (int)err);
    if (err != SKIFF_OK && err != SKIFF_ERR_ROMM_NOT_FOUND) {
        connect_failed(app, err);
        return;
    }
    /* No PSP platform in RomM: the library is empty, which is not an error. */
    app->has_platform = err == SKIFF_OK;
    app->connect = CONNECT_DONE;
    err = app_start_worker(app);
    if (err != SKIFF_OK) {
        /* Downloads would wait for ever: say so instead of browsing as if they ran. */
        app_show_error(app, err, NULL, MESSAGE_RETRY_WORKER, MESSAGE_QUIT, SKIFF_TEXT_QUIT,
                       SKIFF_APP_SCREEN_LIBRARY);
        return;
    }
    app_resume_after_connect(app);
}

void app_connect_update(skiff_app *app, unsigned actions) {
    (void)actions;
    switch (app->connect) {
    case CONNECT_NETWORK:
        if (app->net_joined) {
            app->connect = CONNECT_TLS;
        } else {
            start_join(app);
        }
        return;
    case CONNECT_JOINING:
        poll_join(app);
        return;
    case CONNECT_TLS:
        if (!app->tls_started) {
            const skiff_err err = app->env.tls_start(app->env.ctx);
            skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR,
                            SKIFF_APP_LOG_TAG, "TLS: %s (%d)", skiff_err_name(err), (int)err);
            if (err != SKIFF_OK) {
                app_show_error(app, err, NULL, MESSAGE_QUIT, MESSAGE_NONE, SKIFF_TEXT_OK,
                               SKIFF_APP_SCREEN_CONNECTING);
                return;
            }
            app->tls_started = 1;
        }
        app->connect = CONNECT_HEARTBEAT;
        app->dirty = 1;
        return;
    case CONNECT_HEARTBEAT:
    case CONNECT_PLATFORM:
        /* The step's request runs off this thread; its result moves the walk on. */
        if (!app_call_busy(app)) {
            app_call_start(app, app->connect == CONNECT_HEARTBEAT ? CALL_HEARTBEAT
                                                                  : CALL_PLATFORM);
        }
        return;
    case CONNECT_PAIRING:
        if (app->settings.token[0] == '\0' || app->pairing_requested) {
            app_pair_begin(app);
        } else {
            app->connect = CONNECT_PLATFORM;
            app->dirty = 1;
        }
        return;
    case CONNECT_DONE:
        app_resume_after_connect(app);
        return;
    }
}

/* ---- Pairing ---- */

void app_pair_begin(skiff_app *app) {
    app->pairing_requested = 0;
    /* A request of the pairing this replaces must not land in the new one. */
    if (app->call.kind == CALL_PAIRING_START || app->call.kind == CALL_PAIRING_POLL) {
        app_call_abandon(app);
    }
    skiff_romm_pairing_clear(&app->pairing.pairing, NULL);
    memset(&app->pairing, 0, sizeof app->pairing);
    app_set_screen(app, SKIFF_APP_SCREEN_PAIR);
}

static skiff_err ensure_device_identifier(skiff_app *app) {
    if (app->settings.device_identifier[0] != '\0') {
        return SKIFF_OK;
    }
    unsigned char random[SKIFF_ROMM_DEVICE_IDENTIFIER_BYTES];
    char identifier[SKIFF_CONFIG_DEVICE_IDENTIFIER_MAX];
    skiff_err err = app->env.random(app->env.ctx, random, sizeof random);
    if (err == SKIFF_OK) {
        err = skiff_romm_device_identifier(random, sizeof random, identifier, sizeof identifier);
    }
    if (err == SKIFF_OK) {
        err = app_config_set(app, SKIFF_CONFIG_SECTION_AUTH, SKIFF_CONFIG_KEY_DEVICE_IDENTIFIER,
                             identifier, NULL);
    }
    return err;
}

static void start_pairing(skiff_app *app) {
    const skiff_err err = ensure_device_identifier(app);
    if (err != SKIFF_OK) {
        app_show_error(app, err, NULL, MESSAGE_NEW_PAIRING, MESSAGE_SETTINGS, SKIFF_TEXT_SETTINGS,
                       SKIFF_APP_SCREEN_PAIR);
        return;
    }
    snprintf(app->call.device_identifier, sizeof app->call.device_identifier, "%s",
             app->settings.device_identifier);
    app_call_start(app, CALL_PAIRING_START);
}

void app_pairing_started(skiff_app *app) {
    const skiff_err err = app->call.err;
    if (err == SKIFF_OK) {
        app->pairing.pairing = app->call.pairing;
    }
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "pairing start: %s (%d), expires in %u s, poll every %u s", skiff_err_name(err),
                    (int)err, (unsigned)app->pairing.pairing.expires_in_s,
                    (unsigned)app->pairing.pairing.interval_s);
    if (err != SKIFF_OK) {
        app_network_failed(app, err);
        app_show_error(app, err, NULL, MESSAGE_NEW_PAIRING, MESSAGE_SETTINGS, SKIFF_TEXT_SETTINGS,
                       SKIFF_APP_SCREEN_PAIR);
        return;
    }
    /* A phone reads the address from the screen; without a code (too long for one) it is typed. */
    const skiff_err qr =
        skiff_ui_qr_encode(app->pairing.pairing.verification_url_complete, &app->pairing.qr);
    skiff_log_write(app->log, SKIFF_LOG_DEBUG, SKIFF_APP_LOG_TAG,
                    "pairing address as a QR code: %d modules a side: %s (%d)",
                    app->pairing.qr.size, skiff_err_name(qr), (int)qr);
    const int64_t now = app_now(app);
    app->pairing.active = 1;
    app->pairing.started_ms = now;
    app->pairing.next_poll_ms = now + (int64_t)app->pairing.pairing.interval_s * APP_MS_PER_S;
    app->dirty = 1;
}

static void end_pairing(skiff_app *app, skiff_err err) {
    if (app->call.kind == CALL_PAIRING_START || app->call.kind == CALL_PAIRING_POLL) {
        app_call_abandon(app);
    }
    skiff_romm_pairing_clear(&app->pairing.pairing, NULL);
    app->pairing.qr.size = 0;
    app->pairing.active = 0;
    app->pairing.ended = err;
    skiff_log_write(app->log, SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG, "pairing ended: %s (%d)",
                    skiff_err_name(err), (int)err);
    app->dirty = 1;
}

static void paired(skiff_app *app, skiff_romm_pairing_result *result) {
    size_t length = 0;
    skiff_config_issue issue;
    memset(&issue, 0, sizeof issue);
    const int had_token = app->settings.token[0] != '\0';
    skiff_err err = skiff_romm_pairing_config_text(
        app->config_text, app->config_length, app->settings.device_identifier, result,
        app->edit_text, sizeof app->edit_text, &length, &issue);
    if (err == SKIFF_OK) {
        err = app_config_replace(app, app->edit_text, length);
    }
    skiff_romm_pairing_clear(&app->pairing.pairing, result);
    app->pairing.active = 0;
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "paired as device %s, saved: %s (%d)", app->settings.device_id,
                    skiff_err_name(err), (int)err);
    if (err != SKIFF_OK) {
        app_show_error(app, err, &issue, MESSAGE_NEW_PAIRING, MESSAGE_SETTINGS, SKIFF_TEXT_SETTINGS,
                       SKIFF_APP_SCREEN_PAIR);
        return;
    }
    const char *args[] = {app->settings.server_url};
    char text[SKIFF_TEXT_MAX];
    app_format(app, SKIFF_TEXT_PAIR_DONE, args, 1, text);
    app_note(app, text);
    if (had_token || app->worker_running) {
        /* A new token for a running app: the worker and the queue start again with it. */
        if (!app_stop_worker(app)) {
            app_show_text(app, SKIFF_TEXT_TITLE_NOTICE, app_text(app, SKIFF_TEXT_RESTART_TO_APPLY),
                          MESSAGE_QUIT, SKIFF_APP_SCREEN_SETTINGS);
            return;
        }
        err = app_reset_server(app);
        if (err != SKIFF_OK) {
            app_show_error(app, err, NULL, MESSAGE_QUIT, MESSAGE_NONE, SKIFF_TEXT_OK,
                           SKIFF_APP_SCREEN_SETTINGS);
            return;
        }
        app_connect_begin(app, CONNECT_HEARTBEAT);
        return;
    }
    /* The client asked without a token until now. */
    skiff_romm_client_clear(&app->romm);
    err = skiff_romm_client_init(&app->romm, app->transport, app->settings.server_url,
                                 app->settings.token);
    if (err != SKIFF_OK) {
        connect_failed(app, err);
        return;
    }
    app_connect_begin(app, CONNECT_PLATFORM);
}

static void poll_pairing(skiff_app *app) {
    app->call.pairing = app->pairing.pairing;
    memset(&app->call.pairing_result, 0, sizeof app->call.pairing_result);
    app_call_start(app, CALL_PAIRING_POLL);
}

void app_pairing_polled(skiff_app *app) {
    const int64_t now = app_now(app);
    const skiff_err err = app->call.err;
    skiff_romm_pairing_result *result = &app->call.pairing_result;
    /* A poll may lengthen the interval (slow_down). */
    app->pairing.pairing.interval_s = app->call.pairing.interval_s;
    if (err == SKIFF_ERR_ROMM_PAIRING_DENIED || err == SKIFF_ERR_ROMM_PAIRING_EXPIRED ||
        err == SKIFF_ERR_ROMM_PAIRING_SCOPES) {
        end_pairing(app, err);
        return;
    }
    if (err == SKIFF_OK && result->state == SKIFF_ROMM_PAIRING_APPROVED) {
        paired(app, result);
        return;
    }
    /* Pending, slowed down, or a poll the network or RomM failed: try again after the interval,
     * until the code expires. */
    if (err != SKIFF_OK) {
        skiff_log_write(app->log, SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG, "pairing poll: %s (%d)",
                        skiff_err_name(err), (int)err);
    }
    if (err != app->pairing.last_error) {
        app->pairing.last_error = err;
        app->dirty = 1;
    }
    app->pairing.next_poll_ms = now + (int64_t)app->pairing.pairing.interval_s * APP_MS_PER_S;
}

void app_pair_update(skiff_app *app, unsigned actions) {
    if ((actions & SKIFF_UI_ACTION_MENU) ||
        ((actions & SKIFF_UI_ACTION_BACK) && app->settings.token[0] != '\0')) {
        end_pairing(app, SKIFF_ERR_CANCELLED);
        app_set_screen(app, SKIFF_APP_SCREEN_SETTINGS);
        return;
    }
    if (!app->pairing.active) {
        if (app->pairing.ended != SKIFF_OK) {
            if (actions & SKIFF_UI_ACTION_CONFIRM) {
                app_pair_begin(app);
            }
            return;
        }
        if (!app_call_busy(app)) {
            start_pairing(app);
        }
        return;
    }
    const int64_t now = app_now(app);
    const int64_t expires_ms =
        app->pairing.started_ms + (int64_t)app->pairing.pairing.expires_in_s * APP_MS_PER_S;
    if (now >= expires_ms) {
        end_pairing(app, SKIFF_ERR_ROMM_PAIRING_EXPIRED);
        return;
    }
    if (now >= app->pairing.next_poll_ms && !app_call_busy(app)) {
        poll_pairing(app);
        return;
    }
    /* The countdown changes once a second. */
    if ((expires_ms - now) / APP_MS_PER_S != (expires_ms - app->last_refresh_ms) / APP_MS_PER_S) {
        app->dirty = 1;
    }
}
