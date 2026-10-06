/*
 * UI stack prototype (Phase 1 hardware spike). Confirms the UI design in
 * docs/development/architecture.md before the real ui/ layer is written:
 *
 *   - Skiff's renderer (src/platform/psp/ui_psp.h) draws a scrolling list, a progress bar and the
 *     header and footer with intraFont, using the firmware's fonts (flash0:/font/): a Latin font
 *     with the Japanese font as its fallback, so one UTF-8 string can mix both scripts; a title too
 *     long for its row is cut to fit (skiff_ui_fit_text());
 *   - the d-pad and L/R move the selection through skiff/ui.h's input and list models; how long a
 *     frame takes to draw and render is measured;
 *   - the on-screen keyboard (sceUtilityOsk) and the network picker (sceUtilityNetconf) open from
 *     the render loop and close cleanly, with the network modules loaded only for the picker;
 *   - HOME -> Quit ends the loop through the exit callback, like START.
 *
 * It reports, through report.h: heap used by the fonts, system memory before the network modules
 * load, after they load and after they unload (under the app's own PSP_HEAP_SIZE_KB, since it links
 * the same module info), font load times, and the frame times. On the first frame it reads the
 * rendered pixels back and checks that the Latin and the Japanese line both put glyphs on screen.
 * Each dialog step is logged to result.txt as it happens, so a failure names the step.
 *
 * Pass (SKIFF UI PROTO OK): both fonts render; the keyboard was shown, text was typed and
 * confirmed; the network picker was shown and connected (an IP address was obtained); the
 * connection was dropped and the network modules unloaded; no dialog call failed or timed out and
 * no controller read failed. A tester presses START or HOME -> Quit after trying both.
 *
 * Without a button press for HEADLESS_EXIT_US (PPSSPPHeadless in CI, which cannot press buttons)
 * it exits on its own, checking only the fonts and the frames: SKIFF UI PROTO HEADLESS OK.
 */
#include <intraFont.h>
#include <malloc.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspkernel.h>
#include <psputility.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "skiff/selftest.h"
#include "skiff/ui.h"

#include "lifecycle.h"
#include "net_psp.h"
#include "report.h"
#include "ui_psp.h"

#define PROTO_OK_MARKER "SKIFF UI PROTO OK"
#define PROTO_FAIL_MARKER "SKIFF UI PROTO FAIL"
#define PROTO_HEADLESS_OK_MARKER "SKIFF UI PROTO HEADLESS OK"
#define PROTO_HEADLESS_FAIL_MARKER "SKIFF UI PROTO HEADLESS FAIL"

#define LATIN_FONT_PATH "flash0:/font/ltn0.pgf"
#define JAPANESE_FONT_PATH "flash0:/font/jpn0.pgf"
#define TITLE_TEXT "Skiff UI prototype"
#define LATIN_SAMPLE "Latin: The quick brown fox, café, señor"
/* "Japanese: select a game" in UTF-8, drawn by the Latin font's Japanese fallback. */
#define JAPANESE_SAMPLE "日本語: ゲームを選択してください"
/* Japanese characters only, so a missing fallback font leaves its rows empty. */
#define JAPANESE_CHECK_TEXT "日本語のゲームを選択"
/* Too long for a row: drawn cut to fit, as RomM's longer titles will be. */
#define LONG_TITLE_SAMPLE                                                                          \
    "A homebrew title far too long to fit on a single row of the PSP's screen, cut"
#define PROGRESS_LABEL "Progress bar"

/* A clear without the stencil bit leaves the frame buffer's alpha alone, so compare colour only. */
#define COLOUR_RGB_MASK 0x00FFFFFFU

#define SAMPLE_SIZE 0.7f

enum {
    SCREEN_WIDTH = SKIFF_PSP_UI_SCREEN_WIDTH,

    ITEM_COUNT = 20,
    ITEM_LABEL_MAX = 96,
    TEXT_LEFT = SKIFF_PSP_UI_MARGIN,
    SAMPLE_BASELINE = 36,
    LIST_TOP = 42,
    LIST_ROWS = 14,
    /* Room the list leaves its text: the screen less both margins and the scroll bar. */
    LIST_TEXT_WIDTH = SKIFF_PSP_UI_SCREEN_WIDTH - 3 * SKIFF_PSP_UI_MARGIN,
    PROGRESS_BASELINE = 239,
    PROGRESS_LEFT = 120,
    PROGRESS_TOP = 233,
    PROGRESS_WIDTH = 240,
    PROGRESS_HEIGHT = 6,
    /* The progress bar fills in this many frames, then starts again. */
    PROGRESS_FRAMES = 300,
    PERCENT_FULL = 100,
    STATUS_TEXT_MAX = 96,

    /* First frame: one line per script, far enough apart that their rows read back separately. */
    CHECK_LATIN_BASELINE = 60,
    CHECK_JAPANESE_BASELINE = 140,
    GLYPH_BAND_ASCENT = 24,
    GLYPH_BAND_DESCENT = 8,
    /* A filled rectangle drawn after text, where neither line reaches: every pixel must change. */
    CHECK_RECT_TOP = 200,
    CHECK_RECT_HEIGHT = 10,

    /* A 60 Hz frame. */
    FRAME_BUDGET_US = 16667,
    US_PER_MS = 1000,
    US_PER_S = 1000000,
    BYTES_PER_KB = 1024,

    OSK_TEXT_MAX = 64,
    OSK_LINES = 1,
    /* Thread priorities for the system dialogs, as pspsdk's utility samples use them. */
    DIALOG_GRAPHICS_PRIORITY = 0x11,
    DIALOG_ACCESS_PRIORITY = 0x13,
    DIALOG_FONT_PRIORITY = 0x12,
    DIALOG_SOUND_PRIORITY = 0x10,
    DIALOG_UPDATE_SPEED = 1,
    /* pspUtilityDialogCommon.result: 0 when the player confirmed, 1 when they cancelled. */
    DIALOG_RESULT_CONFIRMED = 0,
};

/*
 * Wall-clock limits, from the system timer. Nobody touching the PSP for 10 s means no tester:
 * tests/emulator/run_eboot.sh gives PPSSPPHeadless 30 s of real time, and PPSSPP runs at least as
 * fast as a PSP, so 10 s of PSP time leaves room for start-up and font loading.
 */
#define HEADLESS_EXIT_US (10LL * US_PER_S)
/* Time to type, or to pick a network and connect, before a dialog is closed for the tester. */
#define DIALOG_TIMEOUT_US (300LL * US_PER_S)
/* Time a dialog gets to close once asked, before the run gives up on it. */
#define DIALOG_CLOSE_GRACE_US (10LL * US_PER_S)
#define DISCONNECT_TIMEOUT_US (10LL * US_PER_S)

/* Buttons a tester presses on purpose; switches (HOLD, Wi-Fi) and HOME never count as input. */
#define TESTER_BUTTONS                                                                             \
    (PSP_CTRL_SELECT | PSP_CTRL_START | PSP_CTRL_UP | PSP_CTRL_RIGHT | PSP_CTRL_DOWN |             \
     PSP_CTRL_LEFT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE | \
     PSP_CTRL_CROSS | PSP_CTRL_SQUARE)

typedef struct memory_snapshot {
    size_t heap_used;
    SceSize system_free;
    SceSize system_largest;
} memory_snapshot;

typedef struct frame_stats {
    long long total_us;
    long long max_us;
    int frames;
    int over_budget;
} frame_stats;

/* How one system dialog went, from InitStart until the system reported it gone. */
typedef struct dialog_outcome {
    int started;
    int reached_visible;
    int closed;
    int timed_out;
    /*
     * First failing sceUtility*Update return, and the last failing *ShutdownStart return (cleared
     * once a retry is accepted); 0 if none failed.
     */
    int update_error;
    int shutdown_error;
    /* pspUtilityDialogCommon.result once closed. */
    int result;
} dialog_outcome;

/* What the run found; summarised on screen and in result.txt once the GU loop is over. */
typedef struct proto_results {
    long long latin_load_us;
    long long japanese_load_us;
    memory_snapshot before_fonts;
    memory_snapshot after_latin_font;
    memory_snapshot after_fonts;
    long latin_glyph_pixels;
    long japanese_glyph_pixels;
    long rect_pixels;
    frame_stats frames;
    int headless;
    int controller_read_errors;
    dialog_outcome osk;
    int osk_field_result;
    int osk_typed_length;
    int net_attempted;
    dialog_outcome netconf;
    char net_ip[SKIFF_PSP_NET_IP_MAX];
    int net_disconnect_ok;
    int net_unload_ok;
    memory_snapshot net_before_load;
    memory_snapshot net_after_load;
    memory_snapshot net_after_unload;
} proto_results;

typedef struct ui_state {
    skiff_psp_report *report;
    intraFont *latin;
    intraFont *japanese;
    skiff_psp_ui gu;
    skiff_ui_list list;
    skiff_ui_input input;
    /* Item labels as drawn: fitted to the list's width once the font is loaded. */
    char items[ITEM_COUNT][ITEM_LABEL_MAX];
    int frame;
    unsigned short osk_text[OSK_TEXT_MAX + 1];
} ui_state;

/* The calls each system dialog offers, so one loop can run either. */
typedef struct dialog_ops {
    const char *name;
    int (*get_status)(void);
    int (*update)(int speed);
    int (*shutdown_start)(void);
} dialog_ops;

static long long now_us(void) { return (long long)sceKernelGetSystemTimeWide(); }

static memory_snapshot take_memory_snapshot(void) {
    const struct mallinfo heap = mallinfo();
    memory_snapshot snapshot = {(size_t)heap.uordblks, sceKernelTotalFreeMemSize(),
                                sceKernelMaxFreeMemSize()};
    return snapshot;
}

/* During the GU loop: logged as it happens, kept off the screen GU is drawing. */
static void log_step(const ui_state *ui, const char *line) {
    skiff_psp_report_line_offscreen(ui->report, line);
}

static void report_memory(skiff_psp_report *report, const char *label,
                          const memory_snapshot *snapshot) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "memory %s: heap used %u KB, system free %u KB (largest %u KB)",
             label, (unsigned)(snapshot->heap_used / BYTES_PER_KB),
             (unsigned)(snapshot->system_free / BYTES_PER_KB),
             (unsigned)(snapshot->system_largest / BYTES_PER_KB));
    skiff_psp_report_line(report, line);
}

static intraFont *load_font(const char *path, unsigned int options, long long *elapsed_us) {
    const long long start_us = now_us();
    intraFont *font = intraFontLoad(path, options);
    *elapsed_us = now_us() - start_us;
    return font;
}

/* Fonts load before GU starts, so these lines can still use the debug screen. */
static int load_fonts(ui_state *ui, proto_results *results) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    results->before_fonts = take_memory_snapshot();
    intraFontInit();
    /* Not INTRAFONT_CACHE_ASCII: that keeps only ASCII glyphs, and accented letters fall back. */
    ui->latin = load_font(LATIN_FONT_PATH, INTRAFONT_CACHE_MED | INTRAFONT_STRING_UTF8,
                          &results->latin_load_us);
    results->after_latin_font = take_memory_snapshot();
    ui->japanese = load_font(JAPANESE_FONT_PATH, INTRAFONT_CACHE_MED | INTRAFONT_STRING_UTF8,
                             &results->japanese_load_us);
    results->after_fonts = take_memory_snapshot();

    snprintf(line, sizeof line, "font %s: %s in %lld ms", LATIN_FONT_PATH,
             ui->latin != NULL ? "loaded" : "FAIL not loaded", results->latin_load_us / US_PER_MS);
    skiff_psp_report_line(ui->report, line);
    snprintf(line, sizeof line, "font %s: %s in %lld ms", JAPANESE_FONT_PATH,
             ui->japanese != NULL ? "loaded" : "FAIL not loaded",
             results->japanese_load_us / US_PER_MS);
    skiff_psp_report_line(ui->report, line);
    report_memory(ui->report, "before fonts", &results->before_fonts);
    report_memory(ui->report, "after Latin font", &results->after_latin_font);
    report_memory(ui->report, "after fonts", &results->after_fonts);
    if (ui->latin == NULL || ui->japanese == NULL) {
        return 0;
    }
    intraFontSetAltFont(ui->latin, ui->japanese);
    return 1;
}

static void unload_fonts(ui_state *ui) {
    if (ui->latin != NULL) {
        intraFontUnload(ui->latin);
    }
    if (ui->japanese != NULL) {
        intraFontUnload(ui->japanese);
    }
    intraFontShutdown();
}

/* The label skiff_psp_ui_list() draws for an item. */
static const char *item_label(void *ctx, size_t index) {
    const ui_state *ui = ctx;
    return ui->items[index];
}

/* Cuts each label to the list's width, once: measuring is too slow to repeat every frame. */
static void fit_items(ui_state *ui) {
    skiff_psp_ui_style style = {&ui->gu, SKIFF_PSP_UI_TEXT_SIZE};
    for (int item = 0; item < ITEM_COUNT; item++) {
        char fitted[ITEM_LABEL_MAX];
        skiff_ui_fit_text(ui->items[item], (float)LIST_TEXT_WIDTH, skiff_psp_ui_measure, &style,
                          fitted, sizeof fitted);
        memcpy(ui->items[item], fitted, sizeof fitted);
    }
}

static void draw_screen(ui_state *ui, const proto_results *results, const char *status) {
    static const skiff_psp_ui_hint hints[] = {
        {SKIFF_UI_ACTION_MENU, "Keyboard"},   {SKIFF_UI_ACTION_EXTRA, "Network"},
        {SKIFF_UI_ACTION_PAGE_UP, "Page up"}, {SKIFF_UI_ACTION_PAGE_DOWN, "Page down"},
        {SKIFF_UI_ACTION_START, "Quit"},
    };
    skiff_psp_ui_header(&ui->gu, TITLE_TEXT, status != NULL ? status : results->net_ip);
    skiff_psp_ui_text(&ui->gu, TEXT_LEFT, SAMPLE_BASELINE, SAMPLE_SIZE, SKIFF_PSP_UI_COLOUR_TEXT,
                      JAPANESE_SAMPLE);
    if (results->osk_typed_length > 0) {
        intraFontPrintUCS2(ui->latin, (float)(SCREEN_WIDTH / 2), (float)SAMPLE_BASELINE,
                           ui->osk_text);
    }
    skiff_psp_ui_list(&ui->gu, &ui->list, LIST_TOP, item_label, ui);
    skiff_psp_ui_text(&ui->gu, TEXT_LEFT, PROGRESS_BASELINE, SKIFF_PSP_UI_HINT_SIZE,
                      SKIFF_PSP_UI_COLOUR_DIM_TEXT, PROGRESS_LABEL);
    skiff_psp_ui_progress_bar(&ui->gu, PROGRESS_LEFT, PROGRESS_TOP, PROGRESS_WIDTH, PROGRESS_HEIGHT,
                              (unsigned)(ui->frame % PROGRESS_FRAMES) * PERCENT_FULL /
                                  PROGRESS_FRAMES);
    skiff_psp_ui_footer(&ui->gu, hints, sizeof hints / sizeof hints[0]);
}

/* Pixels in rows [top, bottom) of the frame just rendered whose colour is not the background's. */
static long count_drawn_pixels(const ui_state *ui, int top, int bottom) {
    const uint32_t *buffer = skiff_psp_ui_drawn_frame(&ui->gu);
    long drawn = 0;
    for (int y = top; y < bottom; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            if ((buffer[y * SKIFF_PSP_UI_BUFFER_WIDTH + x] & COLOUR_RGB_MASK) !=
                (SKIFF_PSP_UI_COLOUR_BACKGROUND & COLOUR_RGB_MASK)) {
                drawn++;
            }
        }
    }
    return drawn;
}

/*
 * First frame: a Latin line and a Japanese-only line, read back after rendering. Zero drawn pixels
 * in a line's rows means its font (for the Japanese line, the Latin font's fallback to the Japanese
 * font) drew nothing.
 */
static void check_glyphs(ui_state *ui, proto_results *results) {
    skiff_psp_ui_begin_frame(&ui->gu);
    skiff_psp_ui_text(&ui->gu, TEXT_LEFT, CHECK_LATIN_BASELINE, SAMPLE_SIZE,
                      SKIFF_PSP_UI_COLOUR_TEXT, LATIN_SAMPLE);
    skiff_psp_ui_text(&ui->gu, TEXT_LEFT, CHECK_JAPANESE_BASELINE, SAMPLE_SIZE,
                      SKIFF_PSP_UI_COLOUR_TEXT, JAPANESE_CHECK_TEXT);
    skiff_psp_ui_rect(&ui->gu, 0, CHECK_RECT_TOP, SCREEN_WIDTH, CHECK_RECT_HEIGHT,
                      SKIFF_PSP_UI_COLOUR_SELECTION);
    skiff_psp_ui_end_frame(&ui->gu);
    results->rect_pixels =
        count_drawn_pixels(ui, CHECK_RECT_TOP, CHECK_RECT_TOP + CHECK_RECT_HEIGHT);
    results->latin_glyph_pixels = count_drawn_pixels(ui, CHECK_LATIN_BASELINE - GLYPH_BAND_ASCENT,
                                                     CHECK_LATIN_BASELINE + GLYPH_BAND_DESCENT);
    results->japanese_glyph_pixels =
        count_drawn_pixels(ui, CHECK_JAPANESE_BASELINE - GLYPH_BAND_ASCENT,
                           CHECK_JAPANESE_BASELINE + GLYPH_BAND_DESCENT);
    skiff_psp_ui_present(&ui->gu);
}

static void fill_dialog_common(pspUtilityDialogCommon *base, unsigned int size) {
    memset(base, 0, sizeof *base);
    base->size = size;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &base->language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP, &base->buttonSwap);
    base->graphicsThread = DIALOG_GRAPHICS_PRIORITY;
    base->accessThread = DIALOG_ACCESS_PRIORITY;
    base->fontThread = DIALOG_FONT_PRIORITY;
    base->soundThread = DIALOG_SOUND_PRIORITY;
}

static void log_dialog_error(const ui_state *ui, const dialog_ops *ops, const char *call,
                             int result) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "FAIL %s: %s 0x%08X", ops->name, call, (unsigned)result);
    log_step(ui, line);
}

/* Asks the dialog to close; returns 1 once the system accepts, so a refusal is retried. */
static int request_close(const ui_state *ui, const dialog_ops *ops, dialog_outcome *outcome) {
    const int result = ops->shutdown_start();
    if (result < 0) {
        if (outcome->shutdown_error != result) {
            log_dialog_error(ui, ops, "ShutdownStart", result);
        }
        outcome->shutdown_error = result;
        return 0;
    }
    outcome->shutdown_error = 0;
    return 1;
}

/*
 * Runs a started dialog over the list until the system reports it gone (NONE). It is closed for
 * the tester after DIALOG_TIMEOUT_US, or when HOME -> Quit is chosen while it is visible; if it is
 * still there DIALOG_CLOSE_GRACE_US later, the run gives up on it (closed stays 0).
 */
static void run_dialog(ui_state *ui, const proto_results *results, const dialog_ops *ops,
                       dialog_outcome *outcome) {
    const long long deadline = now_us() + DIALOG_TIMEOUT_US;
    long long give_up_at = 0; /* set once the dialog has been asked to close */
    int shutdown_accepted = 0;
    for (;;) {
        skiff_psp_ui_begin_frame(&ui->gu);
        draw_screen(ui, results, NULL);
        skiff_psp_ui_backdrop(&ui->gu);
        skiff_psp_ui_end_frame(&ui->gu);
        const int status = ops->get_status();
        const long long now = now_us();
        if (status == PSP_UTILITY_DIALOG_NONE) {
            outcome->closed = 1;
            skiff_psp_ui_present(&ui->gu);
            return;
        }
        if (status == PSP_UTILITY_DIALOG_VISIBLE) {
            outcome->reached_visible = 1;
            if (give_up_at == 0 && (now >= deadline || skiff_psp_exit_requested())) {
                outcome->timed_out = now >= deadline;
                log_step(ui, outcome->timed_out ? "dialog: timed out, closing it"
                                                : "dialog: HOME -> Quit, closing it");
                give_up_at = now + DIALOG_CLOSE_GRACE_US;
                shutdown_accepted = request_close(ui, ops, outcome);
            }
            /* Kept running while closing too: the dialog only reaches QUIT through updates. */
            const int result = ops->update(DIALOG_UPDATE_SPEED);
            if (result < 0 && outcome->update_error == 0) {
                outcome->update_error = result;
                log_dialog_error(ui, ops, "Update", result);
            }
        } else if (status == PSP_UTILITY_DIALOG_QUIT && !shutdown_accepted) {
            if (give_up_at == 0) {
                give_up_at = now + DIALOG_CLOSE_GRACE_US;
            }
            shutdown_accepted = request_close(ui, ops, outcome);
        }
        /* INIT and FINISHED: the system is still bringing it up or tearing it down. */
        const long long limit = give_up_at != 0 ? give_up_at : deadline + DIALOG_CLOSE_GRACE_US;
        if (now >= limit) {
            outcome->timed_out = 1;
            char line[SKIFF_SELFTEST_LINE_MAX];
            snprintf(line, sizeof line, "FAIL %s: still open (status %d), giving up", ops->name,
                     status);
            log_step(ui, line);
            skiff_psp_ui_present(&ui->gu);
            return;
        }
        skiff_psp_ui_present(&ui->gu);
    }
}

static int dialog_ok(const dialog_outcome *outcome) {
    return outcome->started && outcome->reached_visible && outcome->closed && !outcome->timed_out &&
           outcome->update_error == 0 && outcome->shutdown_error == 0 &&
           outcome->result == DIALOG_RESULT_CONFIRMED;
}

static void format_dialog(char *line, size_t size, const char *verdict, const char *name,
                          const dialog_outcome *outcome) {
    snprintf(line, size,
             "%s%s: started %d, shown %d, closed %d, timed out %d, update 0x%08X, shutdown "
             "0x%08X, result %d (0 confirmed)",
             verdict, name, outcome->started, outcome->reached_visible, outcome->closed,
             outcome->timed_out, (unsigned)outcome->update_error, (unsigned)outcome->shutdown_error,
             outcome->result);
}

static void log_dialog(const ui_state *ui, const char *name, const dialog_outcome *outcome) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    format_dialog(line, sizeof line, "", name, outcome);
    log_step(ui, line);
}

static void open_keyboard(ui_state *ui, proto_results *results) {
    static const dialog_ops osk_ops = {"keyboard", sceUtilityOskGetStatus, sceUtilityOskUpdate,
                                       sceUtilityOskShutdownStart};
    /* "Type anything" and an empty start text, as UCS-2. */
    static unsigned short description[] = {'T', 'y', 'p', 'e', ' ', 'a', 'n',
                                           'y', 't', 'h', 'i', 'n', 'g', 0};
    static unsigned short initial_text[] = {0};
    SceUtilityOskData field;
    SceUtilityOskParams params;
    char line[SKIFF_SELFTEST_LINE_MAX];

    memset(&field, 0, sizeof field);
    field.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    field.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
    field.lines = OSK_LINES;
    field.desc = description;
    field.intext = initial_text;
    field.outtextlength = OSK_TEXT_MAX;
    field.outtextlimit = OSK_TEXT_MAX;
    field.outtext = ui->osk_text;
    memset(&params, 0, sizeof params);
    fill_dialog_common(&params.base, sizeof params);
    params.datacount = 1;
    params.data = &field;

    memset(&results->osk, 0, sizeof results->osk);
    memset(ui->osk_text, 0, sizeof ui->osk_text);
    log_step(ui, "keyboard: opening");
    const int started = sceUtilityOskInitStart(&params);
    if (started < 0) {
        log_dialog_error(ui, &osk_ops, "InitStart", started);
        return;
    }
    results->osk.started = 1;
    run_dialog(ui, results, &osk_ops, &results->osk);
    results->osk.result = params.base.result;
    results->osk_field_result = field.result;
    int length = 0;
    while (length < OSK_TEXT_MAX && ui->osk_text[length] != 0) {
        length++;
    }
    results->osk_typed_length = field.result == PSP_UTILITY_OSK_RESULT_CHANGED ? length : 0;
    log_dialog(ui, "keyboard", &results->osk);
    snprintf(line, sizeof line, "keyboard: text %s, %d chars typed",
             field.result == PSP_UTILITY_OSK_RESULT_CHANGED     ? "changed"
             : field.result == PSP_UTILITY_OSK_RESULT_CANCELLED ? "cancelled"
                                                                : "unchanged",
             results->osk_typed_length);
    log_step(ui, line);
}

static int keyboard_ok(const proto_results *results) {
    return dialog_ok(&results->osk) &&
           results->osk_field_result == PSP_UTILITY_OSK_RESULT_CHANGED &&
           results->osk_typed_length > 0;
}

/* How far load_net_modules() got, so teardown undoes exactly that. */
/* A failed net_psp call: the firmware call and its result, as it happens. */
static void log_net_failure(const ui_state *ui, const skiff_psp_net *net, skiff_err err) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "FAIL network: %s (%s returned 0x%08X)", skiff_err_name(err),
             net->failed_call != NULL ? net->failed_call : "-", (unsigned)net->sce_result);
    log_step(ui, line);
}

/*
 * Drops the connection the picker made, if any, and waits until it is gone: tearing the modules
 * down under a live connection is what the app must never do.
 */
static int disconnect(const ui_state *ui, skiff_psp_net *net) {
    const long long start_us = now_us();
    const skiff_err err = skiff_psp_net_disconnect(net, DISCONNECT_TIMEOUT_US);
    if (err != SKIFF_OK) {
        log_net_failure(ui, net, err);
        return 0;
    }
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "network: disconnected after %lld ms",
             (now_us() - start_us) / US_PER_MS);
    log_step(ui, line);
    return 1;
}

static void leave_modules_loaded(const ui_state *ui, proto_results *results, const char *reason) {
    log_step(ui, reason);
    results->net_unload_ok = 0;
    results->net_after_unload = take_memory_snapshot();
}

static void open_network_picker(ui_state *ui, proto_results *results) {
    static const dialog_ops netconf_ops = {"network picker", sceUtilityNetconfGetStatus,
                                           sceUtilityNetconfUpdate, sceUtilityNetconfShutdownStart};
    pspUtilityNetconfData params;

    results->net_attempted = 1;
    memset(&results->netconf, 0, sizeof results->netconf);
    results->net_ip[0] = '\0';
    results->net_disconnect_ok = 0;
    results->net_before_load = take_memory_snapshot();
    skiff_psp_net net;
    const skiff_err loaded = skiff_psp_net_load(&net, SKIFF_PSP_NET_CPU_MHZ_UNCHANGED);
    results->net_after_load = take_memory_snapshot();
    log_step(ui,
             loaded == SKIFF_OK ? "network: modules loaded" : "FAIL network: modules not loaded");
    if (loaded != SKIFF_OK) {
        log_net_failure(ui, &net, loaded);
    } else {
        memset(&params, 0, sizeof params);
        fill_dialog_common(&params.base, sizeof params);
        params.action = PSP_NETCONF_ACTION_CONNECTAP;
        log_step(ui, "network picker: opening");
        const int started = sceUtilityNetconfInitStart(&params);
        if (started < 0) {
            log_dialog_error(ui, &netconf_ops, "InitStart", started);
        } else {
            results->netconf.started = 1;
            run_dialog(ui, results, &netconf_ops, &results->netconf);
            results->netconf.result = params.base.result;
            log_dialog(ui, "network picker", &results->netconf);
            if (params.base.result == DIALOG_RESULT_CONFIRMED) {
                skiff_psp_net_ip(&net, results->net_ip, sizeof results->net_ip);
            }
            log_step(ui, results->net_ip[0] != '\0' ? "network: connected, IP obtained"
                                                    : "network: no IP address");
        }
        /*
         * A picker that never closed, or a connection not proven gone, still uses APCTL and the
         * inet modules; tearing them down under it can hang the EBOOT before it reports. Leave them
         * to the process exit and fail.
         */
        if (results->netconf.started && !results->netconf.closed) {
            leave_modules_loaded(ui, results,
                                 "FAIL network: picker still open, leaving the modules loaded");
            return;
        }
        results->net_disconnect_ok = disconnect(ui, &net);
        if (!results->net_disconnect_ok) {
            leave_modules_loaded(ui, results,
                                 "FAIL network: not disconnected, leaving the modules loaded");
            return;
        }
    }
    const skiff_err unloaded = skiff_psp_net_unload(&net);
    if (unloaded != SKIFF_OK) {
        log_net_failure(ui, &net, unloaded);
    }
    results->net_unload_ok = unloaded == SKIFF_OK;
    results->net_after_unload = take_memory_snapshot();
    log_step(ui, results->net_unload_ok ? "network: modules unloaded"
                                        : "FAIL network: modules not unloaded cleanly");
}

static int network_ok(const proto_results *results) {
    return dialog_ok(&results->netconf) && results->net_ip[0] != '\0' &&
           results->net_disconnect_ok && results->net_unload_ok;
}

/*
 * This frame's actions (skiff/ui.h): tester buttons just pressed, or a held direction repeating. A
 * failed read counts as no input (and is reported), so it can neither move the list nor end
 * headless mode.
 */
static unsigned int read_actions(ui_state *ui, proto_results *results) {
    SceCtrlData pad;
    const int read = sceCtrlReadBufferPositive(&pad, 1);
    if (read <= 0) {
        if (results->controller_read_errors == 0) {
            char line[SKIFF_SELFTEST_LINE_MAX];
            snprintf(line, sizeof line, "FAIL controller: sceCtrlReadBufferPositive 0x%08X",
                     (unsigned)read);
            log_step(ui, line);
        }
        results->controller_read_errors++;
        return 0;
    }
    return skiff_ui_input_update(&ui->input, pad.Buttons & TESTER_BUTTONS);
}

static void record_frame(frame_stats *stats, long long elapsed_us) {
    stats->frames++;
    stats->total_us += elapsed_us;
    if (elapsed_us > stats->max_us) {
        stats->max_us = elapsed_us;
    }
    if (elapsed_us > FRAME_BUDGET_US) {
        stats->over_budget++;
    }
}

/*
 * A dialog the run gave up on may still own input and the screen, so the tester might never reach
 * START or HOME again: end the run there, while result.txt can still get its FAIL marker.
 */
static int dialog_stuck(const ui_state *ui, const dialog_outcome *outcome) {
    if (!outcome->started || outcome->closed) {
        return 0;
    }
    log_step(ui, "FAIL dialog still open: ending the run to report");
    return 1;
}

static void run_list(ui_state *ui, proto_results *results) {
    const long long headless_deadline = now_us() + HEADLESS_EXIT_US;
    int any_input = 0;
    while (!skiff_psp_exit_requested()) {
        const unsigned int actions = read_actions(ui, results);
        if (actions != 0 && !any_input) {
            any_input = 1;
            log_step(ui, "input: a tester is here, headless exit cancelled");
        }
        const long long headless_left_us = headless_deadline - now_us();
        if (!any_input && headless_left_us <= 0) {
            results->headless = 1;
            return;
        }
        if (actions & SKIFF_UI_ACTION_START) {
            return;
        }
        skiff_ui_list_apply(&ui->list, actions);
        if (actions & SKIFF_UI_ACTION_MENU) {
            open_keyboard(ui, results);
            if (dialog_stuck(ui, &results->osk)) {
                return;
            }
            continue;
        }
        if (actions & SKIFF_UI_ACTION_EXTRA) {
            open_network_picker(ui, results);
            if (dialog_stuck(ui, &results->netconf)) {
                return;
            }
            continue;
        }

        const long long start_us = now_us();
        char countdown[STATUS_TEXT_MAX];
        snprintf(countdown, sizeof countdown, "No input: exits in %lld s",
                 headless_left_us / US_PER_S + 1);
        skiff_psp_ui_begin_frame(&ui->gu);
        draw_screen(ui, results, any_input ? NULL : countdown);
        skiff_psp_ui_end_frame(&ui->gu);
        ui->frame++;
        record_frame(&results->frames, now_us() - start_us);
        skiff_psp_ui_present(&ui->gu);
    }
}

/* The highlight and the dialog backdrop are such rectangles. */
static int rect_drawn(const proto_results *results) {
    return results->rect_pixels == (long)SCREEN_WIDTH * CHECK_RECT_HEIGHT;
}

static void report_results(skiff_psp_report *report, const proto_results *results) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "fonts: heap +%u KB for both",
             (unsigned)((results->after_fonts.heap_used - results->before_fonts.heap_used) /
                        BYTES_PER_KB));
    skiff_psp_report_line(report, line);
    snprintf(line, sizeof line, "%s glyphs: Latin line %ld px, Japanese line %ld px",
             results->latin_glyph_pixels > 0 && results->japanese_glyph_pixels > 0 ? "OK" : "FAIL",
             results->latin_glyph_pixels, results->japanese_glyph_pixels);
    skiff_psp_report_line(report, line);
    snprintf(line, sizeof line, "%s rectangle after text: %ld of %d px drawn",
             rect_drawn(results) ? "OK" : "FAIL", results->rect_pixels,
             SCREEN_WIDTH * CHECK_RECT_HEIGHT);
    skiff_psp_report_line(report, line);
    if (results->frames.frames > 0) {
        snprintf(line, sizeof line,
                 "frame (%d-row list, header, footer, progress bar; draw + render): mean %lld us, "
                 "max %lld us, %d of %d over 16.7 ms",
                 LIST_ROWS, results->frames.total_us / results->frames.frames,
                 results->frames.max_us, results->frames.over_budget, results->frames.frames);
    } else {
        snprintf(line, sizeof line, "FAIL frame: no list frame rendered");
    }
    skiff_psp_report_line(report, line);
    if (results->controller_read_errors != 0) {
        snprintf(line, sizeof line, "FAIL controller: %d reads failed",
                 results->controller_read_errors);
        skiff_psp_report_line(report, line);
    }
    if (results->headless) {
        skiff_psp_report_line(report, "no input: headless run, keyboard and network not tried");
        return;
    }
    format_dialog(line, sizeof line, keyboard_ok(results) ? "OK " : "FAIL ", "keyboard",
                  &results->osk);
    skiff_psp_report_line(report, line);
    snprintf(line, sizeof line, "keyboard text: %d chars typed and confirmed (needs at least 1)",
             results->osk_typed_length);
    skiff_psp_report_line(report, line);
    if (!results->net_attempted) {
        skiff_psp_report_line(report, "FAIL network picker: not opened (Square)");
        return;
    }
    format_dialog(line, sizeof line, network_ok(results) ? "OK " : "FAIL ", "network picker",
                  &results->netconf);
    skiff_psp_report_line(report, line);
    snprintf(line, sizeof line, "network: IP %s, disconnect %s, modules unloaded %s",
             results->net_ip[0] != '\0' ? results->net_ip : "none",
             results->net_disconnect_ok ? "ok" : "failed",
             results->net_unload_ok ? "ok" : "failed");
    skiff_psp_report_line(report, line);
    report_memory(report, "before network modules", &results->net_before_load);
    report_memory(report, "after network modules load", &results->net_after_load);
    report_memory(report, "after network modules unload", &results->net_after_unload);
    snprintf(
        line, sizeof line,
        "network modules: took %d KB of system memory, %d KB back after unloading",
        ((int)results->net_before_load.system_free - (int)results->net_after_load.system_free) /
            BYTES_PER_KB,
        ((int)results->net_after_unload.system_free - (int)results->net_after_load.system_free) /
            BYTES_PER_KB);
    skiff_psp_report_line(report, line);
}

static int passed(const proto_results *results) {
    const int rendered = results->latin_glyph_pixels > 0 && results->japanese_glyph_pixels > 0 &&
                         rect_drawn(results) && results->frames.frames > 0 &&
                         results->controller_read_errors == 0;
    if (results->headless) {
        return rendered;
    }
    return rendered && keyboard_ok(results) && network_ok(results);
}

static const char *result_marker(const proto_results *results) {
    if (results->headless) {
        return passed(results) ? PROTO_HEADLESS_OK_MARKER : PROTO_HEADLESS_FAIL_MARKER;
    }
    return passed(results) ? PROTO_OK_MARKER : PROTO_FAIL_MARKER;
}

int main(int argc, char *argv[]) {
    static ui_state ui;
    static proto_results results;
    skiff_psp_report report;

    skiff_psp_install_callbacks();
    skiff_psp_report_open(&report, argc > 0 ? argv[0] : NULL);
    ui.report = &report;
    skiff_psp_report_line(&report, TITLE_TEXT);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    /* The first row shows the accented Latin sample, so a tester can check those glyphs too. */
    snprintf(ui.items[0], ITEM_LABEL_MAX, "%s", LATIN_SAMPLE);
    snprintf(ui.items[1], ITEM_LABEL_MAX, "%s", LONG_TITLE_SAMPLE);
    for (int item = 2; item < ITEM_COUNT; item++) {
        snprintf(ui.items[item], ITEM_LABEL_MAX, "Homebrew sample %02d  (PSP, %d MB)", item + 1,
                 (item + 1) * 16);
    }

    if (!load_fonts(&ui, &results)) {
        unload_fonts(&ui);
        skiff_psp_report_line(&report, PROTO_FAIL_MARKER);
        skiff_psp_report_close(&report);
        sceKernelExitGame();
        return 0;
    }

    log_step(&ui, "GU: starting; the summary follows when the run ends");
    skiff_psp_ui_start(&ui.gu, ui.latin);
    skiff_ui_input_init(&ui.input, ui.gu.confirm_is_cross);
    skiff_ui_list_init(&ui.list, ITEM_COUNT, LIST_ROWS);
    fit_items(&ui);
    check_glyphs(&ui, &results);
    run_list(&ui, &results);
    skiff_psp_ui_stop(&ui.gu);
    unload_fonts(&ui);

    /* GU drew over the debug screen; give the report lines their screen back. */
    pspDebugScreenInit();
    report_results(&report, &results);
    skiff_psp_report_line(&report, result_marker(&results));
    skiff_psp_report_close(&report);
    sceKernelExitGame();
    return 0;
}
