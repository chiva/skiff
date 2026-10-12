/*
 * What the save sync screens show (app_saves.h): the Saves list with the selected game's state
 * under it, a game's backups, the choice between two saves, and the details screen's "Save:" line.
 * Dates are shown as the Memory Stick or RomM wrote them, day, month and time, without a zone.
 */
#include <stdio.h>
#include <string.h>

#include "app_internal.h"

/* "YYYY-MM-DD?HH:MM", the start of RomM's timestamps and of skiff_log_format_timestamp()'s. */
enum {
    ISO_MONTH_AT = 5,
    ISO_DAY_AT = 8,
    ISO_HOUR_AT = 11,
    ISO_MINUTE_AT = 14,
    ISO_DATE_TIME_LENGTH = 16,
    MONTHS_IN_YEAR = 12,
    MONTH_NAME_MAX = 16,
    DECIMAL = 10,
};

typedef struct save_date {
    int month;
    int day;
    int hour;
    int minute;
} save_date;

/* Two digits at text + at, into *out; 0 when they are not digits. */
static int two_digits(const char *text, size_t at, int *out) {
    const char tens = text[at];
    const char ones = text[at + 1];
    if (tens < '0' || tens > '9' || ones < '0' || ones > '9') {
        return 0;
    }
    *out = (tens - '0') * DECIMAL + (ones - '0');
    return 1;
}

static int parse_iso(const char *text, save_date *out) {
    return strlen(text) >= ISO_DATE_TIME_LENGTH && two_digits(text, ISO_MONTH_AT, &out->month) &&
           two_digits(text, ISO_DAY_AT, &out->day) && two_digits(text, ISO_HOUR_AT, &out->hour) &&
           two_digits(text, ISO_MINUTE_AT, &out->minute) && out->month >= 1 &&
           out->month <= MONTHS_IN_YEAR;
}

/* A time in ms since 1970 as its date; 0 for none. */
static int date_of_ms(int64_t unix_ms, save_date *out) {
    char text[SKIFF_LOG_TIMESTAMP_MAX];
    return unix_ms > 0 && skiff_log_format_timestamp(unix_ms, text, sizeof text) == SKIFF_OK &&
           parse_iso(text, out);
}

/* The month's short name from the language's list of twelve. */
static void month_name(const skiff_app *app, int month, char *out, size_t size) {
    const char *names = app_text(app, SKIFF_TEXT_MONTHS);
    out[0] = '\0';
    for (int i = 1; i < month && names != NULL; i++) {
        names = strchr(names, ' ');
        names = names != NULL ? names + 1 : NULL;
    }
    if (names != NULL) {
        snprintf(out, size, "%.*s", (int)strcspn(names, " "), names);
    }
}

/* "11 Oct", or "11 Oct 18:15" with the time. */
static void format_date(const skiff_app *app, const save_date *date, int with_time, char *out,
                        size_t size) {
    char month[MONTH_NAME_MAX];
    month_name(app, date->month, month, sizeof month);
    if (with_time) {
        snprintf(out, size, "%d %s %02d:%02d", date->day, month, date->hour, date->minute);
    } else {
        snprintf(out, size, "%d %s", date->day, month);
    }
}

/* ---- A game's state ---- */

static skiff_text_id state_id(const app_save_game *game) {
    if (game->problem != SAVE_PROBLEM_NONE) {
        return SKIFF_TEXT_SAVE_CANT_SYNC;
    }
    if (!game->decided) {
        return SKIFF_TEXT_COUNT;
    }
    switch (game->decision.action) {
    case SKIFF_SAVES_IN_SYNC:
        return SKIFF_TEXT_SAVE_SYNCED;
    case SKIFF_SAVES_UPLOAD:
        return SKIFF_TEXT_SAVE_CHANGED_HERE;
    case SKIFF_SAVES_DOWNLOAD:
        return game->decision.reason == SKIFF_SAVES_REASON_ONLY_SERVER
                   ? SKIFF_TEXT_SAVE_ONLY_IN_ROMM
                   : SKIFF_TEXT_SAVE_NEWER_IN_ROMM;
    case SKIFF_SAVES_ASK_CONFLICT:
        return game->decision.reason == SKIFF_SAVES_REASON_NO_BASELINE
                   ? SKIFF_TEXT_SAVE_DIFFERENT
                   : SKIFF_TEXT_SAVE_CHANGED_BOTH;
    case SKIFF_SAVES_ASK_DELETED_LOCAL:
        return SKIFF_TEXT_SAVE_DELETED_HERE;
    case SKIFF_SAVES_ASK_DELETED_SERVER:
        return SKIFF_TEXT_SAVE_DELETED_IN_ROMM;
    }
    return SKIFF_TEXT_COUNT;
}

void app_saves_state_text(const skiff_app *app, const app_save_game *game, int whole, char *out) {
    const skiff_text_id id = state_id(game);
    out[0] = '\0';
    if (id == SKIFF_TEXT_COUNT) {
        return;
    }
    if (whole && game->problem != SAVE_PROBLEM_NONE) {
        app_saves_problem_text(app, game, out);
        return;
    }
    save_date date;
    if (id == SKIFF_TEXT_SAVE_SYNCED && date_of_ms(game->synced_ms, &date)) {
        char day[SKIFF_APP_DETAIL_MAX];
        format_date(app, &date, 0, day, sizeof day);
        const char *args[] = {day};
        app_format(app, SKIFF_TEXT_SAVE_SYNCED_ON, args, 1, out);
        return;
    }
    app_format(app, id, NULL, 0, out);
}

/* ---- The Saves screen ---- */

/* The selected game's state in full, and when it last synced. */
static void selected_lines(skiff_app *app, const app_save_game *game) {
    char text[SKIFF_TEXT_MAX];
    app_saves_state_text(app, game, 1, text);
    if (text[0] != '\0') {
        app_view_add_wrapped(app, text, SKIFF_APP_TEXT_WIDTH);
    }
    save_date date;
    if (date_of_ms(game->synced_ms, &date)) {
        char when[SKIFF_APP_DETAIL_MAX];
        format_date(app, &date, 1, when, sizeof when);
        const char *args[] = {when};
        app_format(app, SKIFF_TEXT_SAVE_LAST_SYNCED, args, 1, text);
        app_view_add_wrapped(app, text, SKIFF_APP_TEXT_WIDTH);
    }
}

static void add_error_text(skiff_app *app, skiff_err err) {
    char line[SKIFF_TEXT_MAX];
    (void)skiff_error_line(app->config.language, err, line, sizeof line);
    app_view_add_wrapped(app, line, SKIFF_APP_TEXT_WIDTH);
}

/* The list itself, with a game per row. */
static void list_rows(skiff_app *app) {
    skiff_app_view *view = &app->view;
    const app_saves *saves = app->saves;
    const skiff_ui_list *list = &saves->list;
    view->list = *list;
    for (size_t i = 0; i < list->rows && list->first + i < list->count; i++) {
        const size_t index = list->first + i;
        const app_save_game *game = &saves->shown->games[index];
        skiff_app_row *row = &view->rows[view->row_count++];
        snprintf(row->label, sizeof row->label, "%s", saves->labels[index]);
        char state[SKIFF_TEXT_MAX];
        app_saves_state_text(app, game, 0, state);
        app_fit(app, state, SKIFF_APP_DETAIL_WIDTH, row->detail, sizeof row->detail);
        row->dim = game->problem != SAVE_PROBLEM_NONE && game->problem != SAVE_PROBLEM_ERROR;
        row->attention = app_saves_needs_player(game);
    }
}

void app_saves_view(skiff_app *app) {
    skiff_app_view *view = &app->view;
    const app_saves *saves = app->saves;
    app_view_set_title(app, app_text(app, SKIFF_TEXT_TITLE_SAVES));
    view->has_list = 1;
    view->lines_below = 1;
    skiff_ui_list_init(&view->list, 0, APP_SAVES_ROWS);
    switch (app_saves_blocked(app)) {
    case SAVES_UNRECOVERED:
        add_error_text(app, saves->recovered);
        app_view_add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
        return;
    case SAVES_NOT_CONNECTED:
        app_view_add_wrapped(app, app_text(app, SKIFF_TEXT_SAVES_OFFLINE), SKIFF_APP_TEXT_WIDTH);
        app_view_add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
        return;
    case SAVES_NEEDS_UPDATE:
        add_error_text(app, SKIFF_ERR_ROMM_SAVES_UNSUPPORTED);
        app_view_add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
        return;
    case SAVES_NEEDS_PAIRING:
        app_view_add_wrapped(app, app_text(app, SKIFF_TEXT_SAVES_PAIR_AGAIN), SKIFF_APP_TEXT_WIDTH);
        app_view_add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_PAIR_AGAIN);
        app_view_add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
        return;
    case SAVES_OK:
        break;
    }
    const app_save_list *list = saves->shown;
    if (list->count == 0) {
        if (saves->check_error != SKIFF_OK) {
            add_error_text(app, saves->check_error);
        } else if (app_saves_busy(app)) {
            app_view_add_wrapped(app, app_text(app, SKIFF_TEXT_SAVES_CHECKING),
                                 SKIFF_APP_TEXT_WIDTH);
        } else {
            app_view_add_wrapped(app,
                                 app_text(app, saves->checked ? SKIFF_TEXT_SAVES_EMPTY
                                                              : SKIFF_TEXT_SAVES_NOT_CHECKED),
                                 SKIFF_APP_TEXT_WIDTH);
        }
        app_view_add_hint(app, SKIFF_UI_ACTION_CONFIRM,
                          saves->check_error != SKIFF_OK ? SKIFF_TEXT_RETRY : SKIFF_TEXT_SYNC_ALL);
        app_view_add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
        return;
    }
    char count[24];
    snprintf(count, sizeof count, "%zu", list->count);
    const char *args[] = {count};
    char status[SKIFF_TEXT_MAX];
    app_format(app, SKIFF_TEXT_LIBRARY_COUNT, args, 1, status);
    app_view_set_status(app, status);
    list_rows(app);
    if (saves->check_error != SKIFF_OK) {
        add_error_text(app, saves->check_error);
    } else {
        selected_lines(app, &list->games[saves->list.selected]);
    }
    if (view->line_count > APP_SAVES_DETAIL_LINES) {
        view->line_count = APP_SAVES_DETAIL_LINES;
    }
    app_view_add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_SYNC);
    app_view_add_hint(app, SKIFF_UI_ACTION_EXTRA, SKIFF_TEXT_SYNC_ALL);
    app_view_add_hint(app, SKIFF_UI_ACTION_MENU, SKIFF_TEXT_BACKUPS);
    app_view_add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
}

/* ---- A game's backups ---- */

void app_saves_backups_view(skiff_app *app) {
    skiff_app_view *view = &app->view;
    const app_saves *saves = app->saves;
    app_view_set_title(app, app_text(app, SKIFF_TEXT_TITLE_BACKUPS));
    app_view_set_status(app, app_saves_title(&saves->backups_game));
    view->has_list = 1;
    view->lines_below = 1;
    view->list = saves->backups_list;
    if (!saves->backups_known) {
        app_view_add_wrapped(app, app_text(app, SKIFF_TEXT_LIBRARY_LOADING), SKIFF_APP_TEXT_WIDTH);
    } else if (saves->backups_error != SKIFF_OK) {
        add_error_text(app, saves->backups_error);
    } else if (saves->backups.count == 0) {
        app_view_add_wrapped(app, app_text(app, SKIFF_TEXT_BACKUPS_EMPTY), SKIFF_APP_TEXT_WIDTH);
    }
    const skiff_ui_list *list = &saves->backups_list;
    for (size_t i = 0; i < list->rows && list->first + i < list->count; i++) {
        const skiff_saves_backup *backup = &saves->backups.backups[list->first + i];
        skiff_app_row *row = &view->rows[view->row_count++];
        char number[16];
        snprintf(number, sizeof number, "%u", (unsigned)backup->number);
        const char *args[] = {number};
        char label[SKIFF_TEXT_MAX];
        app_format(app, SKIFF_TEXT_BACKUP_ROW, args, 1, label);
        app_fit(app, label, SKIFF_APP_LABEL_WIDTH, row->label, sizeof row->label);
        save_date date;
        if (date_of_ms(backup->mtime * APP_MS_PER_S, &date)) {
            char when[SKIFF_APP_DETAIL_MAX];
            format_date(app, &date, 1, when, sizeof when);
            app_fit(app, when, SKIFF_APP_DETAIL_WIDTH, row->detail, sizeof row->detail);
        }
    }
    if (saves->backups_known && saves->backups.count > 0) {
        app_view_add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_RESTORE);
    }
    app_view_add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
}

/* ---- Two saves, or one deleted ---- */

/* The comparison's columns: the side, its save's day and time, and the device RomM's came from. */
enum { SIDE_COLUMN, DAY_COLUMN, TIME_COLUMN, SOURCE_COLUMN, SIDE_COLUMNS };
_Static_assert(SIDE_COLUMNS <= SKIFF_APP_TABLE_COLUMNS, "a side's row must fit a table row");

/* A choice's row: its button, what it does, and what that means for the other save. */
typedef struct choice_option {
    unsigned action;
    skiff_text_id label;
    skiff_text_id detail;
} choice_option;

/* Each choice's row: its label, then its detail, dimmed. */
enum { CHOICE_OPTIONS = 3, CHOICE_DETAIL_COLUMN = 1 };
_Static_assert(CHOICE_OPTIONS <= SKIFF_APP_TABLE_ROWS_MAX, "every option must fit the table");

/* What happened to the game's saves, in one line. */
static skiff_text_id situation_text(const app_save_game *game) {
    if (game->decision.action == SKIFF_SAVES_ASK_DELETED_LOCAL) {
        return SKIFF_TEXT_SAVE_DELETED_LOCAL_CHOICE;
    }
    if (game->decision.action == SKIFF_SAVES_ASK_DELETED_SERVER) {
        return SKIFF_TEXT_SAVE_DELETED_SERVER_CHOICE;
    }
    return game->decision.reason == SKIFF_SAVES_REASON_NO_BASELINE ? SKIFF_TEXT_SAVE_CONFLICT_FIRST
                                                                   : SKIFF_TEXT_SAVE_CONFLICT;
}

/* A side's row: its name, then its save's day and time, "Deleted" when it has none, or nothing
 * when the date is not known. */
static void add_side(const skiff_app *app, skiff_app_table *table, skiff_text_id side, int deleted,
                     const save_date *date, const char *source) {
    char day[SKIFF_APP_DETAIL_MAX] = "";
    char time[SKIFF_APP_DETAIL_MAX] = "";
    if (deleted) {
        snprintf(day, sizeof day, "%s", app_text(app, SKIFF_TEXT_SAVE_GONE));
    } else if (date != NULL) {
        format_date(app, date, 0, day, sizeof day);
        snprintf(time, sizeof time, "%02d:%02d", date->hour, date->minute);
    }
    const char *cells[SIDE_COLUMNS] = {app_text(app, side), day, time, source};
    app_view_add_table_row(app, table, 0, cells, SIDE_COLUMNS);
}

/* Both sides aligned: this PSP's save by the Memory Stick's clock, RomM's by RomM's, and the device
 * RomM's came from (or the emulator, when RomM names no device). */
static void add_sides(skiff_app *app, const app_save_game *game) {
    skiff_app_table *table = app_view_add_table(app, SOURCE_COLUMN);
    if (table == NULL) {
        return;
    }
    save_date local;
    const int has_local = date_of_ms(game->local_mtime * APP_MS_PER_S, &local);
    save_date server;
    const int has_server = parse_iso(game->server_updated_at, &server);
    add_side(app, table, SKIFF_TEXT_SIDE_THIS_PSP,
             game->decision.action == SKIFF_SAVES_ASK_DELETED_LOCAL, has_local ? &local : NULL, "");
    add_side(app, table, SKIFF_TEXT_SIDE_ROMM,
             game->decision.action == SKIFF_SAVES_ASK_DELETED_SERVER, has_server ? &server : NULL,
             game->server_device);
    app_view_layout_table(app, table);
}

/* The buttons, as the choice's update reads them (app_saves_choice_update()), each with what it
 * does to the other save; Later changes nothing. */
static void choice_options(const app_save_game *game, choice_option *out) {
    const choice_option later = {SKIFF_UI_ACTION_BACK, SKIFF_TEXT_LATER, SKIFF_TEXT_LATER_DETAIL};
    out[CHOICE_OPTIONS - 1] = later;
    if (game->decision.action == SKIFF_SAVES_ASK_DELETED_LOCAL) {
        const choice_option again = {SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_DOWNLOAD_AGAIN,
                                     SKIFF_TEXT_DOWNLOAD_AGAIN_DETAIL};
        const choice_option leave = {SKIFF_UI_ACTION_EXTRA, SKIFF_TEXT_LEAVE_IT,
                                     SKIFF_TEXT_LEAVE_IT_DETAIL};
        out[0] = again;
        out[1] = leave;
    } else if (game->decision.action == SKIFF_SAVES_ASK_DELETED_SERVER) {
        /* RomM's side is no save at all: taking it removes this PSP's, which is said as such. */
        const choice_option keep = {SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_KEEP_THIS_PSP,
                                    SKIFF_TEXT_UPLOAD_AGAIN_DETAIL};
        const choice_option remove = {SKIFF_UI_ACTION_EXTRA, SKIFF_TEXT_REMOVE_THIS_PSP,
                                      SKIFF_TEXT_REMOVE_THIS_PSP_DETAIL};
        out[0] = keep;
        out[1] = remove;
    } else {
        const choice_option keep = {SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_KEEP_THIS_PSP,
                                    SKIFF_TEXT_KEEP_THIS_PSP_DETAIL};
        const choice_option use = {SKIFF_UI_ACTION_EXTRA, SKIFF_TEXT_USE_ROMM,
                                   SKIFF_TEXT_USE_ROMM_DETAIL};
        out[0] = keep;
        out[1] = use;
    }
}

void app_saves_choice_view(skiff_app *app) {
    const app_save_game *game = &app->saves->choice;
    app_view_set_title(app, app_text(app, SKIFF_TEXT_TITLE_SAVE_CHOICE));
    app_view_set_status(app, app_saves_title(game));
    app_view_add_wrapped(app, app_text(app, situation_text(game)), SKIFF_APP_TEXT_WIDTH);
    add_sides(app, game);
    choice_option options[CHOICE_OPTIONS];
    choice_options(game, options);
    skiff_app_table *table = app_view_add_table(app, CHOICE_DETAIL_COLUMN);
    for (size_t i = 0; i < CHOICE_OPTIONS; i++) {
        if (table != NULL) {
            const char *cells[] = {app_text(app, options[i].label),
                                   app_text(app, options[i].detail)};
            app_view_add_table_row(app, table, options[i].action, cells,
                                   sizeof cells / sizeof cells[0]);
        }
        /* The footer names them too, as on every screen. */
        app_view_add_hint(app, options[i].action, options[i].label);
    }
    if (table != NULL) {
        app_view_layout_table(app, table);
    }
}

/* ---- The game's details ---- */

int app_saves_details_line(const skiff_app *app, char *out) {
    out[0] = '\0';
    if (!app->has_rom || app_saves_blocked(app) != SAVES_OK) {
        return 0;
    }
    char state[SKIFF_TEXT_MAX] = "";
    app_save_game game;
    if (!app_saves_rom_game(app, &game)) {
        app_format(app, SKIFF_TEXT_SAVE_NO_ID_SHORT, NULL, 0, state);
    } else {
        app_saves_state_text(app, &game, 0, state);
    }
    if (state[0] == '\0') {
        return 0;
    }
    const char *args[] = {state};
    app_format(app, SKIFF_TEXT_DETAILS_SAVE, args, 1, out);
    return 1;
}
