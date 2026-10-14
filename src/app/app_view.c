#include <stdio.h>
#include <string.h>

#include "skiff/version.h"

#include "app_internal.h"

#define APP_TITLE "Skiff"

/* ---- Text ---- */

const char *app_text(const skiff_app *app, skiff_text_id id) {
    return skiff_text(app->config.language, id);
}

void app_format(const skiff_app *app, skiff_text_id id, const char *const *args, size_t count,
                char *out) {
    /* A text cut short still shows what fitted. */
    (void)skiff_text_format(app_text(app, id), args, count, out, SKIFF_TEXT_MAX);
}

void app_format_bytes(const skiff_app *app, uint64_t bytes, char *out, size_t out_size) {
    if (skiff_ui_format_bytes(bytes, app_text(app, SKIFF_TEXT_DECIMAL_SEPARATOR), out, out_size) !=
        SKIFF_OK) {
        out[0] = '\0';
    }
}

void app_fit(const skiff_app *app, const char *text, float width, char *out, size_t out_size) {
    if (skiff_ui_fit_text(text, width, app->env.measure, app->env.ctx, out, out_size) != SKIFF_OK) {
        out[0] = '\0';
    }
}

void app_format_fit_name(const skiff_app *app, skiff_text_id id, const char *name, float width,
                         char *out, size_t out_size) {
    const char *none[] = {""};
    char rest[SKIFF_TEXT_MAX];
    app_format(app, id, none, 1, rest);
    char fitted[SKIFF_TEXT_MAX];
    app_fit(app, name, width - app->env.measure(app->env.ctx, rest), fitted, sizeof fitted);
    const char *args[] = {fitted};
    char text[SKIFF_TEXT_MAX];
    app_format(app, id, args, 1, text);
    app_fit(app, text, width, out, out_size);
}

static int is_continuation(char c) { return ((unsigned char)c & 0xC0U) == 0x80U; }

/* The longest start of text, at most length bytes and ending between UTF-8 characters, whose
 * width is within width; at least one character. */
static size_t longest_fitting(const char *text, size_t length, float width,
                              skiff_ui_measure_fn measure, void *ctx) {
    char candidate[SKIFF_TEXT_MAX];
    size_t best = 0;
    for (size_t end = 1; end <= length && end < sizeof candidate; end++) {
        if (end < length && is_continuation(text[end])) {
            continue;
        }
        memcpy(candidate, text, end);
        candidate[end] = '\0';
        if (measure(ctx, candidate) > width) {
            break;
        }
        best = end;
    }
    if (best == 0) {
        best = 1;
        while (best < length && is_continuation(text[best])) {
            best++;
        }
    }
    return best;
}

/* Bytes of text, the length bytes before its line break or end, that fit width on one line,
 * ending at a word where one fits. */
static size_t line_length(const char *text, size_t length, float width, skiff_ui_measure_fn measure,
                          void *ctx) {
    char candidate[SKIFF_TEXT_MAX];
    size_t best = 0;
    for (size_t end = 0; end <= length && end < sizeof candidate; end++) {
        if (end < length && text[end] != ' ') {
            continue;
        }
        memcpy(candidate, text, end);
        candidate[end] = '\0';
        if (measure(ctx, candidate) > width) {
            break;
        }
        best = end;
    }
    if (best == 0) {
        const size_t word = strcspn(text, " ");
        best = longest_fitting(text, word < length ? word : length, width, measure, ctx);
    }
    return best;
}

/* The line of text that starts at *at or after the blanks there, into *span (without the blanks
 * around it), *at moved past it and the line break that ends it; an empty line for a line break
 * right there. 0 when only blanks are left. */
static int next_line(const char *text, size_t *at, float width, skiff_ui_measure_fn measure,
                     void *ctx, skiff_app_span *span) {
    const size_t start = *at + strspn(text + *at, " ");
    span->start = start;
    span->length = 0;
    if (text[start] == '\0') {
        *at = start;
        return 0;
    }
    if (text[start] == '\n') {
        *at = start + 1;
        return 1;
    }
    size_t length = line_length(text + start, strcspn(text + start, "\n"), width, measure, ctx);
    if (length >= SKIFF_TEXT_MAX) {
        length = SKIFF_TEXT_MAX - 1;
    }
    size_t end = length;
    while (end > 0 && text[start + end - 1] == ' ') {
        end--;
    }
    span->length = end;
    size_t next = start + length;
    next += strspn(text + next, " ");
    /* The break that ends this line, not an empty line of its own. */
    *at = text[next] == '\n' ? next + 1 : next;
    return 1;
}

/* What is left of text from rest on, as one line within width, cut with SKIFF_UI_ELLIPSIS: line
 * breaks read as spaces, blanks at its end dropped. */
static void fit_rest(const char *rest, float width, skiff_ui_measure_fn measure, void *ctx,
                     char *out) {
    /* Longer than out holds, so a longer rest is still cut with the ellipsis. */
    char joined[2 * SKIFF_TEXT_MAX];
    size_t used = 0;
    for (const char *c = rest; *c != '\0' && used + 1 < sizeof joined; c++) {
        const int blank = *c == ' ' || *c == '\n';
        if (blank && used > 0 && joined[used - 1] != ' ') {
            joined[used++] = ' ';
        } else if (!blank) {
            joined[used++] = *c;
        }
    }
    while (used > 0 && joined[used - 1] == ' ') {
        used--;
    }
    joined[used] = '\0';
    if (skiff_ui_fit_text(joined, width, measure, ctx, out, SKIFF_TEXT_MAX) != SKIFF_OK) {
        out[0] = '\0';
    }
}

size_t skiff_app_wrap(const char *text, float width, skiff_ui_measure_fn measure, void *measure_ctx,
                      char (*lines)[SKIFF_TEXT_MAX], size_t max_lines) {
    if (text == NULL || measure == NULL || lines == NULL) {
        return 0;
    }
    size_t count = 0;
    size_t at = 0;
    while (count < max_lines) {
        const size_t start = at + strspn(text + at, " ");
        if (text[start] == '\0') {
            break;
        }
        if (count + 1 == max_lines) {
            fit_rest(text + start, width, measure, measure_ctx, lines[count]);
            count++;
            break;
        }
        skiff_app_span span;
        (void)next_line(text, &at, width, measure, measure_ctx, &span);
        memcpy(lines[count], text + span.start, span.length);
        lines[count][span.length] = '\0';
        count++;
    }
    return count;
}

size_t skiff_app_wrap_spans(const char *text, float width, skiff_ui_measure_fn measure,
                            void *measure_ctx, skiff_app_span *spans, size_t max_spans, int *cut) {
    if (cut != NULL) {
        *cut = 0;
    }
    if (text == NULL || measure == NULL || spans == NULL) {
        return 0;
    }
    size_t count = 0;
    size_t at = 0;
    skiff_app_span span;
    while (next_line(text, &at, width, measure, measure_ctx, &span)) {
        if (count == max_spans) {
            /* What ends cut ends on text, so a mark after it reads as the cut. */
            while (count > 0 && spans[count - 1].length == 0) {
                count--;
            }
            if (cut != NULL) {
                *cut = 1;
            }
            break;
        }
        spans[count++] = span;
    }
    return count;
}

/* ---- Line heights ---- */

int skiff_app_line_height(const skiff_app_view *view, size_t index) {
    if (view == NULL || index >= view->line_count) {
        return 0;
    }
    if (view->line_styles[index] != SKIFF_APP_LINE_HEADING) {
        return SKIFF_APP_LINE_HEIGHT;
    }
    const int last =
        index + 1 == view->line_count || view->line_styles[index + 1] != SKIFF_APP_LINE_HEADING;
    return SKIFF_APP_HEADING_LINE_HEIGHT + (last ? SKIFF_APP_HEADING_GAP : 0);
}

int skiff_app_lines_height(const skiff_app_view *view) {
    if (view == NULL) {
        return 0;
    }
    int height = 0;
    for (size_t i = 0; i < view->line_count; i++) {
        height += skiff_app_line_height(view, i);
    }
    return height;
}

/* The pixels the body has left under the view's lines (and the gap above the first). */
static int body_left(const skiff_app_view *view) {
    return SKIFF_APP_BODY_HEIGHT - SKIFF_APP_BLOCK_GAP - skiff_app_lines_height(view);
}

/* The lines of body text that still fit under the view's lines. */
static size_t lines_room(const skiff_app_view *view) {
    const int left = body_left(view);
    const size_t fit = left > 0 ? (size_t)left / SKIFF_APP_LINE_HEIGHT : 0;
    const size_t slots = SKIFF_APP_LINES_MAX - view->line_count;
    return fit < slots ? fit : slots;
}

/* Room for one more line of style under the view's lines: a heading after a heading takes over
 * the gap under it. */
static int line_fits(const skiff_app_view *view, skiff_app_line_style style) {
    if (style != SKIFF_APP_LINE_HEADING) {
        return lines_room(view) > 0;
    }
    const int after_heading =
        view->line_count > 0 && view->line_styles[view->line_count - 1] == SKIFF_APP_LINE_HEADING;
    const int needed = SKIFF_APP_HEADING_LINE_HEIGHT + (after_heading ? 0 : SKIFF_APP_HEADING_GAP);
    return view->line_count < SKIFF_APP_LINES_MAX && body_left(view) >= needed;
}

/* ---- Building the view ---- */

static void add_wrapped_within(skiff_app *app, const char *text, float width) {
    skiff_app_view *view = &app->view;
    view->line_count += skiff_app_wrap(text, width, app->env.measure, app->env.ctx,
                                       &view->lines[view->line_count], lines_room(view));
}

/* The lines text takes at width. */
static size_t wrapped_lines(const skiff_app *app, const char *text, float width) {
    char lines[SKIFF_APP_LINES_MAX][SKIFF_TEXT_MAX];
    return skiff_app_wrap(text, width, app->env.measure, app->env.ctx, lines, SKIFF_APP_LINES_MAX);
}

static void add_wrapped(skiff_app *app, const char *text) {
    add_wrapped_within(app, text, SKIFF_APP_TEXT_WIDTH);
}

static void add_text_within(skiff_app *app, skiff_text_id id, float width) {
    add_wrapped_within(app, app_text(app, id), width);
}

static void add_text(skiff_app *app, skiff_text_id id) {
    add_text_within(app, id, SKIFF_APP_TEXT_WIDTH);
}

static void add_formatted_within(skiff_app *app, skiff_text_id id, const char *const *args,
                                 size_t count, float width) {
    char text[SKIFF_TEXT_MAX];
    app_format(app, id, args, count, text);
    add_wrapped_within(app, text, width);
}

static void add_formatted(skiff_app *app, skiff_text_id id, const char *const *args, size_t count) {
    add_formatted_within(app, id, args, count, SKIFF_APP_TEXT_WIDTH);
}

/* Where an address breaks: before its query ('?') when it does not fit one line, so the pairing
 * code in it stays whole; NULL when it is wrapped as it comes. */
static const char *url_break(const skiff_app *app, const char *url, float width) {
    const char *query = strchr(url, '?');
    return query != NULL && app->env.measure(app->env.ctx, url) > width ? query : NULL;
}

/* The lines add_url() gives an address. */
static size_t url_lines(const skiff_app *app, const char *url, float width) {
    const char *query = url_break(app, url, width);
    if (query == NULL) {
        return wrapped_lines(app, url, width);
    }
    char path[SKIFF_ROMM_URL_MAX];
    snprintf(path, sizeof path, "%.*s", (int)(query - url), url);
    return wrapped_lines(app, path, width) + wrapped_lines(app, query, width);
}

/* An address on lines of its own, broken as url_break() says. */
static void add_url(skiff_app *app, const char *url, float width) {
    const char *query = url_break(app, url, width);
    if (query == NULL) {
        add_wrapped_within(app, url, width);
        return;
    }
    char path[SKIFF_ROMM_URL_MAX];
    snprintf(path, sizeof path, "%.*s", (int)(query - url), url);
    add_wrapped_within(app, path, width);
    add_wrapped_within(app, query, width);
}

/* An empty line between blocks of text. */
static void add_gap(skiff_app *app) {
    skiff_app_view *view = &app->view;
    if (view->line_count > 0 && lines_room(view) > 0) {
        view->lines[view->line_count++][0] = '\0';
    }
}

static void add_error(skiff_app *app, skiff_err err) {
    char text[SKIFF_TEXT_MAX];
    (void)skiff_error_line(app->config.language, err, text, sizeof text);
    add_wrapped(app, text);
}

static void add_hint_text(skiff_app *app, unsigned action, const char *label) {
    skiff_app_view *view = &app->view;
    if (view->hint_count == SKIFF_APP_HINTS_MAX) {
        return;
    }
    view->hints[view->hint_count].action = action;
    snprintf(view->hints[view->hint_count].label, SKIFF_APP_HINT_MAX, "%s", label);
    view->hint_count++;
}

static void add_hint(skiff_app *app, unsigned action, skiff_text_id label) {
    add_hint_text(app, action, app_text(app, label));
}

/* Settings' hint, with the saves that need the player when some do ("Settings (1)"): seen from the
 * library, where the launch check's note is gone after a few seconds. */
static void add_settings_hint(skiff_app *app) {
    const size_t waiting = app_saves_waiting(app);
    if (waiting == 0) {
        add_hint(app, SKIFF_UI_ACTION_MENU, SKIFF_TEXT_SETTINGS);
        return;
    }
    char count[24];
    snprintf(count, sizeof count, "%zu", waiting);
    const char *args[] = {count};
    char label[SKIFF_APP_HINT_MAX];
    (void)skiff_text_format(app_text(app, SKIFF_TEXT_SETTINGS_WAITING), args, 1, label,
                            sizeof label);
    add_hint_text(app, SKIFF_UI_ACTION_MENU, label);
}

float app_view_status_width(const skiff_app *app) {
    const float title = app->env.measure(app->env.ctx, app->view.title) * SKIFF_APP_TITLE_SCALE;
    const float width =
        (SKIFF_APP_TEXT_WIDTH - title - SKIFF_APP_HEADER_GAP) / SKIFF_APP_STATUS_SCALE;
    return width > SKIFF_APP_STATUS_MIN_WIDTH ? width : SKIFF_APP_STATUS_MIN_WIDTH;
}

static void set_status(skiff_app *app, const char *text) {
    app_fit(app, text, app_view_status_width(app), app->view.status, sizeof app->view.status);
}

/* A note or activity in the header, its name shortened when the header is too narrow for it. */
static void set_header_text(skiff_app *app, const app_header_text *header) {
    if (header->named == SKIFF_TEXT_COUNT) {
        set_status(app, header->text);
        return;
    }
    app_format_fit_name(app, header->named, header->text, app_view_status_width(app),
                        app->view.status, sizeof app->view.status);
}

static void set_title(skiff_app *app, const char *title) {
    snprintf(app->view.title, sizeof app->view.title, "%s", title);
}

void app_view_add_hint(skiff_app *app, unsigned action, skiff_text_id label) {
    add_hint(app, action, label);
}

void app_view_add_wrapped(skiff_app *app, const char *text, float width) {
    add_wrapped_within(app, text, width);
}

void app_view_set_status(skiff_app *app, const char *text) { set_status(app, text); }

void app_view_set_title(skiff_app *app, const char *title) { set_title(app, title); }

skiff_app_table *app_view_add_table(skiff_app *app, size_t dim_from) {
    skiff_app_view *view = &app->view;
    if (view->table_count == SKIFF_APP_TABLES_MAX) {
        return NULL;
    }
    skiff_app_table *table = &view->tables[view->table_count++];
    /* Whatever an earlier view left in the slot goes, outside app_view_build()'s reset too. */
    memset(table, 0, sizeof *table);
    table->dim_from = dim_from;
    return table;
}

void app_view_add_table_row(const skiff_app *app, skiff_app_table *table, unsigned action,
                            const char *const *cells, size_t count) {
    if (table->row_count == SKIFF_APP_TABLE_ROWS_MAX) {
        return;
    }
    skiff_app_table_row *row = &table->rows[table->row_count++];
    row->action = action;
    for (size_t i = 0; i < count && i < SKIFF_APP_TABLE_COLUMNS; i++) {
        /* Cut between UTF-8 characters when longer than a cell holds; laid out later. */
        app_fit(app, cells[i] != NULL ? cells[i] : "", SKIFF_APP_TEXT_WIDTH, row->cells[i],
                sizeof row->cells[i]);
    }
}

void app_view_layout_table(const skiff_app *app, skiff_app_table *table) {
    float x = 0.0f;
    for (size_t r = 0; r < table->row_count; r++) {
        if (table->rows[r].action != 0) {
            x = (float)SKIFF_APP_SYMBOL_WIDTH;
        }
    }
    for (size_t c = 0; c < SKIFF_APP_TABLE_COLUMNS; c++) {
        table->column_x[c] = x;
        float widest = 0.0f;
        for (size_t r = 0; r < table->row_count; r++) {
            char *cell = table->rows[r].cells[c];
            if (cell[0] == '\0') {
                continue;
            }
            char fitted[SKIFF_APP_CELL_MAX];
            app_fit(app, cell, SKIFF_APP_TEXT_WIDTH - x, fitted, sizeof fitted);
            memcpy(cell, fitted, sizeof fitted);
            const float width = app->env.measure(app->env.ctx, cell);
            widest = width > widest ? width : widest;
        }
        if (widest > 0.0f) {
            x += widest + SKIFF_APP_COLUMN_GAP;
        }
    }
}

static void add_free_space_within(skiff_app *app, float width) {
    if (!app->free_known) {
        return;
    }
    char size[SKIFF_APP_DETAIL_MAX];
    app_format_bytes(app, app->free_bytes, size, sizeof size);
    const char *args[] = {size};
    add_formatted_within(app, SKIFF_TEXT_FREE_SPACE, args, 1, width);
}

static void add_free_space(skiff_app *app) { add_free_space_within(app, SKIFF_APP_TEXT_WIDTH); }

static void connecting_view(skiff_app *app) {
    set_title(app, APP_TITLE);
    switch (app->connect) {
    case CONNECT_NETWORK:
    case CONNECT_JOINING:
        if (app->waiting_switch) {
            add_text(app, SKIFF_TEXT_NET_WAITING_SWITCH);
        } else {
            const char *args[] = {app->profile_name};
            add_formatted(app, SKIFF_TEXT_NET_JOINING, args, 1);
        }
        break;
    case CONNECT_TLS:
        add_text(app, SKIFF_TEXT_NET_STARTING);
        break;
    case CONNECT_HEARTBEAT:
    case CONNECT_PAIRING:
    case CONNECT_PLATFORM:
    case CONNECT_DONE:
        add_text(app, SKIFF_TEXT_NET_CONTACTING);
        break;
    }
}

static void server_view(skiff_app *app) {
    set_title(app, app_text(app, SKIFF_TEXT_TITLE_SERVER));
    add_text(app, SKIFF_TEXT_SERVER_PROMPT);
    add_text(app, SKIFF_TEXT_SERVER_NONE);
    add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_EDIT);
}

static void pair_view(skiff_app *app) {
    skiff_app_view *view = &app->view;
    const app_pairing *pairing = &app->pairing;
    set_title(app, app_text(app, SKIFF_TEXT_TITLE_PAIR));
    if (pairing->active) {
        /* Beside the QR code when the address fits one, the text wraps narrower. */
        const int has_qr = pairing->qr.size > 0;
        const float width = has_qr ? SKIFF_APP_QR_TEXT_WIDTH : SKIFF_APP_TEXT_WIDTH;
        view->qr = has_qr ? &pairing->qr : NULL;
        set_status(app, app_text(app, SKIFF_TEXT_PAIR_WAITING));
        add_text_within(
            app, has_qr ? SKIFF_TEXT_PAIR_INSTRUCTIONS_QR : SKIFF_TEXT_PAIR_INSTRUCTIONS, width);
        const int64_t left_ms = pairing->started_ms +
                                (int64_t)pairing->pairing.expires_in_s * APP_MS_PER_S -
                                app_now(app);
        char left[SKIFF_APP_DETAIL_MAX];
        if (skiff_ui_format_duration(left_ms > 0 ? (uint64_t)left_ms / APP_MS_PER_S : 0, left,
                                     sizeof left) != SKIFF_OK) {
            left[0] = '\0';
        }
        /* Small: the address carries it. RomM's page shows it too, so the player can check they
         * approve this PSP. */
        const char *args[] = {pairing->pairing.user_code, left};
        char code[SKIFF_TEXT_MAX];
        app_format(app, SKIFF_TEXT_PAIR_CODE, args, 2, code);
        char error[SKIFF_TEXT_MAX];
        error[0] = '\0';
        if (pairing->last_error != SKIFF_OK) {
            (void)skiff_error_line(app->config.language, pairing->last_error, error, sizeof error);
        }
        const char *approve = app_text(app, SKIFF_TEXT_PAIR_APPROVE);
        /* With the code in it: RomM 5.3.1's page has no field to type the code into, so without a
         * QR code this address is the only way to pair and it always goes whole, then the approval
         * and the code. A poll's error, then the two gaps, take what room is left (the longest
         * address, 511 characters, still leaves the approval and the code theirs). */
        const char *url = pairing->pairing.verification_url_complete;
        const size_t needed = view->line_count + url_lines(app, url, width) +
                              wrapped_lines(app, approve, width) + wrapped_lines(app, code, width);
        size_t room = SKIFF_APP_LINES_MAX > needed ? SKIFF_APP_LINES_MAX - needed : 0;
        const size_t error_lines = error[0] != '\0' ? wrapped_lines(app, error, width) : 0;
        const int show_error = error_lines > 0 && error_lines <= room;
        if (show_error) {
            room -= error_lines;
        }
        add_url(app, url, width);
        if (room > 0) {
            add_gap(app);
            room--;
        }
        add_wrapped_within(app, approve, width);
        if (room > 0) {
            add_gap(app);
        }
        add_wrapped_within(app, code, width);
        if (show_error) {
            add_wrapped_within(app, error, width);
        }
    } else if (pairing->ended == SKIFF_ERR_ROMM_PAIRING_DENIED) {
        add_text(app, SKIFF_TEXT_PAIR_DENIED);
    } else if (pairing->ended == SKIFF_ERR_ROMM_PAIRING_EXPIRED) {
        add_text(app, SKIFF_TEXT_PAIR_EXPIRED);
    } else if (pairing->ended != SKIFF_OK) {
        add_error(app, pairing->ended);
    } else {
        add_text(app, SKIFF_TEXT_NET_CONTACTING);
    }
    if (!pairing->active && pairing->ended != SKIFF_OK) {
        add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_NEW_CODE);
    }
    if (app->settings.token[0] != '\0') {
        add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
    }
    add_hint(app, SKIFF_UI_ACTION_MENU, SKIFF_TEXT_SETTINGS);
}

/* SELECT's hint: the list it goes to next. */
static skiff_text_id next_source_label(const skiff_app *app) {
    switch (app->source) {
    case SOURCE_ALL:
        return SKIFF_TEXT_SHOW_FAVOURITES;
    case SOURCE_FAVOURITES:
        return SKIFF_TEXT_SHOW_COLLECTIONS;
    case SOURCE_COLLECTIONS:
        break;
    }
    return SKIFF_TEXT_SHOW_ALL_GAMES;
}

static void set_count_status(skiff_app *app, skiff_text_id id, uint64_t count) {
    char number[24];
    snprintf(number, sizeof number, "%llu", (unsigned long long)count);
    const char *args[] = {number};
    char text[SKIFF_TEXT_MAX];
    app_format(app, id, args, 1, text);
    set_status(app, text);
}

/* The collections themselves (SOURCE_COLLECTIONS, none open): each with RomM's ROM count. */
static void collections_view(skiff_app *app) {
    skiff_app_view *view = &app->view;
    set_title(app, app_text(app, SKIFF_TEXT_TITLE_COLLECTIONS));
    if (app_collections_denied(app)) {
        add_text(app, SKIFF_TEXT_COLLECTIONS_PAIR_AGAIN);
        add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_PAIR_AGAIN);
    } else if (app->collections_error != SKIFF_OK) {
        add_error(app, app->collections_error);
        add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_RETRY);
    } else if (!app->collections_known) {
        add_text(app, SKIFF_TEXT_LIBRARY_LOADING);
    } else if (app->collections.count == 0) {
        add_text(app, SKIFF_TEXT_COLLECTIONS_EMPTY);
    } else {
        set_count_status(app, SKIFF_TEXT_COLLECTIONS_COUNT, app->collections.count);
        view->has_list = 1;
        view->list = app->collections_list;
        const skiff_ui_list *list = &app->collections_list;
        for (size_t i = 0; i < list->rows && list->first + i < list->count; i++) {
            const size_t index = list->first + i;
            skiff_app_row *row = &view->rows[view->row_count++];
            snprintf(row->label, sizeof row->label, "%s", app->collection_labels[index]);
            char number[24];
            snprintf(number, sizeof number, "%llu",
                     (unsigned long long)app->collections.items[index].rom_count);
            const char *args[] = {number};
            char text[SKIFF_TEXT_MAX];
            app_format(app, SKIFF_TEXT_LIBRARY_COUNT, args, 1, text);
            app_fit(app, text, SKIFF_APP_DETAIL_WIDTH, row->detail, sizeof row->detail);
        }
        add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_SELECT);
    }
    add_hint(app, SKIFF_UI_ACTION_EXTRA, SKIFF_TEXT_DOWNLOADS);
    add_settings_hint(app);
    add_hint(app, SKIFF_UI_ACTION_SELECT, SKIFF_TEXT_SHOW_ALL_GAMES);
}

static void library_title(skiff_app *app) {
    if (app->source == SOURCE_COLLECTIONS) {
        app_fit(app, app_collection_name(app, &app->collection), APP_TITLE_NAME_WIDTH,
                app->view.title, sizeof app->view.title);
        return;
    }
    set_title(app, app_text(app, app->source == SOURCE_FAVOURITES ? SKIFF_TEXT_TITLE_FAVOURITES
                                                                  : SKIFF_TEXT_TITLE_LIBRARY));
}

/* The text an empty list shows. */
static skiff_text_id empty_text(const skiff_app *app) {
    if (!app->has_platform) {
        return SKIFF_TEXT_LIBRARY_EMPTY;
    }
    switch (app->source) {
    case SOURCE_FAVOURITES:
        return SKIFF_TEXT_FAVOURITES_EMPTY;
    case SOURCE_COLLECTIONS:
        return SKIFF_TEXT_COLLECTION_EMPTY;
    case SOURCE_ALL:
        break;
    }
    return SKIFF_TEXT_LIBRARY_EMPTY;
}

static void library_view(skiff_app *app) {
    if (app->has_platform && !app_games_listed(app)) {
        collections_view(app);
        return;
    }
    skiff_app_view *view = &app->view;
    library_title(app);
    if (app->total_known) {
        set_count_status(app, SKIFF_TEXT_LIBRARY_COUNT, app->total);
    }
    const app_batch *batch = &app->batch;
    if (batch->step == BATCH_SCANNING || batch->step == BATCH_SPACE) {
        char seen[24];
        char total[24];
        snprintf(seen, sizeof seen, "%llu", (unsigned long long)batch->seen);
        snprintf(total, sizeof total, "%llu", (unsigned long long)app->total);
        const char *args[] = {seen, total};
        char text[SKIFF_TEXT_MAX];
        app_format(app,
                   batch->filter == SKIFF_ROMM_LIST_FAVOURITES
                       ? SKIFF_TEXT_BATCH_CHECKING
                       : SKIFF_TEXT_BATCH_CHECKING_COLLECTION,
                   args, 2, text);
        set_status(app, text);
    } else if (batch->step == BATCH_QUEUEING) {
        set_status(app, app_text(app, SKIFF_TEXT_BATCH_ADDING));
    }
    if (!app->has_platform || (app->total_known && app->total == 0)) {
        add_text(app, empty_text(app));
    } else if (!app->total_known) {
        add_text(app, SKIFF_TEXT_LIBRARY_LOADING);
    } else {
        view->has_list = 1;
        view->list = app->library;
        for (size_t i = 0; i < app->library.rows && app->library.first + i < app->library.count;
             i++) {
            const size_t index = app->library.first + i;
            skiff_app_row *row = &view->rows[view->row_count++];
            const app_page *page = NULL;
            for (size_t p = 0; p < SKIFF_APP_CACHED_PAGES; p++) {
                if (app->pages[p].valid && app->pages[p].index == index / SKIFF_ROMM_PAGE_SIZE) {
                    page = &app->pages[p];
                }
            }
            const size_t at = index % SKIFF_ROMM_PAGE_SIZE;
            if (page == NULL || at >= page->page.count) {
                snprintf(row->label, sizeof row->label, "%s",
                         app_text(app, SKIFF_TEXT_LIBRARY_LOADING));
                continue;
            }
            snprintf(row->label, sizeof row->label, "%s", page->labels[at]);
            app_rom_detail(app, &page->page.items[at], row->detail, sizeof row->detail);
            row->dim = !app_rom_downloadable(app, &page->page.items[at]);
        }
        if (batch->step == BATCH_NONE) {
            add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_SELECT);
        }
    }
    if (batch->step != BATCH_NONE) {
        /* While the list's games are checked, Back stops it; while it queues, nothing waits. */
        if (batch->step != BATCH_QUEUEING) {
            add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_CANCEL);
        }
        return;
    }
    if (app_batch_offered(app)) {
        add_hint(app, SKIFF_UI_ACTION_START, SKIFF_TEXT_DOWNLOAD_ALL);
    }
    add_hint(app, SKIFF_UI_ACTION_EXTRA, SKIFF_TEXT_DOWNLOADS);
    /* In a collection, Back to the collections takes Settings' place among the five hints
     * (Triangle still opens Settings). */
    if (app->source == SOURCE_COLLECTIONS) {
        add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
    } else {
        add_settings_hint(app);
    }
    if (app->has_platform) {
        add_hint(app, SKIFF_UI_ACTION_SELECT, next_source_label(app));
    }
}

/* The width text takes drawn as a heading (ctx is the app). */
static float heading_measure(void *ctx, const char *text) {
    const skiff_app *app = ctx;
    return app->env.measure(app->env.ctx, text) * SKIFF_APP_HEADING_SCALE;
}

/* A game's name (or its file name) beside the cover, in style and wrapped as it is drawn: at most
 * APP_DETAILS_NAME_LINES lines, the last cut with "..." when there is more, so the facts, the
 * download state and the free space always fit under it. */
static void add_name_within(skiff_app *app, const char *name, float width,
                            skiff_app_line_style style) {
    const int heading = style == SKIFF_APP_LINE_HEADING;
    const skiff_ui_measure_fn measure = heading ? heading_measure : app->env.measure;
    void *measure_ctx = heading ? (void *)app : app->env.ctx;
    char lines[SKIFF_APP_LINES_MAX][SKIFF_TEXT_MAX];
    const size_t count =
        skiff_app_wrap(name, width, measure, measure_ctx, lines, SKIFF_APP_LINES_MAX);
    skiff_app_view *view = &app->view;
    for (size_t i = 0; i < count && i < APP_DETAILS_NAME_LINES; i++) {
        if (!line_fits(view, style)) {
            return;
        }
        view->line_styles[view->line_count] = style;
        char *line = view->lines[view->line_count++];
        if (i + 1 == APP_DETAILS_NAME_LINES && count > APP_DETAILS_NAME_LINES) {
            /* Longer than the line, so the fit ends it with "...". */
            char joined[2 * SKIFF_TEXT_MAX];
            const size_t first = strnlen(lines[i], SKIFF_TEXT_MAX - 1);
            const size_t next = strnlen(lines[i + 1], SKIFF_TEXT_MAX - 1);
            memcpy(joined, lines[i], first);
            joined[first] = ' ';
            memcpy(joined + first + 1, lines[i + 1], next);
            joined[first + 1 + next] = '\0';
            if (skiff_ui_fit_text(joined, width, measure, measure_ctx, line, SKIFF_TEXT_MAX) !=
                SKIFF_OK) {
                line[0] = '\0';
            }
        } else {
            memcpy(line, lines[i], SKIFF_TEXT_MAX);
        }
    }
}

/* RomM's language names (backend/handler/filesystem/base_handler.py, 5.4.0) as its own short
 * codes, so a game's languages fit beside the cover; another name shows as it is. */
typedef struct language_code {
    const char *name;
    const char *code;
} language_code;

static const language_code LANGUAGE_CODES[] = {
    {"Arabic", "Ar"},    {"Danish", "Da"},   {"German", "De"},     {"Greek", "El"},
    {"English", "En"},   {"Spanish", "Es"},  {"Finnish", "Fi"},    {"French", "Fr"},
    {"Italian", "It"},   {"Japanese", "Ja"}, {"Korean", "Ko"},     {"Dutch", "Nl"},
    {"Norwegian", "No"}, {"Polish", "Pl"},   {"Portuguese", "Pt"}, {"Russian", "Ru"},
    {"Serbian", "Sr"},   {"Swedish", "Sv"},  {"Chinese", "Zh"},
};
/* RomM's tag for a game without language (a "nolang" file name): nothing to show. */
#define LANGUAGE_NONE "No Language"

static const char *language_shown(const char *name) {
    if (strcmp(name, LANGUAGE_NONE) == 0) {
        return "";
    }
    for (size_t i = 0; i < sizeof LANGUAGE_CODES / sizeof LANGUAGE_CODES[0]; i++) {
        if (strcmp(name, LANGUAGE_CODES[i].name) == 0) {
            return LANGUAGE_CODES[i].code;
        }
    }
    return name;
}

/* The texts of a list joined with ", " into out (SKIFF_TEXT_MAX), empty ones left out. */
static void join_list(const char (*texts)[SKIFF_ROMM_FACT_MAX], size_t count, int languages,
                      char *out) {
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        const char *text = languages ? language_shown(texts[i]) : texts[i];
        if (text[0] == '\0') {
            continue;
        }
        const int written =
            snprintf(out + used, SKIFF_TEXT_MAX - used, "%s%s", used > 0 ? ", " : "", text);
        if (written < 0 || (size_t)written >= SKIFF_TEXT_MAX - used) {
            return;
        }
        used += (size_t)written;
    }
}

/* The most facts a game shows: year, genres, players, rating, companies, regions, languages and
 * size. */
#define DETAILS_FACTS_MAX 8

/* The game's facts, each in display form, in the order they are shown; only the ones RomM has,
 * and the size, which every game has. */
static size_t details_facts(const skiff_app *app, char (*facts)[SKIFF_TEXT_MAX]) {
    const skiff_romm_rom_metadata *m = &app->rom.metadata;
    size_t count = 0;
    if (m->release_year > 0) {
        snprintf(facts[count++], SKIFF_TEXT_MAX, "%d", m->release_year);
    }
    join_list(m->genres, m->genre_count, 0, facts[count]);
    count += facts[count][0] != '\0';
    if (m->players[0] != '\0') {
        const char *args[] = {m->players};
        app_format(app, SKIFF_TEXT_DETAILS_PLAYERS, args, 1, facts[count++]);
    }
    if (m->rating > 0) {
        char rating[24];
        char scale[24];
        snprintf(rating, sizeof rating, "%u", m->rating);
        snprintf(scale, sizeof scale, "%u", (unsigned)SKIFF_ROMM_RATING_SCALE);
        const char *args[] = {rating, scale};
        app_format(app, SKIFF_TEXT_DETAILS_RATING, args, 2, facts[count++]);
    }
    /* "Developer / Publisher", or the one RomM has, or one name for a studio that is both. */
    const int both = m->developer[0] != '\0' && m->publisher[0] != '\0' &&
                     strcmp(m->developer, m->publisher) != 0;
    if (m->developer[0] != '\0' || m->publisher[0] != '\0') {
        snprintf(facts[count++], SKIFF_TEXT_MAX, "%s%s%s",
                 m->developer[0] != '\0' ? m->developer : m->publisher, both ? " / " : "",
                 both ? m->publisher : "");
    }
    join_list(m->regions, m->region_count, 0, facts[count]);
    count += facts[count][0] != '\0';
    join_list(m->languages, m->language_count, 1, facts[count]);
    count += facts[count][0] != '\0';
    char size[SKIFF_APP_DETAIL_MAX];
    app_format_bytes(app, app->rom.summary.size, size, sizeof size);
    const char *args[] = {size};
    app_format(app, SKIFF_TEXT_DETAILS_SIZE, args, 1, facts[count++]);
    return count;
}

/* A line of the view, text fitted to width; nothing once the view is full. */
static void add_fitted_line(skiff_app *app, const char *text, float width) {
    skiff_app_view *view = &app->view;
    if (lines_room(view) > 0) {
        app_fit(app, text, width, view->lines[view->line_count++], SKIFF_TEXT_MAX);
    }
}

/* Appends as much of text to the string in out (size bytes) as fits: a line longer than the view's
 * is fitted to its width afterwards, never shown cut here. */
static void append_text(char *out, size_t size, const char *text) {
    const size_t used = strnlen(out, size - 1);
    const size_t length = strnlen(text, size - 1 - used);
    memcpy(out + used, text, length);
    out[used + length] = '\0';
}

/* The game's facts joined with APP_DETAILS_FACT_SEPARATOR, a fact never split across lines: a
 * fact that does not fit after the others on a line starts the next one, and the last of
 * APP_DETAILS_FACT_LINES takes what is left, cut with "..." when it is too long. */
static void add_facts(skiff_app *app, float width) {
    char facts[DETAILS_FACTS_MAX][SKIFF_TEXT_MAX];
    const size_t count = details_facts(app, facts);
    char line[2 * SKIFF_TEXT_MAX];
    line[0] = '\0';
    size_t lines = 0;
    for (size_t i = 0; i < count; i++) {
        char joined[sizeof line];
        memcpy(joined, line, sizeof line);
        if (line[0] != '\0') {
            append_text(joined, sizeof joined, APP_DETAILS_FACT_SEPARATOR);
        }
        append_text(joined, sizeof joined, facts[i]);
        const int last_line = lines + 1 >= APP_DETAILS_FACT_LINES;
        if (line[0] != '\0' && !last_line && app->env.measure(app->env.ctx, joined) > width) {
            add_fitted_line(app, line, width);
            lines++;
            line[0] = '\0';
            append_text(line, sizeof line, facts[i]);
        } else {
            memcpy(line, joined, sizeof line);
        }
    }
    add_fitted_line(app, line, width);
}

/* The game's description under a rule, as much of it as the body has room for from the line the
 * player scrolled to, marked by arrows where more is above or below. Wrapped once per game. */
static void add_description(skiff_app *app) {
    skiff_app_view *view = &app->view;
    app_description *description = &app->description;
    const char *text = app->rom.metadata.description;
    description->window = 0;
    if (!description->wrapped) {
        description->line_count =
            skiff_app_wrap_spans(text, APP_DESCRIPTION_WIDTH, app->env.measure, app->env.ctx,
                                 description->lines, APP_DESCRIPTION_LINES_MAX, &description->cut);
        description->wrapped = 1;
    }
    /* The rule's line and at least one of text, in what the lines above (the name a heading) left
     * of the body. */
    if (description->line_count == 0 || lines_room(view) < 2) {
        return;
    }
    view->lines[view->line_count++][0] = '\0';
    const size_t room = lines_room(view);
    const size_t window = room < description->line_count ? room : description->line_count;
    const size_t last_first = description->line_count - window;
    if (description->first > last_first) {
        description->first = last_first;
    }
    description->window = window;
    view->has_scroll = 1;
    view->scroll_line = view->line_count;
    view->scroll_width = SKIFF_APP_COVER_TEXT_WIDTH;
    view->more_above = description->first > 0;
    view->more_below = description->first + window < description->line_count;
    for (size_t i = 0; i < window; i++) {
        const size_t index = description->first + i;
        const skiff_app_span *span = &description->lines[index];
        char *line = view->lines[view->line_count++];
        memcpy(line, text + span->start, span->length);
        line[span->length] = '\0';
        if (description->cut && index + 1 == description->line_count) {
            char ended[SKIFF_TEXT_MAX + sizeof SKIFF_UI_ELLIPSIS];
            snprintf(ended, sizeof ended, "%s" SKIFF_UI_ELLIPSIS, line);
            app_fit(app, ended, APP_DESCRIPTION_WIDTH, line, SKIFF_TEXT_MAX);
        }
    }
}

_Static_assert(SKIFF_APP_BLOCK_GAP + APP_DETAILS_NAME_LINES * SKIFF_APP_HEADING_LINE_HEIGHT +
                       SKIFF_APP_HEADING_GAP +
                       (APP_DETAILS_NAME_LINES + APP_DETAILS_FACT_LINES + 2) *
                           SKIFF_APP_LINE_HEIGHT <=
                   SKIFF_APP_BODY_HEIGHT,
               "the name, the file name, the facts, the state and the free space fit the body");

/* "Downloads as CSO: RomM converts it first" for a game RomM converts; 0 for one it does not. */
static int conversion_line(const skiff_app *app, char *out) {
    const char *name = skiff_romm_format_name(app_details_format(app));
    if (name == NULL) {
        return 0;
    }
    char upper[8];
    size_t i = 0;
    for (; name[i] != '\0' && i + 1 < sizeof upper; i++) {
        upper[i] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 'a' + 'A' : name[i]);
    }
    upper[i] = '\0';
    const char *args[] = {upper};
    app_format(app, SKIFF_TEXT_DETAILS_CONVERTED, args, 1, out);
    return 1;
}

static void details_view(skiff_app *app) {
    skiff_app_view *view = &app->view;
    set_title(app, app_text(app, SKIFF_TEXT_TITLE_DETAILS));
    const skiff_romm_rom_summary *rom = &app->rom.summary;
    /* The cover box is there from the first frame, so the text never re-wraps when it arrives. */
    const float width = SKIFF_APP_COVER_TEXT_WIDTH;
    view->has_cover_box = 1;
    view->cover = app_cover_shown(app) ? app->cover : NULL;
    add_name_within(app, rom->name[0] != '\0' ? rom->name : rom->fs_name, width,
                    SKIFF_APP_LINE_HEADING);
    if (!app->has_rom) {
        add_text_within(app, SKIFF_TEXT_LIBRARY_LOADING, width);
        add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
        return;
    }
    if (rom->name[0] != '\0' && strcmp(rom->name, rom->fs_name) != 0) {
        add_name_within(app, rom->fs_name, width, SKIFF_APP_LINE_TEXT);
    }
    add_facts(app, width);
    const skiff_text_id refusal = app_details_refusal(app);
    char detail[SKIFF_APP_DETAIL_MAX];
    app_rom_detail(app, rom, detail, sizeof detail);
    char converted[SKIFF_TEXT_MAX];
    if (refusal != SKIFF_TEXT_COUNT) {
        add_text_within(app, refusal, width);
    } else if (detail[0] != '\0') {
        add_wrapped_within(app, detail, width);
    } else if (conversion_line(app, converted)) {
        add_wrapped_within(app, converted, width);
    }
    char save[SKIFF_TEXT_MAX];
    if (app_saves_details_line(app, save)) {
        add_wrapped_within(app, save, width);
    }
    add_free_space_within(app, width);
    add_description(app);
    if (refusal == SKIFF_TEXT_COUNT) {
        add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_DOWNLOAD);
    }
    if (app_saves_details_offered(app)) {
        add_hint(app, SKIFF_UI_ACTION_EXTRA, SKIFF_TEXT_SYNC_SAVE);
    }
    if (view->more_above || view->more_below) {
        add_hint(app, SKIFF_UI_ACTION_UP | SKIFF_UI_ACTION_DOWN, SKIFF_TEXT_SCROLL);
    }
    add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
}

static void job_detail(const skiff_app *app, const skiff_job *job, char *out, size_t size) {
    char text[SKIFF_TEXT_MAX];
    char code[16];
    snprintf(code, sizeof code, "%d", (int)job->error);
    const char *args[] = {code};
    switch (job->state) {
    case SKIFF_JOB_QUEUED:
        app_format(app, SKIFF_TEXT_QUEUE_WAITING, NULL, 0, text);
        break;
    case SKIFF_JOB_ACTIVE:
        app_format(app, SKIFF_TEXT_QUEUE_DOWNLOADING, NULL, 0, text);
        break;
    case SKIFF_JOB_DONE:
        app_format(app, SKIFF_TEXT_INSTALLED, NULL, 0, text);
        break;
    case SKIFF_JOB_FAILED:
        app_format(app, SKIFF_TEXT_QUEUE_FAILED, args, 1, text);
        break;
    case SKIFF_JOB_CANCELLED:
        snprintf(text, sizeof text, "%s",
                 skiff_error_text(app->config.language, SKIFF_ERR_CANCELLED));
        break;
    }
    app_fit(app, text, SKIFF_APP_DETAIL_WIDTH, out, size);
}

static void progress_lines(skiff_app *app) {
    skiff_app_view *view = &app->view;
    const skiff_ui_progress *progress = &app->queue.progress;
    char done[SKIFF_APP_DETAIL_MAX];
    char total[SKIFF_APP_DETAIL_MAX];
    char percent[8];
    app_format_bytes(app, progress->done, done, sizeof done);
    app_format_bytes(app, progress->total, total, sizeof total);
    view->percent = skiff_ui_progress_percent(progress);
    view->has_progress = 1;
    snprintf(percent, sizeof percent, "%u", view->percent);
    const char *args[] = {done, total, percent};
    add_formatted(app, SKIFF_TEXT_PROGRESS, args, 3);
    uint64_t seconds = 0;
    if (progress->has_rate && skiff_ui_progress_eta(progress, &seconds)) {
        char rate[SKIFF_APP_DETAIL_MAX];
        char left[SKIFF_APP_DETAIL_MAX];
        app_format_bytes(app, progress->rate, rate, sizeof rate);
        if (skiff_ui_format_duration(seconds, left, sizeof left) != SKIFF_OK) {
            left[0] = '\0';
        }
        const char *rate_args[] = {rate, left};
        add_formatted(app, SKIFF_TEXT_PROGRESS_RATE, rate_args, 2);
    }
}

static void recovery_line(skiff_app *app) {
    const skiff_jobs_event *recovery = &app->queue.recovery;
    switch (recovery->step) {
    case SKIFF_JOBS_WAITING_FOR_WIFI:
        add_text(app, SKIFF_TEXT_QUEUE_WAITING_WIFI);
        break;
    case SKIFF_JOBS_REJOINING:
    case SKIFF_JOBS_RELOADING:
        add_text(app, SKIFF_TEXT_QUEUE_REJOINING);
        break;
    case SKIFF_JOBS_RETRYING:
    case SKIFF_JOBS_CONVERTING: {
        char in[SKIFF_APP_DETAIL_MAX];
        if (skiff_ui_format_duration((recovery->retry_in_ms + APP_MS_PER_S - 1) / APP_MS_PER_S, in,
                                     sizeof in) != SKIFF_OK) {
            in[0] = '\0';
        }
        const char *args[] = {in};
        add_formatted(app,
                      recovery->step == SKIFF_JOBS_CONVERTING ? SKIFF_TEXT_QUEUE_CONVERTING
                                                              : SKIFF_TEXT_QUEUE_RETRYING,
                      args, 1);
        break;
    }
    }
}

static void queue_view(skiff_app *app) {
    skiff_app_view *view = &app->view;
    const app_queue_view *queue = &app->queue;
    set_title(app, app_text(app, SKIFF_TEXT_TITLE_QUEUE));
    if (queue->count == 0) {
        add_text(app, SKIFF_TEXT_QUEUE_EMPTY);
        add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
        return;
    }
    char count[24];
    snprintf(count, sizeof count, "%zu", queue->count);
    const char *count_args[] = {count};
    char text[SKIFF_TEXT_MAX];
    app_format(app, SKIFF_TEXT_QUEUE_COUNT, count_args, 1, text);
    set_status(app, text);
    const skiff_job *job = &queue->jobs[app->queue_list.selected];
    if (job->state == SKIFF_JOB_ACTIVE) {
        if (job->id == queue->progress_job) {
            progress_lines(app);
        } else {
            /* No bytes yet (RomM may be converting it): an empty bar, under what holds it up. */
            view->has_progress = 1;
            view->percent = 0;
        }
        if (queue->has_recovery && queue->recovery.job_id == job->id) {
            recovery_line(app);
        }
    } else if (job->state == SKIFF_JOB_FAILED) {
        add_error(app, job->error);
    }
    /* The rows leave this much room under them; the row's own detail still names the error. */
    if (view->line_count > APP_QUEUE_DETAIL_LINES) {
        view->line_count = APP_QUEUE_DETAIL_LINES;
    }
    view->lines_below = 1;
    view->has_list = 1;
    view->list = app->queue_list;
    for (size_t i = 0;
         i < app->queue_list.rows && app->queue_list.first + i < app->queue_list.count; i++) {
        const size_t index = app->queue_list.first + i;
        skiff_app_row *row = &view->rows[view->row_count++];
        snprintf(row->label, sizeof row->label, "%s", queue->labels[index]);
        job_detail(app, &queue->jobs[index], row->detail, sizeof row->detail);
    }
    if (job->state == SKIFF_JOB_FAILED || job->state == SKIFF_JOB_CANCELLED) {
        add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_RETRY);
    }
    if (job->state == SKIFF_JOB_QUEUED || job->state == SKIFF_JOB_ACTIVE ||
        job->state == SKIFF_JOB_FAILED) {
        add_hint(app, SKIFF_UI_ACTION_EXTRA, SKIFF_TEXT_CANCEL_DOWNLOAD);
    }
    add_hint(app, SKIFF_UI_ACTION_MENU, SKIFF_TEXT_CLEAR_FINISHED);
    add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
}

/* Saves starts a new group (with Games from RomM): the list never scrolls, with room for the gap
 * above it. */
_Static_assert(SKIFF_APP_BLOCK_GAP + SKIFF_APP_LINE_HEIGHT * APP_SETTINGS_ROW_COUNT <=
                   SKIFF_APP_LINE_HEIGHT * APP_SETTINGS_ROWS,
               "Settings' rows and the gap above Saves must fit its list");

static void settings_view(skiff_app *app) {
    skiff_app_view *view = &app->view;
    set_title(app, app_text(app, SKIFF_TEXT_TITLE_SETTINGS));
    const char *version[] = {skiff_version_string()};
    add_formatted(app, SKIFF_TEXT_SETTINGS_VERSION, version, 1);
    if (app->has_server) {
        const char *romm[] = {app->server.version};
        add_formatted(app, SKIFF_TEXT_SETTINGS_ROMM, romm, 1);
    }
    add_free_space(app);
    skiff_ui_list_set_count(&app->settings_list, APP_SETTINGS_ROW_COUNT);
    view->has_list = 1;
    view->list = app->settings_list;
    char text[SKIFF_TEXT_MAX];
    if (app->settings.server_url[0] != '\0') {
        const char *url[] = {app->settings.server_url};
        app_format(app, SKIFF_TEXT_SERVER_CURRENT, url, 1, text);
    } else {
        snprintf(text, sizeof text, "%s", app_text(app, SKIFF_TEXT_SERVER_NONE));
    }
    app_fit(app, text, SKIFF_APP_LABEL_WIDTH, view->rows[0].label, sizeof view->rows[0].label);
    app_fit(app, app_text(app, SKIFF_TEXT_PAIR_AGAIN), SKIFF_APP_LABEL_WIDTH, view->rows[1].label,
            sizeof view->rows[1].label);
    view->rows[1].dim = app->settings.server_url[0] == '\0';
    app_fit(app, app_text(app, SKIFF_TEXT_TITLE_SAVES), SKIFF_APP_LABEL_WIDTH, view->rows[2].label,
            sizeof view->rows[2].label);
    view->rows[2].dim = app->connect != CONNECT_DONE;
    view->rows[2].new_group = 1;
    const size_t waiting = app_saves_waiting(app);
    if (waiting > 0) {
        char count[24];
        snprintf(count, sizeof count, "%zu", waiting);
        const char *args[] = {count};
        app_format(app, waiting == 1 ? SKIFF_TEXT_SAVES_WAITING_ONE : SKIFF_TEXT_SAVES_WAITING,
                   args, 1, text);
        app_fit(app, text, SKIFF_APP_DETAIL_WIDTH, view->rows[2].detail,
                sizeof view->rows[2].detail);
        view->rows[2].attention = 1;
    }
    app_sends_row(app, view->rows[3].label, sizeof view->rows[3].label, &view->rows[3].dim);
    view->row_count = APP_SETTINGS_ROW_COUNT;
    add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_SELECT);
    add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
}

static skiff_text_id action_label(app_message_action action) {
    switch (action) {
    case MESSAGE_RETRY_CONNECT:
    case MESSAGE_RETRY_REQUEST:
    case MESSAGE_RETRY_WORKER:
        return SKIFF_TEXT_RETRY;
    case MESSAGE_PICK_NETWORK:
        return SKIFF_TEXT_CHOOSE_NETWORK;
    case MESSAGE_NEW_PAIRING:
        return SKIFF_TEXT_NEW_CODE;
    case MESSAGE_SETTINGS:
        return SKIFF_TEXT_SETTINGS;
    case MESSAGE_QUIT:
        return SKIFF_TEXT_QUIT;
    case MESSAGE_BACK:
    case MESSAGE_RESUME:
    case MESSAGE_NOTICE_SEEN:
    case MESSAGE_START_CONTINUE:
    case MESSAGE_NONE:
        break;
    }
    return SKIFF_TEXT_OK;
}

static void message_view(skiff_app *app) {
    skiff_app_view *view = &app->view;
    const app_message *message = &app->message;
    set_title(app, app_text(app, message->title));
    for (size_t i = 0; i < message->line_count; i++) {
        snprintf(view->lines[i], SKIFF_TEXT_MAX, "%s", message->lines[i]);
    }
    view->line_count = message->line_count;
    add_hint(app, SKIFF_UI_ACTION_CONFIRM, action_label(message->ok));
    if (message->other != MESSAGE_NONE) {
        add_hint(app, message->other == MESSAGE_BACK ? SKIFF_UI_ACTION_BACK : SKIFF_UI_ACTION_MENU,
                 message->other_label);
    }
}

static void confirm_view(skiff_app *app) {
    set_title(app, app_text(app, SKIFF_TEXT_TITLE_CONFIRM));
    if (app->confirm == CONFIRM_DOWNLOAD_ALL) {
        char lines[SKIFF_APP_LINES_MAX][SKIFF_TEXT_MAX];
        const size_t count = app_batch_lines(app, lines, SKIFF_APP_LINES_MAX);
        for (size_t i = 0; i < count; i++) {
            add_wrapped_within(app, lines[i], SKIFF_APP_TEXT_WIDTH);
        }
    } else {
        add_text(app, app->confirm_text);
    }
    add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_OK);
    add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_CANCEL);
}

void app_view_build(skiff_app *app) {
    skiff_app_view *view = &app->view;
    memset(view, 0, sizeof *view);
    view->screen = app->screen;
    switch (app->screen) {
    case SKIFF_APP_SCREEN_STARTING:
        set_title(app, APP_TITLE);
        add_text(app, SKIFF_TEXT_LIBRARY_LOADING);
        break;
    case SKIFF_APP_SCREEN_SERVER:
        server_view(app);
        break;
    case SKIFF_APP_SCREEN_CONNECTING:
        connecting_view(app);
        break;
    case SKIFF_APP_SCREEN_PAIR:
        pair_view(app);
        break;
    case SKIFF_APP_SCREEN_LIBRARY:
        library_view(app);
        break;
    case SKIFF_APP_SCREEN_DETAILS:
        details_view(app);
        break;
    case SKIFF_APP_SCREEN_QUEUE:
        queue_view(app);
        break;
    case SKIFF_APP_SCREEN_SETTINGS:
        settings_view(app);
        break;
    case SKIFF_APP_SCREEN_MESSAGE:
        message_view(app);
        break;
    case SKIFF_APP_SCREEN_CONFIRM:
        confirm_view(app);
        break;
    case SKIFF_APP_SCREEN_SAVES:
        app_saves_view(app);
        break;
    case SKIFF_APP_SCREEN_SAVE_BACKUPS:
        app_saves_backups_view(app);
        break;
    case SKIFF_APP_SCREEN_SAVE_CHOICE:
        app_saves_choice_view(app);
        break;
    }
    /* "Checking saves...", "Syncing ...": in the header while the browsing screens show. */
    const int browsing =
        app->screen == SKIFF_APP_SCREEN_LIBRARY || app->screen == SKIFF_APP_SCREEN_DETAILS ||
        app->screen == SKIFF_APP_SCREEN_SAVES || app->screen == SKIFF_APP_SCREEN_SAVE_BACKUPS;
    if (browsing && app->saves->activity.text[0] != '\0' && app->batch.step == BATCH_NONE) {
        set_header_text(app, &app->saves->activity);
    }
    /* Not over the choice between two saves: its header names the game it asks about. */
    if (app->note.text[0] != '\0' && app->screen != SKIFF_APP_SCREEN_SAVE_CHOICE) {
        set_header_text(app, &app->note);
    }
    view->dialog = app->dialog;
    if (app->dialog == SKIFF_APP_DIALOG_KEYBOARD) {
        snprintf(view->dialog_title, sizeof view->dialog_title, "%s",
                 app_text(app, SKIFF_TEXT_SERVER_KEYBOARD));
        snprintf(view->dialog_text, sizeof view->dialog_text, "%s",
                 app->settings.server_url[0] != '\0' ? app->settings.server_url : APP_URL_PREFILL);
    }
}
