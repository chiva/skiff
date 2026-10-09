#include <stdio.h>
#include <string.h>

#include "skiff/version.h"

#include "app_internal.h"

#define APP_TITLE "Skiff"
/* The blank lines on the pairing screen: before the approval line and before the code. */
#define PAIR_GAPS 2

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

/* Bytes of text that fit width on one line, ending at a word where one fits. */
static size_t line_length(const char *text, float width, skiff_ui_measure_fn measure, void *ctx) {
    char candidate[SKIFF_TEXT_MAX];
    const size_t length = strlen(text);
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
        best = longest_fitting(text, word, width, measure, ctx);
    }
    return best;
}

size_t skiff_app_wrap(const char *text, float width, skiff_ui_measure_fn measure, void *measure_ctx,
                      char (*lines)[SKIFF_TEXT_MAX], size_t max_lines) {
    if (text == NULL || measure == NULL || lines == NULL) {
        return 0;
    }
    size_t count = 0;
    const char *rest = text;
    while (count < max_lines) {
        rest += strspn(rest, " ");
        if (*rest == '\0') {
            break;
        }
        if (count + 1 == max_lines) {
            if (skiff_ui_fit_text(rest, width, measure, measure_ctx, lines[count],
                                  SKIFF_TEXT_MAX) != SKIFF_OK) {
                lines[count][0] = '\0';
            }
            count++;
            break;
        }
        size_t length = line_length(rest, width, measure, measure_ctx);
        if (length >= SKIFF_TEXT_MAX) {
            length = SKIFF_TEXT_MAX - 1;
        }
        memcpy(lines[count], rest, length);
        size_t end = length;
        while (end > 0 && lines[count][end - 1] == ' ') {
            end--;
        }
        lines[count][end] = '\0';
        count++;
        rest += length;
    }
    return count;
}

/* ---- Building the view ---- */

/* Wraps text onto the view's lines while it has fewer than limit (at most SKIFF_APP_LINES_MAX). */
static void add_wrapped_upto(skiff_app *app, const char *text, float width, size_t limit) {
    skiff_app_view *view = &app->view;
    if (view->line_count >= limit) {
        return;
    }
    view->line_count += skiff_app_wrap(text, width, app->env.measure, app->env.ctx,
                                       &view->lines[view->line_count], limit - view->line_count);
}

static void add_wrapped_within(skiff_app *app, const char *text, float width) {
    add_wrapped_upto(app, text, width, SKIFF_APP_LINES_MAX);
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

static void add_formatted(skiff_app *app, skiff_text_id id, const char *const *args, size_t count) {
    char text[SKIFF_TEXT_MAX];
    app_format(app, id, args, count, text);
    add_wrapped(app, text);
}

/* An address on lines of its own, while the view has fewer than limit: whole when it fits, else
 * broken before its query ('?') so the pairing code in it stays on one line, else wrapped wherever
 * it must. */
static void add_url_upto(skiff_app *app, const char *url, float width, size_t limit) {
    const char *query = strchr(url, '?');
    if (query == NULL || app->env.measure(app->env.ctx, url) <= width) {
        add_wrapped_upto(app, url, width, limit);
        return;
    }
    char path[SKIFF_ROMM_URL_MAX];
    snprintf(path, sizeof path, "%.*s", (int)(query - url), url);
    add_wrapped_upto(app, path, width, limit);
    add_wrapped_upto(app, query, width, limit);
}

/* An empty line between blocks of text. */
static void add_gap(skiff_app *app) {
    skiff_app_view *view = &app->view;
    if (view->line_count > 0 && view->line_count < SKIFF_APP_LINES_MAX) {
        view->lines[view->line_count++][0] = '\0';
    }
}

static void add_error(skiff_app *app, skiff_err err) {
    char text[SKIFF_TEXT_MAX];
    (void)skiff_error_line(app->config.language, err, text, sizeof text);
    add_wrapped(app, text);
}

static void add_hint(skiff_app *app, unsigned action, skiff_text_id label) {
    skiff_app_view *view = &app->view;
    if (view->hint_count == SKIFF_APP_HINTS_MAX) {
        return;
    }
    view->hints[view->hint_count].action = action;
    snprintf(view->hints[view->hint_count].label, SKIFF_APP_HINT_MAX, "%s", app_text(app, label));
    view->hint_count++;
}

static void set_status(skiff_app *app, const char *text) {
    app_fit(app, text, SKIFF_APP_STATUS_WIDTH, app->view.status, sizeof app->view.status);
}

static void set_title(skiff_app *app, const char *title) {
    snprintf(app->view.title, sizeof app->view.title, "%s", title);
}

static void add_free_space(skiff_app *app) {
    if (!app->free_known) {
        return;
    }
    char size[SKIFF_APP_DETAIL_MAX];
    app_format_bytes(app, app->free_bytes, size, sizeof size);
    const char *args[] = {size};
    add_formatted(app, SKIFF_TEXT_FREE_SPACE, args, 1);
}

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
    add_hint(app, SKIFF_UI_ACTION_START, SKIFF_TEXT_QUIT);
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
        /* The address takes what the lines under it and their two gaps leave, so a long one cannot
         * push the code or an error off the screen. */
        const size_t below = wrapped_lines(app, approve, width) + wrapped_lines(app, code, width) +
                             (error[0] != '\0' ? wrapped_lines(app, error, width) : 0) + PAIR_GAPS;
        /* With the code in it: RomM 5.3.1's page has no field to type the code into. */
        add_url_upto(app, pairing->pairing.verification_url_complete, width,
                     SKIFF_APP_LINES_MAX > below ? SKIFF_APP_LINES_MAX - below : 0);
        add_gap(app);
        add_wrapped_within(app, approve, width);
        add_gap(app);
        add_wrapped_within(app, code, width);
        add_wrapped_within(app, error, width);
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
    add_hint(app, SKIFF_UI_ACTION_START, SKIFF_TEXT_QUIT);
}

static void library_view(skiff_app *app) {
    skiff_app_view *view = &app->view;
    set_title(app, app_text(app, SKIFF_TEXT_TITLE_LIBRARY));
    if (app->total_known) {
        char count[24];
        snprintf(count, sizeof count, "%llu", (unsigned long long)app->total);
        const char *args[] = {count};
        char text[SKIFF_TEXT_MAX];
        app_format(app, SKIFF_TEXT_LIBRARY_COUNT, args, 1, text);
        set_status(app, text);
    }
    if (!app->has_platform || (app->total_known && app->total == 0)) {
        add_text(app, SKIFF_TEXT_LIBRARY_EMPTY);
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
        add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_SELECT);
    }
    add_hint(app, SKIFF_UI_ACTION_EXTRA, SKIFF_TEXT_DOWNLOADS);
    add_hint(app, SKIFF_UI_ACTION_MENU, SKIFF_TEXT_SETTINGS);
    add_hint(app, SKIFF_UI_ACTION_START, SKIFF_TEXT_QUIT);
}

static void details_view(skiff_app *app) {
    set_title(app, app_text(app, SKIFF_TEXT_TITLE_DETAILS));
    const skiff_romm_rom_summary *rom = &app->rom.summary;
    add_wrapped(app, rom->name[0] != '\0' ? rom->name : rom->fs_name);
    if (!app->has_rom) {
        add_text(app, SKIFF_TEXT_LIBRARY_LOADING);
        add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
        return;
    }
    if (rom->name[0] != '\0') {
        add_wrapped(app, rom->fs_name);
    }
    char size[SKIFF_APP_DETAIL_MAX];
    app_format_bytes(app, rom->size, size, sizeof size);
    const char *args[] = {size};
    add_formatted(app, SKIFF_TEXT_DETAILS_SIZE, args, 1);
    const skiff_text_id refusal = app_details_refusal(app);
    char detail[SKIFF_APP_DETAIL_MAX];
    app_rom_detail(app, rom, detail, sizeof detail);
    if (refusal != SKIFF_TEXT_COUNT) {
        add_text(app, refusal);
    } else if (detail[0] != '\0') {
        add_wrapped(app, detail);
    }
    add_free_space(app);
    if (refusal == SKIFF_TEXT_COUNT) {
        add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_DOWNLOAD);
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
    case SKIFF_JOBS_RETRYING: {
        char in[SKIFF_APP_DETAIL_MAX];
        if (skiff_ui_format_duration((recovery->retry_in_ms + APP_MS_PER_S - 1) / APP_MS_PER_S, in,
                                     sizeof in) != SKIFF_OK) {
            in[0] = '\0';
        }
        const char *args[] = {in};
        add_formatted(app, SKIFF_TEXT_QUEUE_RETRYING, args, 1);
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
    if (job->state == SKIFF_JOB_ACTIVE && job->id == queue->progress_job) {
        progress_lines(app);
        if (queue->has_recovery && queue->recovery.job_id == job->id) {
            recovery_line(app);
        }
    } else if (job->state == SKIFF_JOB_FAILED) {
        add_error(app, job->error);
    }
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
    skiff_ui_list_set_count(&app->settings_list, 2);
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
    view->row_count = 2;
    add_hint(app, SKIFF_UI_ACTION_CONFIRM, SKIFF_TEXT_SELECT);
    add_hint(app, SKIFF_UI_ACTION_BACK, SKIFF_TEXT_BACK);
    add_hint(app, SKIFF_UI_ACTION_START, SKIFF_TEXT_QUIT);
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
    add_text(app, app->confirm_text);
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
    }
    if (app->note[0] != '\0') {
        set_status(app, app->note);
    }
    view->dialog = app->dialog;
    if (app->dialog == SKIFF_APP_DIALOG_KEYBOARD) {
        snprintf(view->dialog_title, sizeof view->dialog_title, "%s",
                 app_text(app, SKIFF_TEXT_SERVER_KEYBOARD));
        snprintf(view->dialog_text, sizeof view->dialog_text, "%s",
                 app->settings.server_url[0] != '\0' ? app->settings.server_url : APP_URL_PREFILL);
    }
}
