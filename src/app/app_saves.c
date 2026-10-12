/*
 * Save sync on the screen's thread (app_saves.h): what runs next, the results of the browsing
 * thread's calls, and the Saves, Backups and choice screens' buttons. The launch check runs once
 * per connection, after the library's first page, and is followed by every transfer that needs no
 * choice (with a backup before every download); conflicts and deletions wait for the player on the
 * Saves screen or the game's details. Library pages and "Download all" always go first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_internal.h"

static const char *const SAVE_SCOPES[] = {
    SKIFF_ROMM_SCOPE_ASSETS_READ, SKIFF_ROMM_SCOPE_ASSETS_WRITE, SKIFF_ROMM_SCOPE_DEVICES_READ,
    SKIFF_ROMM_SCOPE_DEVICES_WRITE};

/* ---- State ---- */

skiff_err app_saves_create(app_saves **out) {
    app_saves *saves = calloc(1, sizeof *saves);
    skiff_err err = saves != NULL ? SKIFF_OK : SKIFF_ERR_NO_MEMORY;
    if (err == SKIFF_OK) {
        err = skiff_saves_baseline_create(&saves->baseline);
    }
    if (err == SKIFF_OK) {
        saves->shown = calloc(1, sizeof *saves->shown);
        saves->work = calloc(1, sizeof *saves->work);
        err = saves->shown != NULL && saves->work != NULL ? SKIFF_OK : SKIFF_ERR_NO_MEMORY;
    }
    if (err != SKIFF_OK) {
        app_saves_destroy(saves);
        saves = NULL;
    }
    if (saves != NULL) {
        skiff_ui_list_init(&saves->list, 0, APP_SAVES_ROWS);
        skiff_ui_list_init(&saves->backups_list, 0, APP_SAVES_BACKUP_ROWS);
    }
    *out = saves;
    return err;
}

void app_saves_destroy(app_saves *saves) {
    if (saves == NULL) {
        return;
    }
    skiff_saves_baseline_destroy(saves->baseline);
    free(saves->shown);
    free(saves->work);
    free(saves);
}

static skiff_err baseline_path(const skiff_app *app, char *out, size_t size) {
    return skiff_storage_resolve(&app->config.roots, SKIFF_SAVES_BASELINE_PATH, out, size);
}

skiff_err app_saves_start(skiff_app *app) {
    app_saves *saves = app->saves;
    char path[SKIFF_STORAGE_PATH_MAX];
    skiff_saves_baseline_load_result result = SKIFF_SAVES_BASELINE_NO_FILE;
    skiff_err err = baseline_path(app, path, sizeof path);
    if (err == SKIFF_OK) {
        err = skiff_saves_baseline_load(saves->baseline, app->config.storage, path, &result);
    }
    if (result == SKIFF_SAVES_BASELINE_DAMAGED) {
        skiff_log_write(app->log, SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG,
                        "saves.json could not be read; it is kept as saves.json%s",
                        SKIFF_SAVES_BASELINE_DAMAGED_SUFFIX);
    }
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG,
                    "saves.json: %zu game(s), %zu ROM(s): %s (%d)", saves->baseline->count,
                    saves->baseline->rom_count, skiff_err_name(err), (int)err);
    /* Before anything touches a save: a restore cut short is finished from its stage, or given
     * up with the backup it made. */
    skiff_saves_recovery recovery;
    memset(&recovery, 0, sizeof recovery);
    const skiff_err recovered =
        skiff_saves_recover(app->config.storage, &app->config.roots, &recovery);
    skiff_log_write(app->log, recovered == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_WARN,
                    SKIFF_APP_LOG_TAG, "save recovery: outcome %d, game %s, backup %u: %s (%d)",
                    (int)recovery.outcome,
                    recovery.save_target[0] != '\0' ? recovery.save_target : "-",
                    (unsigned)recovery.backup, skiff_err_name(recovered), (int)recovered);
    saves->recovered = recovered;
    return recovered;
}

int app_saves_has_scopes(const skiff_config *settings) {
    for (size_t i = 0; i < sizeof SAVE_SCOPES / sizeof SAVE_SCOPES[0]; i++) {
        if (!skiff_config_has_scope(settings, SAVE_SCOPES[i])) {
            return 0;
        }
    }
    return 1;
}

static int recovery_failed(skiff_err recovered) {
    return recovered != SKIFF_OK && recovered != SKIFF_ERR_STORAGE_SAVE_RESTORE_FINISHED &&
           recovered != SKIFF_ERR_STORAGE_SAVE_RESTORE_UNFINISHED;
}

app_saves_block app_saves_blocked(const skiff_app *app) {
    /* A restore cut short could not be finished: its game's folders may be half replaced, which a
     * check would read as a change to upload. Nothing syncs until a launch finishes it. */
    if (recovery_failed(app->saves->recovered)) {
        return SAVES_UNRECOVERED;
    }
    if (!app->has_server) {
        return SAVES_NOT_CONNECTED;
    }
    if (!skiff_romm_server_syncs_saves(&app->server)) {
        return SAVES_NEEDS_UPDATE;
    }
    if (app->saves->forbidden || !app_saves_has_scopes(&app->settings) ||
        app->settings.device_id[0] == '\0') {
        return SAVES_NEEDS_PAIRING;
    }
    return SAVES_OK;
}

static int is_saves_call(app_call_kind kind) {
    return kind == CALL_SAVES_CHECK || kind == CALL_SAVES_APPLY || kind == CALL_SAVES_COMPLETE ||
           kind == CALL_SAVES_BACKUPS;
}

static int any_recheck(const app_save_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->games[i].recheck) {
            return 1;
        }
    }
    return 0;
}

int app_saves_busy(const skiff_app *app) {
    const app_saves *saves = app->saves;
    /* A session waits to close while its choice is on screen: nothing runs meanwhile. */
    const int closing = saves->batch_session != 0 && app->screen != SKIFF_APP_SCREEN_SAVE_CHOICE;
    return is_saves_call(app->call.kind) || saves->want_check || saves->want_one ||
           saves->want_apply || saves->batch || closing || any_recheck(saves->shown);
}

/* Drops what waited to run, the list and what RomM refused: a new server or token. */
static void reset(skiff_app *app) {
    app_saves *saves = app->saves;
    saves->shown->count = 0;
    saves->shown->session_id = 0;
    saves->checked = 0;
    saves->check_error = SKIFF_OK;
    saves->forbidden = 0;
    saves->launch_done = 0;
    saves->want_check = 0;
    saves->want_one = 0;
    saves->want_apply = 0;
    saves->batch = 0;
    saves->batch_session = 0;
    saves->activity[0] = '\0';
    skiff_ui_list_init(&saves->list, 0, APP_SAVES_ROWS);
}

/* Removes saves.json and what a cut replacement of it left (its .new would otherwise come back as
 * the file on the next read). */
static skiff_err remove_baseline(skiff_app *app, const char *path) {
    static const char *const SUFFIXES[] = {SKIFF_STORAGE_PENDING_SUFFIX, SKIFF_STORAGE_DRAFT_SUFFIX,
                                           ""};
    skiff_err err = SKIFF_OK;
    for (size_t i = 0; err == SKIFF_OK && i < sizeof SUFFIXES / sizeof SUFFIXES[0]; i++) {
        char file[SKIFF_STORAGE_PATH_MAX + sizeof SKIFF_STORAGE_PENDING_SUFFIX];
        snprintf(file, sizeof file, "%s%s", path, SUFFIXES[i]);
        err = skiff_storage_remove(app->config.storage, file);
        err = err == SKIFF_ERR_STORAGE_NOT_FOUND ? SKIFF_OK : err;
    }
    return err;
}

skiff_err app_saves_forget(skiff_app *app, int server_changed) {
    app_saves *saves = app->saves;
    char path[SKIFF_STORAGE_PATH_MAX];
    skiff_err err = baseline_path(app, path, sizeof path);
    /* A new server's ROM ids mean other games, and a saves.json that could not be read is not one
     * to rewrite: either way the file goes whole. */
    const int whole = server_changed || saves->baseline->load_error != SKIFF_OK;
    skiff_saves_baseline *forgotten = NULL;
    if (err == SKIFF_OK && !whole) {
        err = skiff_saves_baseline_create(&forgotten);
    }
    if (err == SKIFF_OK && !whole) {
        *forgotten = *saves->baseline;
        skiff_saves_baseline_forget_all(forgotten);
        err = skiff_saves_baseline_save(forgotten, app->config.storage, path);
    } else if (err == SKIFF_OK) {
        err = remove_baseline(app, path);
    }
    skiff_saves_baseline_destroy(forgotten);
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "save baselines forgotten (%s): %s (%d)",
                    server_changed ? "new server" : "paired again", skiff_err_name(err), (int)err);
    if (err != SKIFF_OK) {
        return err;
    }
    reset(app);
    skiff_saves_baseline_forget_all(saves->baseline);
    if (whole) {
        skiff_saves_baseline_forget_roms(saves->baseline);
        saves->baseline->load_error = SKIFF_OK;
        saves->baseline->set_aside_damaged = 0;
    }
    return SKIFF_OK;
}

/* ---- The list ---- */

const char *app_saves_title(const app_save_game *game) {
    return game->title[0] != '\0' ? game->title : game->save_target;
}

static void fit_labels(skiff_app *app) {
    app_saves *saves = app->saves;
    for (size_t i = 0; i < saves->shown->count; i++) {
        app_fit(app, app_saves_title(&saves->shown->games[i]), SKIFF_APP_LABEL_WIDTH,
                saves->labels[i], sizeof saves->labels[i]);
    }
    skiff_ui_list_set_count(&saves->list, saves->shown->count);
}

static int same_game(const app_save_game *a, const app_save_game *b) {
    return a->save_target[0] != '\0' ? strcmp(a->save_target, b->save_target) == 0
                                     : b->save_target[0] == '\0' && a->rom_id == b->rom_id;
}

static app_save_game *shown_game(skiff_app *app, const app_save_game *game) {
    app_save_list *list = app->saves->shown;
    for (size_t i = 0; i < list->count; i++) {
        if (same_game(&list->games[i], game)) {
            return &list->games[i];
        }
    }
    return NULL;
}

/* The game's row as a call left it; added when the list did not have it. */
static void put_game(skiff_app *app, const app_save_game *game) {
    app_save_list *list = app->saves->shown;
    app_save_game *row = shown_game(app, game);
    if (row == NULL && list->count < APP_SAVES_GAMES_MAX) {
        row = &list->games[list->count++];
    }
    if (row != NULL) {
        *row = *game;
        row->recheck = 0;
    }
    fit_labels(app);
}

const app_save_game *app_saves_game_of_rom(const skiff_app *app, uint64_t rom_id) {
    const app_save_list *list = app->saves->shown;
    for (size_t i = 0; i < list->count; i++) {
        if (list->games[i].rom_id == rom_id && rom_id != 0) {
            return &list->games[i];
        }
    }
    return NULL;
}

static int transfers(const app_save_game *game) {
    return game->problem == SAVE_PROBLEM_NONE && game->decided &&
           (game->decision.action == SKIFF_SAVES_UPLOAD ||
            game->decision.action == SKIFF_SAVES_DOWNLOAD);
}

int app_saves_needs_player(const app_save_game *game) {
    if (game->problem == SAVE_PROBLEM_ERROR) {
        return 1;
    }
    return game->problem == SAVE_PROBLEM_NONE && game->decided &&
           (game->decision.action == SKIFF_SAVES_ASK_CONFLICT ||
            game->decision.action == SKIFF_SAVES_ASK_DELETED_LOCAL ||
            game->decision.action == SKIFF_SAVES_ASK_DELETED_SERVER);
}

/* ---- Starting calls ---- */

static void prepare(skiff_app *app) {
    app_call *call = &app->call;
    app_saves_call *saves = &call->saves;
    call->storage = app->config.storage;
    call->roots = app->config.roots;
    call->now_ms = app->env.now_ms;
    call->clock_ctx = app->env.ctx;
    saves->baseline = app->saves->baseline;
    saves->log = app->log;
    saves->now_utc_ms = 0;
    if (app->config.clock == NULL ||
        app->config.clock(app->config.clock_ctx, &saves->now_utc_ms) == 0) {
        saves->now_utc_ms = 0;
    }
    snprintf(saves->device_id, sizeof saves->device_id, "%s", app->settings.device_id);
    saves->stale = 0;
    saves->baseline_error = SKIFF_OK;
}

static void set_activity(skiff_app *app, skiff_text_id id, const char *arg) {
    const char *args[] = {arg};
    app_format(app, id, args, arg != NULL ? 1 : 0, app->saves->activity);
    app->dirty = 1;
}

static void start_check(skiff_app *app) {
    app_saves *saves = app->saves;
    app_saves_call *call = &app->call.saves;
    prepare(app);
    call->op = SAVES_OP_CHECK;
    call->list = saves->work;
    call->force_hash = saves->want_force;
    call->has_platform = app->has_platform;
    call->platform_id = app->platform.id;
    app_lock_manifest(app);
    call->installed_count = 0;
    for (size_t i = 0; i < app->manifest->count && i < SKIFF_INSTALL_RECORDS_MAX; i++) {
        call->installed[call->installed_count++] = app->manifest->records[i].rom_id;
    }
    app_unlock_manifest(app);
    saves->want_check = 0;
    saves->want_force = 0;
    set_activity(app, SKIFF_TEXT_SAVES_CHECKING, NULL);
    app_call_start(app, CALL_SAVES_CHECK);
}

static void start_one(skiff_app *app, const app_save_game *game, app_saves_origin origin) {
    app->saves->running_origin = origin;
    prepare(app);
    app->call.saves.op = SAVES_OP_CHECK_ONE;
    app->call.saves.game = *game;
    set_activity(app, SKIFF_TEXT_SAVES_CHECKING, NULL);
    app_call_start(app, CALL_SAVES_CHECK);
}

static void start_apply(skiff_app *app, const app_save_game *game, app_saves_op op, uint32_t backup,
                        app_saves_origin origin) {
    app->saves->running_origin = origin;
    prepare(app);
    app->call.saves.op = op;
    app->call.saves.game = *game;
    app->call.saves.backup = backup;
    set_activity(app, SKIFF_TEXT_SAVES_SYNCING, app_saves_title(game));
    app_call_start(app, CALL_SAVES_APPLY);
}

static void start_complete(skiff_app *app) {
    app_saves *saves = app->saves;
    prepare(app);
    app->call.saves.session_id = saves->batch_session;
    app->call.saves.completed = saves->batch_completed;
    app->call.saves.failed = saves->batch_failed;
    saves->batch_session = 0;
    saves->batch_completed = 0;
    saves->batch_failed = 0;
    app_call_start(app, CALL_SAVES_COMPLETE);
}

/* ---- What runs next ---- */

/* Save calls wait for the library's requests and "Download all", and never run while Skiff
 * connects, pairs or shows a system dialog. */
static int may_run(const skiff_app *app) {
    if (app_call_busy(app) || app->connect != CONNECT_DONE || app->request != REQUEST_NONE ||
        app->batch.step != BATCH_NONE || app->dialog != SKIFF_APP_DIALOG_NONE) {
        return 0;
    }
    /* A new server forgets the baselines a call would read and write. */
    if (app->screen == SKIFF_APP_SCREEN_CONFIRM && app->confirm == CONFIRM_SERVER_CHANGE) {
        return 0;
    }
    return app->screen != SKIFF_APP_SCREEN_PAIR && app->screen != SKIFF_APP_SCREEN_CONNECTING &&
           app->screen != SKIFF_APP_SCREEN_SERVER && app->screen != SKIFF_APP_SCREEN_STARTING;
}

static app_save_game *game_to_recheck(skiff_app *app) {
    app_save_list *list = app->saves->shown;
    for (size_t i = 0; i < list->count; i++) {
        if (list->games[i].recheck) {
            return &list->games[i];
        }
    }
    return NULL;
}

/* The batch's next transfer, or its end: the session closed, then the header's summary. */
static void batch_step(skiff_app *app) {
    app_saves *saves = app->saves;
    app_save_list *list = saves->shown;
    for (; saves->batch_next < list->count; saves->batch_next++) {
        const app_save_game *game = &list->games[saves->batch_next];
        if (transfers(game)) {
            saves->batch_next++;
            start_apply(app, game,
                        game->decision.action == SKIFF_SAVES_UPLOAD ? SAVES_OP_UPLOAD
                                                                    : SAVES_OP_DOWNLOAD,
                        0, SAVES_FROM_NONE);
            return;
        }
    }
    const uint32_t synced = saves->batch_completed;
    size_t waiting = 0;
    for (size_t i = 0; i < list->count; i++) {
        waiting += (size_t)(app_saves_needs_player(&list->games[i]) || list->games[i].recheck);
    }
    saves->batch = 0;
    saves->activity[0] = '\0';
    app->dirty = 1;
    skiff_log_write(app->log, SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG,
                    "saves: %u synced, %u failed, %zu need the player", (unsigned)synced,
                    (unsigned)saves->batch_failed, waiting);
    if (synced > 0 || waiting > 0) {
        char done[16];
        char ask[16];
        snprintf(done, sizeof done, "%u", (unsigned)synced);
        snprintf(ask, sizeof ask, "%zu", waiting);
        const char *args[] = {done, ask};
        char text[SKIFF_TEXT_MAX];
        app_format(app, SKIFF_TEXT_SAVES_SUMMARY, args, 2, text);
        app_note(app, text);
    }
}

void app_saves_update(skiff_app *app) {
    app_saves *saves = app->saves;
    if (!saves->launch_done && app->connect == CONNECT_DONE && app->has_server) {
        saves->launch_done = 1;
        const app_saves_block block = app_saves_blocked(app);
        skiff_log_write(app->log, SKIFF_LOG_DEBUG, SKIFF_APP_LOG_TAG, "saves at launch: %d",
                        (int)block);
        if (block == SAVES_OK) {
            saves->want_check = 1;
        } else if (block == SAVES_NEEDS_PAIRING) {
            app_note(app, app_text(app, SKIFF_TEXT_SAVES_PAIR_NOTE));
        }
    }
    if (!may_run(app)) {
        return;
    }
    if (app_saves_blocked(app) != SAVES_OK) {
        saves->want_check = 0;
        saves->want_one = 0;
        saves->want_apply = 0;
        saves->batch = 0;
        saves->batch_session = 0;
        for (size_t i = 0; i < saves->shown->count; i++) {
            saves->shown->games[i].recheck = 0;
        }
        saves->activity[0] = '\0';
        return;
    }
    if (saves->want_apply) {
        saves->want_apply = 0;
        start_apply(app, &saves->apply_game, saves->apply_op, saves->backups_pick,
                    saves->apply_origin);
        saves->apply_origin = SAVES_FROM_NONE;
        return;
    }
    if (saves->batch) {
        batch_step(app);
        return;
    }
    if (saves->batch_session != 0 && app->screen != SKIFF_APP_SCREEN_SAVE_CHOICE) {
        start_complete(app);
        return;
    }
    if (saves->want_one) {
        saves->want_one = 0;
        start_one(app, &saves->one, saves->one_origin);
        saves->one_origin = SAVES_FROM_NONE;
        return;
    }
    const app_save_game *recheck = game_to_recheck(app);
    if (recheck != NULL) {
        start_one(app, recheck, SAVES_FROM_NONE);
        return;
    }
    if (saves->want_check) {
        start_check(app);
    }
}

/* ---- Results ---- */

/* RomM refused the token a save call: Pair again, as without the scopes. */
static void refused(skiff_app *app) {
    app_saves *saves = app->saves;
    saves->forbidden = 1;
    saves->batch = 0;
    saves->batch_session = 0;
    saves->activity[0] = '\0';
    app->dirty = 1;
}

/* Where a player's sync goes back to. */
static skiff_app_screen origin_screen(app_saves_origin origin) {
    return origin == SAVES_FROM_DETAILS ? SKIFF_APP_SCREEN_DETAILS : SKIFF_APP_SCREEN_SAVES;
}

/* The player is still where they started the sync: its game's details, or the Saves screens. */
static int still_there(const skiff_app *app, app_saves_origin origin, const app_save_game *game) {
    if (origin == SAVES_FROM_DETAILS) {
        return app->screen == SKIFF_APP_SCREEN_DETAILS && app->rom.summary.id == game->rom_id;
    }
    return origin == SAVES_FROM_LIST &&
           (app->screen == SKIFF_APP_SCREEN_SAVES || app->screen == SKIFF_APP_SCREEN_SAVE_BACKUPS);
}

static void show_failure(skiff_app *app, skiff_err err, app_saves_origin origin) {
    app_show_error(app, err, NULL, MESSAGE_BACK, MESSAGE_NONE, SKIFF_TEXT_OK,
                   origin_screen(origin));
}

static void note_game(skiff_app *app, skiff_text_id id, const app_save_game *game) {
    const char *args[] = {app_saves_title(game)};
    char text[SKIFF_TEXT_MAX];
    app_format(app, id, args, 1, text);
    app_note(app, text);
}

/* Why a game cannot be synced, as a sentence (into out, SKIFF_TEXT_MAX). */
void app_saves_problem_text(const skiff_app *app, const app_save_game *game, char *out) {
    switch (game->problem) {
    case SAVE_PROBLEM_NO_ID:
        app_format(app, SKIFF_TEXT_SAVE_NO_ID, NULL, 0, out);
        return;
    case SAVE_PROBLEM_UNKNOWN_GAME:
        app_format(app, SKIFF_TEXT_SAVE_UNKNOWN_GAME, NULL, 0, out);
        return;
    case SAVE_PROBLEM_SEVERAL_GAMES:
        app_format(app, SKIFF_TEXT_SAVE_SEVERAL_GAMES, NULL, 0, out);
        return;
    case SAVE_PROBLEM_ERROR: {
        char line[SKIFF_TEXT_MAX];
        (void)skiff_error_line(app->config.language, game->error, line, sizeof line);
        const char *args[] = {line};
        app_format(app, SKIFF_TEXT_SAVE_ERROR, args, 1, out);
        return;
    }
    case SAVE_PROBLEM_NONE:
        break;
    }
    out[0] = '\0';
}

static void open_choice(skiff_app *app, const app_save_game *game, app_saves_origin origin) {
    app->saves->choice = *game;
    app->saves->choice_origin = origin;
    app_set_screen(app, SKIFF_APP_SCREEN_SAVE_CHOICE);
}

/* After a player's check of one game: transfer it, ask, or say why not. */
static void continue_one(skiff_app *app, const app_save_game *game, app_saves_origin origin) {
    app_saves *saves = app->saves;
    if (game->problem != SAVE_PROBLEM_NONE) {
        char text[SKIFF_TEXT_MAX];
        app_saves_problem_text(app, game, text);
        app_show_text(app, SKIFF_TEXT_TITLE_NOTICE, text, MESSAGE_BACK, origin_screen(origin));
        return;
    }
    switch (game->decision.action) {
    case SKIFF_SAVES_UPLOAD:
    case SKIFF_SAVES_DOWNLOAD:
        saves->apply_game = *game;
        saves->apply_op =
            game->decision.action == SKIFF_SAVES_UPLOAD ? SAVES_OP_UPLOAD : SAVES_OP_DOWNLOAD;
        saves->apply_origin = origin;
        saves->want_apply = 1;
        return;
    case SKIFF_SAVES_ASK_CONFLICT:
    case SKIFF_SAVES_ASK_DELETED_LOCAL:
    case SKIFF_SAVES_ASK_DELETED_SERVER:
        open_choice(app, game, origin);
        return;
    case SKIFF_SAVES_IN_SYNC:
        break;
    }
    note_game(app, SKIFF_TEXT_SAVE_IN_SYNC_NOTE, game);
}

static void one_checked(skiff_app *app) {
    app_saves *saves = app->saves;
    const skiff_err err = app->call.err;
    app_save_game game = app->call.saves.game;
    const app_saves_origin origin = saves->running_origin;
    saves->running_origin = SAVES_FROM_NONE;
    saves->activity[0] = '\0';
    if (app->call.saves.session_id != 0 && err == SKIFF_OK) {
        saves->batch_session = app->call.saves.session_id;
    }
    if (err == SKIFF_ERR_ROMM_FORBIDDEN) {
        refused(app);
        if (origin != SAVES_FROM_NONE) {
            show_failure(app, SKIFF_ERR_ROMM_SAVES_FORBIDDEN, origin);
        }
        return;
    }
    if (err != SKIFF_OK) {
        app_network_failed(app, err);
        game.problem = SAVE_PROBLEM_ERROR;
        game.error = err;
        put_game(app, &game);
        if (origin != SAVES_FROM_NONE) {
            show_failure(app, err, origin);
        }
        return;
    }
    put_game(app, &game);
    /* The player left what the sync was for: the row is updated, nothing more. */
    if (origin == SAVES_FROM_NONE || app->screen != origin_screen(origin) ||
        (origin == SAVES_FROM_DETAILS && app->rom.summary.id != game.rom_id)) {
        return;
    }
    continue_one(app, &game, origin);
}

void app_saves_checked(skiff_app *app) {
    app_saves *saves = app->saves;
    if (app->call.saves.op == SAVES_OP_CHECK_ONE) {
        one_checked(app);
        return;
    }
    const skiff_err err = app->call.err;
    saves->activity[0] = '\0';
    app->dirty = 1;
    if (err == SKIFF_ERR_ROMM_FORBIDDEN) {
        refused(app);
        return;
    }
    if (err != SKIFF_OK) {
        app_network_failed(app, err);
        saves->check_error = err;
        return;
    }
    app_save_list *shown = saves->shown;
    saves->shown = saves->work;
    saves->work = shown;
    saves->checked = 1;
    saves->check_error = SKIFF_OK;
    fit_labels(app);
    saves->batch = 1;
    saves->batch_next = 0;
    saves->batch_completed = 0;
    saves->batch_failed = 0;
    saves->batch_session = saves->shown->session_id;
}

void app_saves_applied(skiff_app *app) {
    app_saves *saves = app->saves;
    const skiff_err err = app->call.err;
    const app_saves_call *call = &app->call.saves;
    const app_saves_origin origin = saves->running_origin;
    saves->running_origin = SAVES_FROM_NONE;
    saves->activity[0] = '\0';
    app->dirty = 1;
    app_save_game game = call->game;
    if (call->baseline_error != SKIFF_OK) {
        skiff_log_write(app->log, SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG,
                        "saves.json not saved: %s (%d)", skiff_err_name(call->baseline_error),
                        (int)call->baseline_error);
    }
    if (recovery_failed(call->recovery)) {
        saves->recovered = call->recovery;
    }
    /* Left meanwhile: the row and a note say how it went, the player's screen stays. */
    const int there = origin != SAVES_FROM_NONE && still_there(app, origin, &game);
    const int again = err == SKIFF_ERR_ROMM_SAVE_CONFLICT || (err == SKIFF_OK && call->stale);
    if (err == SKIFF_OK && !call->stale) {
        saves->batch_completed++;
    } else if (!again) {
        saves->batch_failed++;
    }
    if (err == SKIFF_ERR_ROMM_FORBIDDEN) {
        refused(app);
        if (there) {
            show_failure(app, SKIFF_ERR_ROMM_SAVES_FORBIDDEN, origin);
        }
        return;
    }
    if (call->op == SAVES_OP_RESTORE_BACKUP) {
        app_save_game *row = shown_game(app, &game);
        if (err == SKIFF_OK && row != NULL) {
            row->recheck = 1;
        }
        if (err == SKIFF_OK) {
            app_note(app, app_text(app, SKIFF_TEXT_BACKUP_RESTORED));
            if (there) {
                app_set_screen(app, SKIFF_APP_SCREEN_SAVES);
            }
        } else if (there) {
            show_failure(app, err, SAVES_FROM_LIST);
        }
        return;
    }
    if (again) {
        /* RomM's save moved on (409), or this PSP's did, since the check: compare again. A player's
         * sync goes on from there once; a batch leaves it for the player. */
        skiff_log_write(app->log, SKIFF_LOG_INFO, SKIFF_APP_LOG_TAG, "saves: %s changed: %s (%d)",
                        game.save_target, skiff_err_name(err), (int)err);
        if (origin != SAVES_FROM_NONE && !saves->one_retried) {
            saves->one_retried = 1;
            saves->one = game;
            saves->one_origin = origin;
            saves->want_one = 1;
        } else {
            app_save_game *row = shown_game(app, &game);
            if (row != NULL) {
                row->recheck = 1;
            }
        }
        return;
    }
    if (err != SKIFF_OK) {
        app_network_failed(app, err);
        game.problem = SAVE_PROBLEM_ERROR;
        game.error = err;
        put_game(app, &game);
        /* Lost connection: the rest of the batch waits for the next check. */
        if (app_is_network_error(err)) {
            saves->batch_next = saves->shown->count;
        }
        if (there) {
            show_failure(app, err, origin);
        }
        return;
    }
    put_game(app, &game);
    if (origin != SAVES_FROM_NONE) {
        note_game(app, SKIFF_TEXT_SAVE_SYNCED_NOTE, &game);
    }
}

void app_saves_completed(skiff_app *app) {
    /* Logged on the browsing thread; a session left open is RomM's to expire. */
    app_network_failed(app, app->call.err);
}

void app_saves_backups_listed(skiff_app *app) {
    app_saves *saves = app->saves;
    const skiff_err err = app->call.err;
    saves->backups_error = err;
    saves->backups_known = 1;
    saves->backups = err == SKIFF_OK ? app->call.saves.backups : saves->backups;
    if (err != SKIFF_OK) {
        saves->backups.count = 0;
    }
    skiff_ui_list_init(&saves->backups_list, saves->backups.count, APP_SAVES_BACKUP_ROWS);
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_DEBUG : SKIFF_LOG_WARN, SKIFF_APP_LOG_TAG,
                    "backups of %s: %zu: %s (%d)", saves->backups_game.save_target,
                    saves->backups.count, skiff_err_name(err), (int)err);
    app->dirty = 1;
}

/* ---- The player's syncs ---- */

static void sync_one(skiff_app *app, const app_save_game *game, app_saves_origin origin) {
    app_saves *saves = app->saves;
    saves->one = *game;
    saves->one_origin = origin;
    saves->one_retried = 0;
    saves->want_one = 1;
    app->dirty = 1;
}

static void sync_all(skiff_app *app) {
    app->saves->want_check = 1;
    app->saves->want_force = 1;
    app->dirty = 1;
}

void app_saves_open(skiff_app *app) {
    skiff_ui_list_set_count(&app->saves->list, app->saves->shown->count);
    app_set_screen(app, SKIFF_APP_SCREEN_SAVES);
}

static void open_backups(skiff_app *app, const app_save_game *game) {
    app_saves *saves = app->saves;
    saves->backups_game = *game;
    saves->backups_known = 0;
    saves->backups.count = 0;
    saves->backups_error = SKIFF_OK;
    skiff_ui_list_init(&saves->backups_list, 0, APP_SAVES_BACKUP_ROWS);
    app_set_screen(app, SKIFF_APP_SCREEN_SAVE_BACKUPS);
    prepare(app);
    app->call.saves.game = *game;
    app_call_start(app, CALL_SAVES_BACKUPS);
}

void app_saves_screen_update(skiff_app *app, unsigned actions) {
    app_saves *saves = app->saves;
    if (actions & SKIFF_UI_ACTION_BACK) {
        app_set_screen(app, SKIFF_APP_SCREEN_SETTINGS);
        return;
    }
    const app_saves_block block = app_saves_blocked(app);
    const int confirm = (actions & SKIFF_UI_ACTION_CONFIRM) != 0;
    if (block == SAVES_NEEDS_PAIRING) {
        /* A new pairing replaces the browse client: not under a running request. */
        if (confirm && !app_call_busy(app)) {
            app_pair_begin(app);
        }
        return;
    }
    if (block != SAVES_OK) {
        return;
    }
    if (skiff_ui_list_apply(&saves->list, actions)) {
        app->dirty = 1;
    }
    if (app_saves_busy(app) || app_call_busy(app)) {
        return;
    }
    const app_save_list *list = saves->shown;
    const app_save_game *game = list->count > 0 && saves->list.selected < list->count
                                    ? &list->games[saves->list.selected]
                                    : NULL;
    if ((actions & SKIFF_UI_ACTION_EXTRA) || (confirm && game == NULL)) {
        sync_all(app);
        return;
    }
    /* A failed game syncs again; one no ROM is known for says why it cannot. */
    if (confirm && game->problem != SAVE_PROBLEM_NONE &&
        (game->problem != SAVE_PROBLEM_ERROR || game->rom_id == 0)) {
        char text[SKIFF_TEXT_MAX];
        app_saves_problem_text(app, game, text);
        app_show_text(app, SKIFF_TEXT_TITLE_NOTICE, text, MESSAGE_BACK, SKIFF_APP_SCREEN_SAVES);
        return;
    }
    if (confirm) {
        sync_one(app, game, SAVES_FROM_LIST);
        return;
    }
    if ((actions & SKIFF_UI_ACTION_MENU) && game != NULL && game->save_target[0] != '\0') {
        open_backups(app, game);
    }
}

void app_saves_backups_update(skiff_app *app, unsigned actions) {
    app_saves *saves = app->saves;
    if (actions & SKIFF_UI_ACTION_BACK) {
        app_set_screen(app, SKIFF_APP_SCREEN_SAVES);
        return;
    }
    if (skiff_ui_list_apply(&saves->backups_list, actions)) {
        app->dirty = 1;
    }
    if ((actions & SKIFF_UI_ACTION_CONFIRM) && saves->backups_known && saves->backups.count > 0 &&
        !app_saves_busy(app) && !app_call_busy(app)) {
        saves->backups_pick = saves->backups.backups[saves->backups_list.selected].number;
        app_confirm(app, SKIFF_TEXT_CONFIRM_RESTORE_BACKUP, CONFIRM_RESTORE_BACKUP, 0);
    }
}

void app_saves_restore_confirmed(skiff_app *app) {
    app_saves *saves = app->saves;
    saves->apply_game = saves->backups_game;
    saves->apply_op = SAVES_OP_RESTORE_BACKUP;
    saves->apply_origin = SAVES_FROM_LIST;
    saves->want_apply = 1;
    app_set_screen(app, SKIFF_APP_SCREEN_SAVE_BACKUPS);
}

void app_saves_choice_update(skiff_app *app, unsigned actions) {
    app_saves *saves = app->saves;
    const app_saves_origin origin = saves->choice_origin;
    if (actions & SKIFF_UI_ACTION_BACK) {
        app_set_screen(app, origin_screen(origin));
        return;
    }
    const int deleted_here = saves->choice.decision.action == SKIFF_SAVES_ASK_DELETED_LOCAL;
    app_saves_op op = SAVES_OP_CHECK;
    if (actions & SKIFF_UI_ACTION_CONFIRM) {
        op = deleted_here ? SAVES_OP_DOWNLOAD : SAVES_OP_UPLOAD_OVERWRITE;
    } else if (actions & SKIFF_UI_ACTION_EXTRA) {
        op = deleted_here ? SAVES_OP_LEAVE_DELETED : SAVES_OP_DOWNLOAD;
    } else {
        return;
    }
    saves->apply_game = saves->choice;
    saves->apply_op = op;
    saves->apply_origin = origin;
    saves->want_apply = 1;
    app_set_screen(app, origin_screen(origin));
}

/* ---- The game's details ---- */

int app_saves_rom_game(const skiff_app *app, app_save_game *out) {
    const skiff_romm_rom_summary *rom = &app->rom.summary;
    memset(out, 0, sizeof *out);
    if (!app->has_rom || rom->save_layout != SKIFF_ROMM_SAVE_LAYOUT_FOLDER_PREFIX ||
        !skiff_saves_target_valid(rom->save_target)) {
        return 0;
    }
    const app_save_game *known = app_saves_game_of_rom(app, rom->id);
    if (known != NULL && strcmp(known->save_target, rom->save_target) == 0) {
        *out = *known;
    }
    /* Valid, so at most SKIFF_SAVES_TARGET_MAX characters. */
    snprintf(out->save_target, sizeof out->save_target, "%.*s", SKIFF_SAVES_TARGET_MAX,
             rom->save_target);
    snprintf(out->title, sizeof out->title, "%s", app_rom_name(rom));
    out->rom_id = rom->id;
    out->problem = SAVE_PROBLEM_NONE;
    return 1;
}

void app_saves_rom_loaded(skiff_app *app) {
    if (app_saves_blocked(app) != SAVES_OK || app_call_busy(app)) {
        return;
    }
    char target[SKIFF_SAVES_TARGET_SIZE] = "";
    const skiff_romm_rom_summary *rom = &app->rom.summary;
    if (rom->save_layout == SKIFF_ROMM_SAVE_LAYOUT_FOLDER_PREFIX &&
        skiff_saves_target_valid(rom->save_target)) {
        snprintf(target, sizeof target, "%.*s", SKIFF_SAVES_TARGET_MAX, rom->save_target);
    }
    /* Kept in memory; the next save call writes saves.json with it. */
    (void)skiff_saves_baseline_remember_rom(app->saves->baseline, rom->id, target, NULL);
}

int app_saves_details_offered(const skiff_app *app) {
    app_save_game game;
    return app_saves_blocked(app) == SAVES_OK && app_saves_rom_game(app, &game);
}

void app_saves_details_sync(skiff_app *app) {
    app_save_game game;
    if (app_saves_busy(app) || !app_saves_details_offered(app) || !app_saves_rom_game(app, &game)) {
        return;
    }
    sync_one(app, &game, SAVES_FROM_DETAILS);
}

skiff_err skiff_app_open_saves(skiff_app *app) {
    if (app == NULL || app->screen == SKIFF_APP_SCREEN_STARTING ||
        app->dialog != SKIFF_APP_DIALOG_NONE) {
        return SKIFF_ERR_INVALID_ARG;
    }
    app_saves_open(app);
    return SKIFF_OK;
}
