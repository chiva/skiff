#include "skiff/ui.h"

#include <stdio.h>
#include <string.h>

#define BYTES_PER_KB 1024ULL
#define BYTES_PER_MB (BYTES_PER_KB * 1024ULL)
#define BYTES_PER_GB (BYTES_PER_MB * 1024ULL)
/* Sizes below 100 units show a tenth: "1.5 MB", "12.3 GB", but "640 MB". */
#define TENTHS_PER_UNIT 10ULL
#define TENTHS_SHOWN_BELOW 1000ULL
#define PERCENT_FULL 100U
#define MS_PER_S 1000ULL
#define SECONDS_PER_MINUTE 60ULL
#define MINUTES_PER_HOUR 60ULL
/* Each new window counts for a quarter of the rate, so one slow window does not swing it. */
#define RATE_SMOOTHING_OLD_WEIGHT 3ULL
#define RATE_SMOOTHING_TOTAL_WEIGHT 4ULL

/* Directions and shoulder buttons: the ones that repeat when held. */
#define MOVE_BUTTONS                                                                               \
    (SKIFF_UI_BUTTON_UP | SKIFF_UI_BUTTON_DOWN | SKIFF_UI_BUTTON_LEFT | SKIFF_UI_BUTTON_RIGHT |    \
     SKIFF_UI_BUTTON_L | SKIFF_UI_BUTTON_R)

/* ---- Buttons and actions ---- */

typedef struct button_action {
    unsigned button;
    unsigned action;
} button_action;

/* Every button but Circle and Cross, whose meaning depends on the console's setting. */
static const button_action FIXED_ACTIONS[] = {
    {SKIFF_UI_BUTTON_UP, SKIFF_UI_ACTION_UP},
    {SKIFF_UI_BUTTON_DOWN, SKIFF_UI_ACTION_DOWN},
    {SKIFF_UI_BUTTON_LEFT, SKIFF_UI_ACTION_LEFT},
    {SKIFF_UI_BUTTON_RIGHT, SKIFF_UI_ACTION_RIGHT},
    {SKIFF_UI_BUTTON_L, SKIFF_UI_ACTION_PAGE_UP},
    {SKIFF_UI_BUTTON_R, SKIFF_UI_ACTION_PAGE_DOWN},
    {SKIFF_UI_BUTTON_TRIANGLE, SKIFF_UI_ACTION_MENU},
    {SKIFF_UI_BUTTON_SQUARE, SKIFF_UI_ACTION_EXTRA},
    {SKIFF_UI_BUTTON_START, SKIFF_UI_ACTION_START},
    {SKIFF_UI_BUTTON_SELECT, SKIFF_UI_ACTION_SELECT},
};

void skiff_ui_input_init(skiff_ui_input *input, int confirm_is_cross) {
    if (input == NULL) {
        return;
    }
    memset(input, 0, sizeof *input);
    input->confirm_is_cross = confirm_is_cross != 0;
}

unsigned skiff_ui_input_update(skiff_ui_input *input, unsigned buttons) {
    if (input == NULL) {
        return 0;
    }
    const unsigned pressed = buttons & ~input->held;
    const unsigned moves = buttons & MOVE_BUTTONS;
    unsigned repeated = 0;
    if (moves != 0 && moves == input->held_moves) {
        input->held_frames++;
        if (input->held_frames >= SKIFF_UI_REPEAT_DELAY_FRAMES &&
            (input->held_frames - SKIFF_UI_REPEAT_DELAY_FRAMES) % SKIFF_UI_REPEAT_INTERVAL_FRAMES ==
                0) {
            repeated = moves;
        }
    } else {
        input->held_frames = 0;
    }
    input->held = buttons;
    input->held_moves = moves;

    const unsigned active = pressed | repeated;
    unsigned actions = 0;
    for (size_t i = 0; i < sizeof FIXED_ACTIONS / sizeof FIXED_ACTIONS[0]; i++) {
        if ((active & FIXED_ACTIONS[i].button) != 0) {
            actions |= FIXED_ACTIONS[i].action;
        }
    }
    const unsigned confirm =
        input->confirm_is_cross ? SKIFF_UI_BUTTON_CROSS : SKIFF_UI_BUTTON_CIRCLE;
    const unsigned back = input->confirm_is_cross ? SKIFF_UI_BUTTON_CIRCLE : SKIFF_UI_BUTTON_CROSS;
    if ((active & confirm) != 0) {
        actions |= SKIFF_UI_ACTION_CONFIRM;
    }
    if ((active & back) != 0) {
        actions |= SKIFF_UI_ACTION_BACK;
    }
    return actions;
}

/* ---- A scrolling list ---- */

static void keep_visible(skiff_ui_list *list) {
    if (list->count == 0) {
        list->selected = 0;
        list->first = 0;
        return;
    }
    if (list->selected < list->first) {
        list->first = list->selected;
    } else if (list->selected >= list->first + list->rows) {
        list->first = list->selected - list->rows + 1;
    }
    const size_t last_first = list->count > list->rows ? list->count - list->rows : 0;
    if (list->first > last_first) {
        list->first = last_first;
    }
}

void skiff_ui_list_init(skiff_ui_list *list, size_t count, size_t rows) {
    if (list == NULL) {
        return;
    }
    list->count = count;
    list->rows = rows > 0 ? rows : 1;
    list->selected = 0;
    list->first = 0;
}

void skiff_ui_list_set_count(skiff_ui_list *list, size_t count) {
    if (list == NULL) {
        return;
    }
    list->count = count;
    if (list->selected >= count) {
        list->selected = count > 0 ? count - 1 : 0;
    }
    keep_visible(list);
}

void skiff_ui_list_select(skiff_ui_list *list, size_t index) {
    if (list == NULL) {
        return;
    }
    list->selected = index;
    skiff_ui_list_set_count(list, list->count);
}

int skiff_ui_list_apply(skiff_ui_list *list, unsigned actions) {
    if (list == NULL || list->count == 0) {
        return 0;
    }
    const size_t before = list->selected;
    if ((actions & SKIFF_UI_ACTION_UP) != 0) {
        list->selected = list->selected > 0 ? list->selected - 1 : list->count - 1;
    }
    if ((actions & SKIFF_UI_ACTION_DOWN) != 0) {
        list->selected = list->selected + 1 < list->count ? list->selected + 1 : 0;
    }
    if ((actions & SKIFF_UI_ACTION_PAGE_UP) != 0) {
        list->selected = list->selected > list->rows ? list->selected - list->rows : 0;
    }
    if ((actions & SKIFF_UI_ACTION_PAGE_DOWN) != 0) {
        list->selected = list->count - 1 - list->selected > list->rows ? list->selected + list->rows
                                                                       : list->count - 1;
    }
    keep_visible(list);
    return list->selected != before;
}

/* ---- Text cut to a width ---- */

static int is_continuation(char c) { return ((unsigned char)c & 0xC0U) == 0x80U; }

/* The start of the character holding byte position (position itself when it starts one). */
static size_t boundary_at_or_before(const char *text, size_t position) {
    while (position > 0 && is_continuation(text[position])) {
        position--;
    }
    return position;
}

/* The start of the character after the one at position. */
static size_t next_boundary(const char *text, size_t position) {
    position++;
    while (text[position] != '\0' && is_continuation(text[position])) {
        position++;
    }
    return position;
}

/* out = the first length bytes of text, blanks at the end dropped, then the ellipsis. */
static void compose_cut(const char *text, size_t length, char *out) {
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t')) {
        length--;
    }
    memcpy(out, text, length);
    memcpy(out + length, SKIFF_UI_ELLIPSIS, sizeof SKIFF_UI_ELLIPSIS);
}

static int cut_fits(const char *text, size_t length, float max_width, skiff_ui_measure_fn measure,
                    void *ctx, char *out) {
    compose_cut(text, length, out);
    return measure(ctx, out) <= max_width;
}

skiff_err skiff_ui_fit_text(const char *text, float max_width, skiff_ui_measure_fn measure,
                            void *measure_ctx, char *out, size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (text == NULL || measure == NULL || out == NULL || out_size < sizeof SKIFF_UI_ELLIPSIS) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const size_t length = strlen(text);
    if (length < out_size && measure(measure_ctx, text) <= max_width) {
        memcpy(out, text, length + 1);
        return SKIFF_OK;
    }
    if (!cut_fits(text, 0, max_width, measure, measure_ctx, out)) {
        out[0] = '\0';
        return SKIFF_OK;
    }
    /* The longest cut that fits, searched over character starts: lo always fits. */
    const size_t room = out_size - sizeof SKIFF_UI_ELLIPSIS;
    size_t lo = 0;
    size_t hi = boundary_at_or_before(text, length < room ? length : room);
    while (lo < hi) {
        size_t mid = boundary_at_or_before(text, lo + (hi - lo + 1) / 2);
        if (mid <= lo) {
            mid = next_boundary(text, lo);
        }
        if (cut_fits(text, mid, max_width, measure, measure_ctx, out)) {
            lo = mid;
        } else {
            hi = boundary_at_or_before(text, mid - 1);
        }
    }
    compose_cut(text, lo, out);
    return SKIFF_OK;
}

/* ---- UTF-16, the system dialogs' text ---- */

#define UTF8_CONTINUATION 0x80U
#define UTF8_CONTINUATION_LAST 0xBFU
#define UTF8_PAYLOAD_MASK 0x3FU
#define UTF8_PAYLOAD_BITS 6U
#define UTF8_LEAD_TWO 0xC0U
#define UTF8_LEAD_THREE 0xE0U
#define UTF8_LEAD_FOUR 0xF0U
#define CODE_POINT_LAST_ONE_BYTE 0x7FU
#define CODE_POINT_LAST_TWO_BYTES 0x7FFU
#define CODE_POINT_LAST_BMP 0xFFFFU
#define SURROGATE_HIGH_FIRST 0xD800U
#define SURROGATE_LOW_FIRST 0xDC00U
#define SURROGATE_LAST 0xDFFFU
#define SURROGATE_PAYLOAD_MASK 0x3FFU
#define SURROGATE_PAYLOAD_BITS 10U
#define SUPPLEMENTARY_FIRST 0x10000U
#define UTF8_MAX_BYTES 4U

/*
 * The well-formed UTF-8 lead bytes and the range their first continuation byte must fall in
 * (Unicode, table 3-7): the narrower ranges rule out overlong forms, surrogates and code points
 * above U+10FFFF. Later continuation bytes are always 0x80 to 0xBF.
 */
typedef struct utf8_lead {
    unsigned char first;
    unsigned char last;
    unsigned char payload_mask;
    unsigned char continuations;
    unsigned char second_low;
    unsigned char second_high;
} utf8_lead;

static const utf8_lead UTF8_LEADS[] = {
    {0xC2, 0xDF, 0x1F, 1, 0x80, 0xBF}, {0xE0, 0xE0, 0x0F, 2, 0xA0, 0xBF},
    {0xE1, 0xEC, 0x0F, 2, 0x80, 0xBF}, {0xED, 0xED, 0x0F, 2, 0x80, 0x9F},
    {0xEE, 0xEF, 0x0F, 2, 0x80, 0xBF}, {0xF0, 0xF0, 0x07, 3, 0x90, 0xBF},
    {0xF1, 0xF3, 0x07, 3, 0x80, 0xBF}, {0xF4, 0xF4, 0x07, 3, 0x80, 0x8F},
};

static const utf8_lead *find_utf8_lead(unsigned lead) {
    for (size_t i = 0; i < sizeof UTF8_LEADS / sizeof UTF8_LEADS[0]; i++) {
        if (lead >= UTF8_LEADS[i].first && lead <= UTF8_LEADS[i].last) {
            return &UTF8_LEADS[i];
        }
    }
    return NULL;
}

/*
 * The character starting at bytes[*position], moving *position past it. An invalid part decodes as
 * the replacement character and is left after its longest valid prefix, so the byte that broke it
 * starts the next character (the terminating NUL included).
 */
static uint32_t decode_utf8(const unsigned char *bytes, size_t *position) {
    const unsigned lead = bytes[*position];
    (*position)++;
    if (lead <= CODE_POINT_LAST_ONE_BYTE) {
        return lead;
    }
    const utf8_lead *form = find_utf8_lead(lead);
    if (form == NULL) {
        return SKIFF_UI_REPLACEMENT_CHARACTER;
    }
    uint32_t code = lead & form->payload_mask;
    unsigned low = form->second_low;
    unsigned high = form->second_high;
    for (unsigned i = 0; i < form->continuations; i++) {
        const unsigned byte = bytes[*position];
        if (byte < low || byte > high) {
            return SKIFF_UI_REPLACEMENT_CHARACTER;
        }
        code = (code << UTF8_PAYLOAD_BITS) | (byte & UTF8_PAYLOAD_MASK);
        (*position)++;
        low = UTF8_CONTINUATION;
        high = UTF8_CONTINUATION_LAST;
    }
    return code;
}

skiff_err skiff_ui_utf8_to_utf16(const char *text, uint16_t *out, size_t out_units,
                                 size_t *length) {
    if (length != NULL) {
        *length = 0;
    }
    if (out != NULL && out_units > 0) {
        out[0] = 0;
    }
    if (text == NULL || out == NULL || out_units == 0) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const unsigned char *bytes = (const unsigned char *)text;
    size_t position = 0;
    size_t used = 0;
    while (bytes[position] != '\0') {
        const uint32_t code = decode_utf8(bytes, &position);
        const size_t units = code > CODE_POINT_LAST_BMP ? 2 : 1;
        if (used + units >= out_units) {
            out[0] = 0;
            return SKIFF_ERR_BUFFER_TOO_SMALL;
        }
        if (units == 1) {
            out[used++] = (uint16_t)code;
        } else {
            const uint32_t offset = code - SUPPLEMENTARY_FIRST;
            out[used++] = (uint16_t)(SURROGATE_HIGH_FIRST | (offset >> SURROGATE_PAYLOAD_BITS));
            out[used++] = (uint16_t)(SURROGATE_LOW_FIRST | (offset & SURROGATE_PAYLOAD_MASK));
        }
    }
    out[used] = 0;
    if (length != NULL) {
        *length = used;
    }
    return SKIFF_OK;
}

static int is_surrogate(uint32_t unit) {
    return unit >= SURROGATE_HIGH_FIRST && unit <= SURROGATE_LAST;
}

static int is_low_surrogate(uint32_t unit) {
    return unit >= SURROGATE_LOW_FIRST && unit <= SURROGATE_LAST;
}

/* The character starting at text[*index], moving *index past it (a pair is one character). */
static uint32_t decode_utf16(const uint16_t *text, size_t max_units, size_t *index) {
    const uint32_t unit = text[*index];
    (*index)++;
    if (!is_surrogate(unit)) {
        return unit;
    }
    if (is_low_surrogate(unit) || *index >= max_units || !is_low_surrogate(text[*index])) {
        return SKIFF_UI_REPLACEMENT_CHARACTER;
    }
    const uint32_t low = text[*index];
    (*index)++;
    return SUPPLEMENTARY_FIRST + ((unit - SURROGATE_HIGH_FIRST) << SURROGATE_PAYLOAD_BITS) +
           (low - SURROGATE_LOW_FIRST);
}

/* code as UTF-8 into bytes; returns how many it took. */
static size_t encode_utf8(uint32_t code, unsigned char bytes[UTF8_MAX_BYTES]) {
    if (code <= CODE_POINT_LAST_ONE_BYTE) {
        bytes[0] = (unsigned char)code;
        return 1;
    }
    size_t count = code <= CODE_POINT_LAST_TWO_BYTES ? 2 : code <= CODE_POINT_LAST_BMP ? 3 : 4;
    const unsigned lead = count == 2   ? UTF8_LEAD_TWO
                          : count == 3 ? UTF8_LEAD_THREE
                                       : UTF8_LEAD_FOUR;
    for (size_t i = count - 1; i > 0; i--) {
        bytes[i] = (unsigned char)(UTF8_CONTINUATION | (code & UTF8_PAYLOAD_MASK));
        code >>= UTF8_PAYLOAD_BITS;
    }
    bytes[0] = (unsigned char)(lead | code);
    return count;
}

skiff_err skiff_ui_utf16_to_utf8(const uint16_t *text, size_t max_units, char *out,
                                 size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (text == NULL || out == NULL || out_size == 0) {
        return SKIFF_ERR_INVALID_ARG;
    }
    size_t index = 0;
    size_t used = 0;
    while (index < max_units && text[index] != 0) {
        unsigned char bytes[UTF8_MAX_BYTES];
        const size_t count = encode_utf8(decode_utf16(text, max_units, &index), bytes);
        if (used + count >= out_size) {
            out[0] = '\0';
            return SKIFF_ERR_BUFFER_TOO_SMALL;
        }
        for (size_t i = 0; i < count; i++) {
            out[used++] = (char)bytes[i];
        }
    }
    out[used] = '\0';
    return SKIFF_OK;
}

/* ---- Progress ---- */

void skiff_ui_progress_start(skiff_ui_progress *progress, uint64_t done, uint64_t total,
                             uint64_t now_ms) {
    if (progress == NULL) {
        return;
    }
    memset(progress, 0, sizeof *progress);
    progress->done = done;
    progress->total = total;
    progress->window_done = done;
    progress->window_start_ms = now_ms;
}

void skiff_ui_progress_update(skiff_ui_progress *progress, uint64_t done, uint64_t now_ms) {
    if (progress == NULL) {
        return;
    }
    if (done < progress->done || now_ms < progress->window_start_ms) {
        skiff_ui_progress_start(progress, done, progress->total, now_ms);
        return;
    }
    progress->done = done;
    const uint64_t elapsed_ms = now_ms - progress->window_start_ms;
    if (elapsed_ms < SKIFF_UI_RATE_WINDOW_MS) {
        return;
    }
    const uint64_t window_rate = (done - progress->window_done) * MS_PER_S / elapsed_ms;
    progress->rate = progress->has_rate
                         ? (progress->rate * RATE_SMOOTHING_OLD_WEIGHT + window_rate) /
                               RATE_SMOOTHING_TOTAL_WEIGHT
                         : window_rate;
    progress->has_rate = 1;
    progress->window_done = done;
    progress->window_start_ms = now_ms;
}

unsigned skiff_ui_progress_percent(const skiff_ui_progress *progress) {
    if (progress == NULL || progress->total == 0) {
        return 0;
    }
    if (progress->done >= progress->total) {
        return PERCENT_FULL;
    }
    return (unsigned)(progress->done * PERCENT_FULL / progress->total);
}

int skiff_ui_progress_eta(const skiff_ui_progress *progress, uint64_t *seconds) {
    if (progress == NULL || seconds == NULL || !progress->has_rate || progress->rate == 0 ||
        progress->total == 0) {
        return 0;
    }
    const uint64_t left = progress->total > progress->done ? progress->total - progress->done : 0;
    *seconds = (left + progress->rate - 1) / progress->rate;
    return 1;
}

/* snprintf()'s result checked: the whole text fitted. */
static skiff_err fitted(int written, char *out, size_t out_size) {
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    return SKIFF_OK;
}

skiff_err skiff_ui_format_bytes(uint64_t bytes, const char *decimal_separator, char *out,
                                size_t out_size) {
    if (decimal_separator == NULL || out == NULL || out_size == 0) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (bytes < BYTES_PER_KB) {
        return fitted(snprintf(out, out_size, "%llu B", (unsigned long long)bytes), out, out_size);
    }
    if (bytes < BYTES_PER_MB) {
        return fitted(
            snprintf(out, out_size, "%llu KB", (unsigned long long)(bytes / BYTES_PER_KB)), out,
            out_size);
    }
    const uint64_t unit = bytes < BYTES_PER_GB ? BYTES_PER_MB : BYTES_PER_GB;
    const char *symbol = bytes < BYTES_PER_GB ? "MB" : "GB";
    const uint64_t tenths = bytes * TENTHS_PER_UNIT / unit;
    if (tenths < TENTHS_SHOWN_BELOW) {
        return fitted(snprintf(out, out_size, "%llu%s%llu %s",
                               (unsigned long long)(tenths / TENTHS_PER_UNIT), decimal_separator,
                               (unsigned long long)(tenths % TENTHS_PER_UNIT), symbol),
                      out, out_size);
    }
    return fitted(snprintf(out, out_size, "%llu %s", (unsigned long long)(bytes / unit), symbol),
                  out, out_size);
}

skiff_err skiff_ui_format_duration(uint64_t seconds, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (seconds < SECONDS_PER_MINUTE) {
        return fitted(snprintf(out, out_size, "%llu s", (unsigned long long)seconds), out,
                      out_size);
    }
    const uint64_t minutes = (seconds + SECONDS_PER_MINUTE - 1) / SECONDS_PER_MINUTE;
    if (minutes < MINUTES_PER_HOUR) {
        return fitted(snprintf(out, out_size, "%llu min", (unsigned long long)minutes), out,
                      out_size);
    }
    return fitted(snprintf(out, out_size, "%llu h %02llu min",
                           (unsigned long long)(minutes / MINUTES_PER_HOUR),
                           (unsigned long long)(minutes % MINUTES_PER_HOUR)),
                  out, out_size);
}
