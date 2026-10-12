/*
 * Games sent from RomM: a player picks this PSP in RomM's web UI ("Install on device", RomM 5.4)
 * and Skiff adds the game to Downloads without asking. RomM keeps each send as an install request
 * until the device reports it (skiff/romm_installs.h); Skiff holds no socket, so it claims them:
 * right after connecting, then every SKIFF_APP_INSTALL_CLAIM_INTERVAL_MS while connected, only
 * when no other call runs or waits (library pages and "Download all" go first, save sync after). A
 * claim that fails on the network never joins the Wi-Fi again: claims wait for whatever joins it
 * next.
 *
 * A round is three calls on the browsing thread, with the sorting on the screen's:
 *   1. CALL_SENDS_CLAIM: remote_install sent once per connection, the claim, each claimed game's
 *      details (one request each) and the Memory Stick's free space;
 *   2. each send sorted: a PSP game, one file, then Download all's rules (app_batch_take()) on a
 *      batch of the round's own, cut to what fits the free space;
 *   3. CALL_SENDS_QUEUE: the chosen games queued with one save (app_batch_queue_run());
 *   4. CALL_SENDS_REPORT: every send reported, only after that save. The queue is free again
 *      meanwhile: a slow RomM holds no download, cancel or retry of the player's.
 *
 * Skiff keeps no record of sends: one claimed and not reported (Skiff quit, the Wi-Fi went, the
 * report failed) comes back on the next claim and finds its game in Downloads (done) or installed
 * (already_installed), with nothing queued twice. A send whose details were lost with the
 * connection is not reported: it stays taken in RomM for the next claim. Every other one is, a
 * refusal with its reason in the PSP's language: fixed text and Skiff's error sentences, never
 * RomM's text, a token or a path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_internal.h"

/* The characters of a request id the log shows. */
#define LOG_ID_CHARS 8
/* A count as text, and the log's note on a send's report. */
#define NUMBER_TEXT_MAX 24
#define REPORT_NOTE_MAX 80

/* What becomes of a claimed send. */
typedef enum send_fate {
    /* Not reported: it stays taken in RomM and comes back on the next claim. */
    FATE_LEFT,
    /* Chosen for the queue: the queue call settles it. */
    FATE_QUEUE,
    FATE_DONE,
    FATE_ALREADY_INSTALLED,
    FATE_FAILED,
} send_fate;

/* One claimed send, from its claim to its report. */
typedef struct app_send {
    skiff_romm_install_request request;
    /* Its game's details (rom_err SKIFF_OK) or why not; not asked when the round lost the
     * connection first. */
    int asked;
    skiff_err rom_err;
    skiff_romm_rom_summary rom;
    size_t rom_files;
    send_fate fate;
    /* FATE_QUEUE: the game's place in the round's batch. */
    size_t game;
    /* This round added its game to Downloads (counted once per game). */
    int queued;
    /* For the log: a few English words, and the error behind them (SKIFF_OK for none). */
    const char *why;
    skiff_err why_err;
    /* FATE_FAILED: what RomM shows the player. */
    char reason[SKIFF_TEXT_MAX];
    int reported;
    skiff_err report_err;
} app_send;

struct app_sends_round {
    /* In: the device, and remote_install as it is sent first (when register_first). */
    char device_id[SKIFF_CONFIG_DEVICE_ID_MAX];
    int on;
    int register_first;
    /* Out of the claim call: what RomM was told and returned, each send (claim.count of them, on
     * the heap), the free space, and the connection lost while the details were read. */
    int registered;
    int claimed;
    int64_t claim_ms;
    skiff_romm_install_claim claim;
    app_send *sends;
    skiff_err free_err;
    uint64_t free_bytes;
    skiff_err lost;
    /* The games to queue, on the heap once one is sorted, and what queueing them returned. */
    app_batch *batch;
    skiff_err queue_err;
};

static const char *const FATE_NAMES[] = {
    [FATE_LEFT] = "left taken", [FATE_QUEUE] = "not queued",
    [FATE_DONE] = "done",       [FATE_ALREADY_INSTALLED] = "already_installed",
    [FATE_FAILED] = "failed",
};

/* ---- The round ---- */

static int is_sends_call(app_call_kind kind) {
    return kind == CALL_SENDS_CLAIM || kind == CALL_SENDS_QUEUE || kind == CALL_SENDS_REPORT;
}

static void end_round(skiff_app *app) {
    app_sends_round *round = app->sends.round;
    if (round != NULL) {
        free(round->sends);
        free(round->batch);
        free(round);
    }
    app->sends.round = NULL;
    app->dirty = 1;
}

void app_sends_connected(skiff_app *app) {
    app_sends *sends = &app->sends;
    sends->registered = 0;
    sends->unregistered = 0;
    sends->gone = 0;
    sends->next_claim_ms = 0;
    sends->last_error = SKIFF_OK;
}

void app_sends_reset(skiff_app *app) {
    app_sends_connected(app);
    app->sends.forbidden = 0;
    /* A round whose call still runs ends through app_sends_dropped() once it is done. */
    if (!is_sends_call(app->call.kind)) {
        end_round(app);
    }
}

void app_sends_dropped(skiff_app *app) {
    if (app->sends.round == NULL) {
        return;
    }
    if (app->log != NULL) {
        skiff_log_write(app->log, SKIFF_LOG_DEBUG, SKIFF_APP_LOG_TAG, "send: round dropped");
    }
    end_round(app);
}

/* ---- What runs next ---- */

app_sends_block app_sends_blocked(const skiff_app *app) {
    if (!app->has_server) {
        return SENDS_NOT_CONNECTED;
    }
    /* Sends came with save sync's release, 5.4 (a development build counts as newer): before it
     * RomM has neither the heartbeat's key nor the calls. */
    if (!skiff_romm_server_syncs_saves(&app->server)) {
        return SENDS_NEEDS_UPDATE;
    }
    if (!app->server.installs_enabled) {
        return SENDS_TURNED_OFF;
    }
    if (app->sends.forbidden || app->settings.device_id[0] == '\0' ||
        !skiff_config_has_scope(&app->settings, SKIFF_ROMM_SCOPE_DEVICES_WRITE)) {
        return SENDS_NEEDS_PAIRING;
    }
    return SENDS_OK;
}

/* Sends have something to do on this connection: claims (on), or remote_install false not sent
 * yet (off: never a claim). */
static int has_work(const skiff_app *app) {
    const app_sends *sends = &app->sends;
    return app_sends_blocked(app) == SENDS_OK && !sends->gone && sends->round == NULL &&
           (app->settings.from_romm || !sends->unregistered);
}

/* A sends call is wanted now: a claim that is due, or remote_install false still to send. */
static int wanted(const skiff_app *app) {
    return has_work(app) && app->net_joined && app_now(app) >= app->sends.next_claim_ms;
}

/* Sends wait for any other call and the library's requests, "Download all", and never run while
 * Skiff connects, pairs, shows a system dialog or changes the server. */
static int may_run(const skiff_app *app) {
    if (app_call_busy(app) || app->connect != CONNECT_DONE || app->request != REQUEST_NONE ||
        app->batch.step != BATCH_NONE || app->dialog != SKIFF_APP_DIALOG_NONE) {
        return 0;
    }
    if (app->screen == SKIFF_APP_SCREEN_CONFIRM && app->confirm == CONFIRM_SERVER_CHANGE) {
        return 0;
    }
    return app->screen != SKIFF_APP_SCREEN_PAIR && app->screen != SKIFF_APP_SCREEN_CONNECTING &&
           app->screen != SKIFF_APP_SCREEN_SERVER && app->screen != SKIFF_APP_SCREEN_STARTING;
}

int app_sends_busy(const skiff_app *app) { return app->sends.round != NULL || wanted(app); }

int app_sends_queueing(const skiff_app *app) { return app->call.kind == CALL_SENDS_QUEUE; }

int app_sends_taking_room(const skiff_app *app) {
    return app->sends.round != NULL && app->call.kind != CALL_SENDS_REPORT;
}

/* A claim (or remote_install, what) that did not happen: tried again after the interval, unless
 * RomM refused the token or no longer knows the device, the connection went (claims then wait for
 * the next join), or it was remote_install false (tried once per connection). Logged once while
 * the same error repeats. */
static void claim_failed(skiff_app *app, const char *what, skiff_err err) {
    app_sends *sends = &app->sends;
    sends->next_claim_ms = app_now(app) + SKIFF_APP_INSTALL_CLAIM_INTERVAL_MS;
    if (err == SKIFF_ERR_ROMM_SENDS_FORBIDDEN) {
        sends->forbidden = 1;
    } else if (err == SKIFF_ERR_ROMM_NOT_FOUND) {
        sends->gone = 1;
    }
    if (err != sends->last_error) {
        skiff_log_write(app->log, SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG, "send: %s: %s (%d)%s", what,
                        skiff_err_name(err), (int)err,
                        sends->forbidden            ? ", Settings offers Pair again"
                        : sends->gone               ? ", no claim until the next connection"
                        : !app->settings.from_romm  ? ", not again until the next connection"
                        : app_is_network_error(err) ? ", waiting for the Wi-Fi to be joined"
                                                    : "");
    }
    sends->last_error = err;
    app_network_failed(app, err);
    app->dirty = 1;
}

static void start_claim(skiff_app *app) {
    app_sends *sends = &app->sends;
    app_sends_round *round = calloc(1, sizeof *round);
    if (round == NULL) {
        claim_failed(app, "claim", SKIFF_ERR_NO_MEMORY);
        return;
    }
    snprintf(round->device_id, sizeof round->device_id, "%s", app->settings.device_id);
    round->on = app->settings.from_romm;
    /* On: once per connection, so RomM lists this PSP. Off: RomM is told to stop. */
    round->register_first = !round->on || !sends->registered;
    sends->round = round;
    app_call *call = &app->call;
    call->app = app;
    call->storage = app->config.storage;
    call->roots = app->config.roots;
    call->now_ms = app->env.now_ms;
    call->clock_ctx = app->env.ctx;
    app_call_start(app, CALL_SENDS_CLAIM);
}

/* Claims that wait for the Wi-Fi ask the platform, at most every APP_SENDS_JOIN_PROBE_MS, whether
 * something else joined it again (the download worker rejoins on its own, and a PSP left on
 * Downloads overnight makes no request of the player's). Skiff never joins for sends, and the
 * platform never counts a join made before the last suspend. */
static void notice_join(skiff_app *app) {
    app_sends *sends = &app->sends;
    if (app->net_joined || app->connect != CONNECT_DONE || app->dialog != SKIFF_APP_DIALOG_NONE ||
        !has_work(app) || app_now(app) < sends->next_probe_ms) {
        return;
    }
    sends->next_probe_ms = app_now(app) + APP_SENDS_JOIN_PROBE_MS;
    if (!app->env.net_online(app->env.ctx)) {
        return;
    }
    /* The claim keeps its interval: the access point may never have gone (RomM was out of reach),
     * and an outage is not asked about every second. */
    skiff_log_write(app->log, SKIFF_LOG_DEBUG, SKIFF_APP_LOG_TAG,
                    "send: the Wi-Fi is joined, claims go on at their interval");
    app->net_joined = 1;
}

void app_sends_update(skiff_app *app) {
    notice_join(app);
    if (wanted(app) && may_run(app)) {
        start_claim(app);
    }
}

/* ---- On the browsing thread: the claim ---- */

/* Each send's game details, in RomM's order (into the call's ROM, which no other call uses
 * meanwhile), then the free space. A lost connection stops it: the rest are not asked. */
static skiff_err fetch_games(app_call *call, app_sends_round *round) {
    int any = 0;
    for (size_t i = 0; i < round->claim.count; i++) {
        app_send *send = &round->sends[i];
        send->request = round->claim.requests[i];
        if (round->lost != SKIFF_OK) {
            continue;
        }
        send->asked = 1;
        send->rom_err = skiff_romm_get_rom(call->romm, send->request.rom_id, &call->rom);
        if (send->rom_err == SKIFF_OK) {
            send->rom = call->rom.summary;
            send->rom_files = call->rom.file_count;
            any = 1;
        } else if (app_is_suspend_error(send->rom_err)) {
            round->lost = send->rom_err;
        }
    }
    if (any) {
        round->free_err =
            skiff_storage_free_space(call->storage, call->roots.app, &round->free_bytes);
    }
    return round->lost;
}

skiff_err app_sends_claim_run(app_call *call) {
    app_sends_round *round = call->app->sends.round;
    const int64_t started = call->now_ms(call->clock_ctx);
    if (round->register_first) {
        const skiff_err err = skiff_romm_installs_register(call->romm, round->device_id, round->on);
        if (err != SKIFF_OK) {
            return err;
        }
        round->registered = 1;
    }
    if (!round->on) {
        return SKIFF_OK;
    }
    const skiff_err err = skiff_romm_installs_claim(call->romm, round->device_id, &round->claim);
    round->claim_ms = call->now_ms(call->clock_ctx) - started;
    if (err != SKIFF_OK) {
        return err;
    }
    if (round->claim.count > 0) {
        round->sends = calloc(round->claim.count, sizeof *round->sends);
        if (round->sends == NULL) {
            return SKIFF_ERR_NO_MEMORY;
        }
    }
    round->claimed = 1;
    return fetch_games(call, round);
}

/* ---- Sorting ---- */

static void settle(app_send *send, send_fate fate, const char *why, skiff_err why_err) {
    send->fate = fate;
    send->why = why;
    send->why_err = why_err;
}

static void fail_text(const skiff_app *app, app_send *send, const char *why, skiff_text_id text) {
    settle(send, FATE_FAILED, why, SKIFF_OK);
    snprintf(send->reason, sizeof send->reason, "%s", app_text(app, text));
}

/* Refused with err's sentence and code, as the player sees it on the PSP. */
static void fail_error(const skiff_app *app, app_send *send, const char *why, skiff_err err) {
    settle(send, FATE_FAILED, why, err);
    (void)skiff_error_line(app->config.language, err, send->reason, sizeof send->reason);
}

/* Why Skiff cannot install the game's file, as app_batch_take() found. */
static void refuse(const skiff_app *app, app_send *send) {
    const skiff_romm_rom_summary *rom = &send->rom;
    if (!rom->has_file) {
        fail_text(app, send, "no single file", SKIFF_TEXT_INSTALL_UNKNOWN_EXTENSION);
    } else if (rom->file.size == 0) {
        fail_text(app, send, "empty file", SKIFF_TEXT_INSTALL_EMPTY_FILE);
    } else if (rom->file.size > SKIFF_STORAGE_MAX_FILE_BYTES) {
        fail_error(app, send, "too large", SKIFF_ERR_STORAGE_FILE_TOO_LARGE);
    } else {
        const skiff_install_support support = skiff_install_check(app->installer, rom, &rom->file);
        fail_text(app, send,
                  support == SKIFF_INSTALL_UNUSABLE_NAME ? "unusable name" : "not a disc image",
                  app_install_support_text(support));
    }
}

/* The round's batch, made (emptied with the room there is now) for its first game. */
static app_batch *round_batch(skiff_app *app, app_sends_round *round) {
    if (round->batch == NULL) {
        round->batch = calloc(1, sizeof *round->batch);
        if (round->batch != NULL) {
            app_batch_reset(app, round->batch);
        }
    }
    return round->batch;
}

static size_t game_of(const app_batch *batch, uint64_t rom_id) {
    size_t game = 0;
    while (game < batch->count && batch->games[game].id != rom_id) {
        game++;
    }
    return game;
}

/* A send whose game RomM described: a PSP game of one file, then Download all's rules. */
static void sort_game(skiff_app *app, app_sends_round *round, app_send *send) {
    const skiff_romm_rom_summary *rom = &send->rom;
    /* RomM sends any platform's games but Windows ones to any device. */
    if (!app->has_platform || rom->platform_id != app->platform.id) {
        fail_error(app, send, "not a PSP game", SKIFF_ERR_ROMM_SEND_NOT_PSP);
        return;
    }
    /* RomM sends a game's update and DLC files with it, and Skiff installs one file. */
    if (send->rom_files > 1 || send->request.file_count > 1 || rom->multiple_files) {
        fail_text(app, send, "several files", SKIFF_TEXT_INSTALL_MULTIPLE_FILES);
        return;
    }
    app_batch *batch = round_batch(app, round);
    if (batch == NULL) {
        settle(send, FATE_LEFT, "sorting", SKIFF_ERR_NO_MEMORY);
        return;
    }
    switch (app_batch_take(app, batch, rom)) {
    case OUTCOME_TAKEN:
        settle(send, FATE_QUEUE, "chosen", SKIFF_OK);
        send->game = batch->count - 1;
        return;
    case OUTCOME_REPEATED:
        settle(send, FATE_QUEUE, "sent twice", SKIFF_OK);
        send->game = game_of(batch, rom->id);
        return;
    case OUTCOME_REFUSED:
        refuse(app, send);
        return;
    case OUTCOME_QUEUED: {
        const skiff_job *job = app_queue_job_for(app, rom->id);
        if (job != NULL && job->state == SKIFF_JOB_FAILED) {
            fail_text(app, send, "failed in Downloads", SKIFF_TEXT_SEND_FAILED_DOWNLOAD);
        } else {
            settle(send, FATE_DONE, "in Downloads", SKIFF_OK);
        }
        return;
    }
    case OUTCOME_INSTALLED:
        settle(send, FATE_ALREADY_INSTALLED, "installed", SKIFF_OK);
        return;
    case OUTCOME_CHANGED:
        /* Replacing an installed copy stays the player's choice, game by game. */
        fail_text(app, send, "changed in RomM", SKIFF_TEXT_SEND_CHANGED);
        return;
    case OUTCOME_OVER_QUEUE:
        fail_text(app, send, "Downloads full", SKIFF_TEXT_QUEUE_FULL);
        return;
    case OUTCOME_OVER_RECORDS:
        fail_text(app, send, "installed.json full", SKIFF_TEXT_INSTALLED_FULL);
        return;
    }
}

static void sort_send(skiff_app *app, app_sends_round *round, app_send *send) {
    if (!send->asked) {
        settle(send, FATE_LEFT, "connection lost", round->lost);
        return;
    }
    if (send->rom_err != SKIFF_OK) {
        if (app_is_suspend_error(send->rom_err)) {
            settle(send, FATE_LEFT, "details", send->rom_err);
        } else {
            fail_error(app, send, "details", send->rom_err);
        }
        return;
    }
    sort_game(app, round, send);
}

/* Every send sorted in RomM's order (the oldest first), the chosen games cut to the longest run
 * that fits the free space. */
static void sort_sends(skiff_app *app, app_sends_round *round) {
    for (size_t i = 0; i < round->claim.count; i++) {
        sort_send(app, round, &round->sends[i]);
    }
    app_batch *batch = round->batch;
    if (batch == NULL) {
        return;
    }
    app_batch_fit(app, batch, round->free_err, round->free_bytes);
    for (size_t i = 0; i < round->claim.count; i++) {
        app_send *send = &round->sends[i];
        if (send->fate == FATE_QUEUE && send->game >= batch->count) {
            fail_error(app, send, "no room", SKIFF_ERR_STORAGE_NO_SPACE);
        }
    }
}

/* ---- On the browsing thread: queueing, then reports ---- */

/* The queued sends' fates: added, left out by a full queue, or failed with their game's plan or
 * the queue's save. */
static void settle_queued(const skiff_app *app, app_sends_round *round) {
    const app_batch *batch = round->batch;
    /* A send is chosen for the queue only into the round's batch. */
    if (batch == NULL) {
        return;
    }
    unsigned char counted[APP_BATCH_MAX] = {0};
    for (size_t i = 0; i < round->claim.count; i++) {
        app_send *send = &round->sends[i];
        if (send->fate != FATE_QUEUE) {
            continue;
        }
        const size_t game = send->game;
        if (round->queue_err != SKIFF_OK) {
            fail_error(app, send, "queue", round->queue_err);
            continue;
        }
        if (!batch->planned[game]) {
            fail_error(app, send, "plan", batch->plan_errors[game]);
            continue;
        }
        /* add_many() queues the requests in order and stops at a full queue. */
        size_t request = 0;
        for (size_t g = 0; g < game; g++) {
            request += (size_t)(batch->planned[g] != 0);
        }
        if (request >= batch->added) {
            fail_text(app, send, "Downloads full", SKIFF_TEXT_QUEUE_FULL);
            continue;
        }
        settle(send, FATE_DONE, "queued", SKIFF_OK);
        send->queued = !counted[game];
        counted[game] = 1;
    }
}

static skiff_romm_install_status status_of(send_fate fate) {
    return fate == FATE_DONE                ? SKIFF_ROMM_INSTALL_DONE
           : fate == FATE_ALREADY_INSTALLED ? SKIFF_ROMM_INSTALL_ALREADY_INSTALLED
                                            : SKIFF_ROMM_INSTALL_FAILED;
}

/* Every settled send reported; a lost connection stops it (the rest stay taken in RomM). A report
 * RomM answers 404 or 409 (cancelled in the web UI, or already ended) is only logged. */
static skiff_err report_all(app_call *call, app_sends_round *round) {
    for (size_t i = 0; i < round->claim.count; i++) {
        app_send *send = &round->sends[i];
        if (send->fate == FATE_LEFT || send->fate == FATE_QUEUE) {
            continue;
        }
        send->report_err = skiff_romm_installs_report(
            call->romm, round->device_id, send->request.id, status_of(send->fate),
            send->fate == FATE_FAILED ? send->reason : NULL);
        send->reported = 1;
        if (app_is_suspend_error(send->report_err)) {
            return send->report_err;
        }
    }
    return SKIFF_OK;
}

skiff_err app_sends_queue_run(app_call *call) {
    skiff_app *app = call->app;
    app_sends_round *round = app->sends.round;
    app_batch *batch = round->batch;
    if (batch != NULL && batch->count > 0 && round->queue_err == SKIFF_OK) {
        round->queue_err = app_batch_queue_run(app, batch);
    }
    settle_queued(app, round);
    return SKIFF_OK;
}

skiff_err app_sends_report_run(app_call *call) { return report_all(call, call->app->sends.round); }

/* ---- Back on the screen's thread ---- */

static void log_send(skiff_app *app, const app_send *send) {
    char report[REPORT_NOTE_MAX] = "";
    int trouble = send->fate == FATE_LEFT;
    if (send->reported && send->report_err != SKIFF_OK) {
        snprintf(report, sizeof report, "; report: %s (%d)", skiff_err_name(send->report_err),
                 (int)send->report_err);
        /* 404 or 409: cancelled in RomM's web UI, or already ended. */
        trouble |= send->report_err != SKIFF_ERR_ROMM_NOT_FOUND;
    } else if (!send->reported && send->fate != FATE_LEFT) {
        snprintf(report, sizeof report, "; not reported, comes back on the next claim");
        trouble = 1;
    }
    skiff_log_write(app->log, trouble ? SKIFF_LOG_WARN : SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG,
                    "send %.*s: rom %llu -> %s (%s%s%s)%s", LOG_ID_CHARS, send->request.id,
                    (unsigned long long)send->request.rom_id, FATE_NAMES[send->fate],
                    send->why != NULL ? send->why : "", send->why_err != SKIFF_OK ? ": " : "",
                    send->why_err != SKIFF_OK ? skiff_err_name(send->why_err) : "", report);
}

static void note_round(skiff_app *app, size_t added, size_t refused) {
    char added_text[NUMBER_TEXT_MAX];
    char refused_text[NUMBER_TEXT_MAX];
    snprintf(added_text, sizeof added_text, "%zu", added);
    snprintf(refused_text, sizeof refused_text, "%zu", refused);
    const char *args[] = {added_text, refused_text};
    char text[SKIFF_TEXT_MAX];
    if (refused > 0) {
        app_format(app, SKIFF_TEXT_SENDS_ADDED_SOME, args, 2, text);
    } else {
        app_format(app, SKIFF_TEXT_SENDS_ADDED, args, 1, text);
    }
    app_note(app, text);
}

/* The round's end: each send logged, the header's note, and a lost connection noted (claims then
 * wait for the next join). */
static void finish(skiff_app *app, skiff_err err) {
    app_sends_round *round = app->sends.round;
    size_t added = 0;
    size_t refused = 0;
    for (size_t i = 0; i < round->claim.count; i++) {
        app_send *send = &round->sends[i];
        /* The queue call never ran (it could not start). */
        if (send->fate == FATE_QUEUE) {
            settle(send, FATE_LEFT, "not queued", err);
        }
        log_send(app, send);
        added += (size_t)send->queued;
        refused += (size_t)(send->fate == FATE_FAILED);
    }
    if (round->batch != NULL && round->queue_err != SKIFF_OK) {
        skiff_log_write(app->log, SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG, "send: queue: %s (%d)",
                        skiff_err_name(round->queue_err), (int)round->queue_err);
    }
    if (added > 0 || refused > 0) {
        note_round(app, added, refused);
    }
    if (added > 0) {
        app_queue_refresh(app);
    }
    end_round(app);
    app_network_failed(app, err);
}

void app_sends_claimed(skiff_app *app) {
    app_sends *sends = &app->sends;
    app_sends_round *round = sends->round;
    const skiff_err err = app->call.err;
    if (round == NULL) {
        return;
    }
    if (round->register_first && !round->on) {
        sends->unregistered = 1;
    }
    if (round->registered) {
        sends->registered = round->on;
        skiff_log_write(app->log, SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG,
                        "send: RomM %s games to this PSP",
                        round->on ? "may send" : "sends no more");
    }
    if (!round->claimed) {
        if (err != SKIFF_OK) {
            const int register_failed = round->register_first && !round->registered;
            claim_failed(app,
                         !register_failed ? "claim"
                         : round->on      ? "remote_install true"
                                          : "remote_install false",
                         err);
        }
        end_round(app);
        return;
    }
    sends->next_claim_ms = app_now(app) + SKIFF_APP_INSTALL_CLAIM_INTERVAL_MS;
    sends->last_error = SKIFF_OK;
    /* An empty claim, every interval: nothing to say. */
    if (round->claim.total == 0) {
        end_round(app);
        return;
    }
    skiff_log_write(
        app->log, round->claim.count > 0 ? SKIFF_LOG_INFO : SKIFF_LOG_DEBUG, SKIFF_APP_LOG_TAG,
        "send: claim of %zu in %lld ms: %zu kept, %zu with an id Skiff does not use",
        round->claim.total, (long long)round->claim_ms, round->claim.count, round->claim.dropped);
    sort_sends(app, round);
    int reports = 0;
    for (size_t i = 0; i < round->claim.count; i++) {
        reports |= round->sends[i].fate != FATE_LEFT;
    }
    if (!reports) {
        finish(app, err);
        return;
    }
    /* Downloads only run with the worker, as Download all's do. */
    if (round->batch != NULL && round->batch->count > 0) {
        round->queue_err = app_start_worker(app);
    }
    app->call.app = app;
    app_call_start(app, CALL_SENDS_QUEUE);
}

void app_sends_queued(skiff_app *app) {
    const app_sends_round *round = app->sends.round;
    if (round == NULL) {
        return;
    }
    /* The queue call could not start, or the connection went while the details were read: every
     * send waits for the next claim, which finds the queued ones in Downloads. */
    if (app->call.err != SKIFF_OK || round->lost != SKIFF_OK) {
        finish(app, app->call.err != SKIFF_OK ? app->call.err : round->lost);
        return;
    }
    app->call.app = app;
    app_call_start(app, CALL_SENDS_REPORT);
}

void app_sends_reported(skiff_app *app) {
    if (app->sends.round != NULL) {
        finish(app, app->call.err);
    }
}

/* ---- Settings ---- */

void app_sends_row(const skiff_app *app, char *label, size_t size, int *dim) {
    const app_sends_block block = app_sends_blocked(app);
    skiff_text_id state = app->settings.from_romm ? SKIFF_TEXT_SENDS_ON : SKIFF_TEXT_SENDS_OFF;
    switch (block) {
    case SENDS_NEEDS_UPDATE:
        state = SKIFF_TEXT_SENDS_NEEDS_UPDATE;
        break;
    case SENDS_TURNED_OFF:
        state = SKIFF_TEXT_SENDS_TURNED_OFF;
        break;
    case SENDS_NEEDS_PAIRING:
        state = SKIFF_TEXT_SENDS_PAIR_AGAIN;
        break;
    case SENDS_OK:
    case SENDS_NOT_CONNECTED:
        break;
    }
    *dim = block == SENDS_NEEDS_UPDATE || block == SENDS_TURNED_OFF;
    const char *args[] = {app_text(app, state)};
    char text[SKIFF_TEXT_MAX];
    app_format(app, SKIFF_TEXT_SENDS_ROW, args, 1, text);
    app_fit(app, text, SKIFF_APP_LABEL_WIDTH, label, size);
}

/* The player switches the setting: saved in config.ini; off tells RomM at once, on claims at
 * once. */
static void toggle(skiff_app *app) {
    const int on = !app->settings.from_romm;
    const skiff_err err =
        app_config_set(app, SKIFF_CONFIG_SECTION_DOWNLOADS, SKIFF_CONFIG_KEY_FROM_ROMM,
                       on ? SKIFF_CONFIG_ON : SKIFF_CONFIG_OFF, NULL);
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "games from RomM %s, saved: %s (%d)", on ? "on" : "off", skiff_err_name(err),
                    (int)err);
    if (err != SKIFF_OK) {
        app_show_error(app, err, NULL, MESSAGE_BACK, MESSAGE_NONE, SKIFF_TEXT_OK,
                       SKIFF_APP_SCREEN_SETTINGS);
        return;
    }
    app->sends.unregistered = 0;
    app->sends.next_claim_ms = 0;
    app->dirty = 1;
}

void app_sends_choose(skiff_app *app) {
    switch (app_sends_blocked(app)) {
    case SENDS_NEEDS_PAIRING:
        /* A new pairing replaces the browse client: not under a running request. */
        if (!app_call_busy(app)) {
            app_pair_begin(app);
        }
        return;
    case SENDS_NEEDS_UPDATE:
    case SENDS_TURNED_OFF:
        return;
    case SENDS_OK:
    case SENDS_NOT_CONNECTED:
        break;
    }
    toggle(app);
}
