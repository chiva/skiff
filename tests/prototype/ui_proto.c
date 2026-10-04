/*
 * UI stack prototype (Phase 1 hardware spike). Confirms the UI design in
 * docs/development/architecture.md before the real ui/ layer is written:
 *
 *   - GU draws a 20-item list with intraFont, using the firmware's fonts (flash0:/font/): a Latin
 *     font with the Japanese font as its fallback, so one UTF-8 string can mix both scripts;
 *   - the d-pad moves the selection; how long a frame takes to draw and render is measured;
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
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <psputility.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "skiff/selftest.h"

#include "lifecycle.h"
#include "report.h"

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
#define HINT_TEXT "Up/Down move   Triangle keyboard   Square network   START quit"

/* Colours are 0xAABBGGRR. */
#define COLOUR_BACKGROUND 0xFF302010U
#define COLOUR_TEXT 0xFFFFFFFFU
#define COLOUR_DIM_TEXT 0xFFB0B0B0U
#define COLOUR_SHADOW 0xFF000000U
#define COLOUR_SELECTION 0xFF805020U
/* A clear without the stencil bit leaves the frame buffer's alpha alone, so compare colour only. */
#define COLOUR_RGB_MASK 0x00FFFFFFU

#define TITLE_SIZE 0.8f
#define SAMPLE_SIZE 0.7f
#define ITEM_SIZE 0.6f
#define HINT_SIZE 0.5f

enum {
    SCREEN_WIDTH = 480,
    SCREEN_HEIGHT = 272,
    BUFFER_WIDTH = 512,
    BYTES_PER_PIXEL = 4,
    FRAME_BUFFER_BYTES = BUFFER_WIDTH * SCREEN_HEIGHT * BYTES_PER_PIXEL,
    /* GU's virtual coordinate space is 4096 wide; the screen sits at its centre. */
    GU_VIRTUAL_CENTRE = 2048,
    DISPLAY_LIST_WORDS = 0x40000,

    ITEM_COUNT = 20,
    ITEM_LABEL_MAX = 48,
    TEXT_LEFT = 12,
    TITLE_BASELINE = 16,
    SAMPLE_BASELINE = 32,
    LIST_FIRST_BASELINE = 48,
    LIST_ROW_HEIGHT = 11,
    SELECTION_ASCENT = 9,
    SELECTION_DESCENT = 2,
    HINT_BASELINE = 269,
    STATUS_LEFT = 260,

    /* First frame: one line per script, far enough apart that their rows read back separately. */
    CHECK_LATIN_BASELINE = 60,
    CHECK_JAPANESE_BASELINE = 140,
    GLYPH_BAND_ASCENT = 24,
    GLYPH_BAND_DESCENT = 8,

    /* Held d-pad: first repeat after 20 frames, then every 4 (at 60 frames per second). */
    REPEAT_DELAY_FRAMES = 20,
    REPEAT_INTERVAL_FRAMES = 4,
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

    /* sceNet pool and threads, as pspsdk's net samples size them. */
    NET_POOL_BYTES = 128 * 1024,
    NET_CALLOUT_PRIORITY = 42,
    NET_CALLOUT_STACK_BYTES = 4 * 1024,
    NET_INTERRUPT_PRIORITY = 42,
    NET_INTERRUPT_STACK_BYTES = 4 * 1024,
    APCTL_STACK_BYTES = 0x8000,
    APCTL_PRIORITY = 48,
    /* SceNetApctlInfo.ip: a dotted IPv4 address. */
    NET_IP_MAX = 16,
    DISCONNECT_POLL_US = 50 * 1000,
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

static unsigned int __attribute__((aligned(16))) display_list[DISPLAY_LIST_WORDS];

typedef struct sprite_vertex {
    unsigned int colour;
    short x;
    short y;
    short z;
} sprite_vertex;

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
    /* First failing sceUtility*Update / *ShutdownStart return, 0 if none failed. */
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
    memory_snapshot after_fonts;
    long latin_glyph_pixels;
    long japanese_glyph_pixels;
    frame_stats frames;
    int headless;
    int controller_read_errors;
    dialog_outcome osk;
    int osk_field_result;
    int osk_typed_length;
    int net_attempted;
    dialog_outcome netconf;
    char net_ip[NET_IP_MAX];
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
    char items[ITEM_COUNT][ITEM_LABEL_MAX];
    int selected;
    unsigned int held_buttons;
    int held_frames;
    unsigned short osk_text[OSK_TEXT_MAX + 1];
    int draw_buffer_index;
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
    ui->latin = load_font(LATIN_FONT_PATH, INTRAFONT_CACHE_ASCII | INTRAFONT_STRING_UTF8,
                          &results->latin_load_us);
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

static void start_gu(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, display_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUFFER_WIDTH);
    sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, (void *)FRAME_BUFFER_BYTES, BUFFER_WIDTH);
    sceGuOffset(GU_VIRTUAL_CENTRE - SCREEN_WIDTH / 2, GU_VIRTUAL_CENTRE - SCREEN_HEIGHT / 2);
    sceGuViewport(GU_VIRTUAL_CENTRE, GU_VIRTUAL_CENTRE, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static void stop_gu(void) {
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
}

static void draw_rect(int x, int y, int width, int height, unsigned int colour) {
    sprite_vertex *vertices = sceGuGetMemory(2 * sizeof(sprite_vertex));
    vertices[0] = (sprite_vertex){colour, (short)x, (short)y, 0};
    vertices[1] = (sprite_vertex){colour, (short)(x + width), (short)(y + height), 0};
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL,
                   vertices);
    sceGuEnable(GU_TEXTURE_2D);
}

static void print_text(intraFont *font, int x, int y, float size, unsigned int colour,
                       const char *text) {
    intraFontSetStyle(font, size, colour, COLOUR_SHADOW, 0.0f, INTRAFONT_ALIGN_LEFT);
    intraFontPrint(font, (float)x, (float)y, text);
}

static void begin_frame(void) {
    sceGuStart(GU_DIRECT, display_list);
    sceGuClearColor(COLOUR_BACKGROUND);
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

static void end_frame(void) {
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}

static void present_frame(ui_state *ui) {
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
    ui->draw_buffer_index ^= 1;
}

static void draw_list(const ui_state *ui, const proto_results *results) {
    print_text(ui->latin, TEXT_LEFT, TITLE_BASELINE, TITLE_SIZE, COLOUR_TEXT, TITLE_TEXT);
    print_text(ui->latin, TEXT_LEFT, SAMPLE_BASELINE, SAMPLE_SIZE, COLOUR_TEXT, JAPANESE_SAMPLE);
    if (results->osk_typed_length > 0) {
        intraFontSetStyle(ui->latin, ITEM_SIZE, COLOUR_DIM_TEXT, COLOUR_SHADOW, 0.0f,
                          INTRAFONT_ALIGN_LEFT);
        intraFontPrintUCS2(ui->latin, (float)STATUS_LEFT, (float)TITLE_BASELINE, ui->osk_text);
    }
    if (results->net_ip[0] != '\0') {
        print_text(ui->latin, STATUS_LEFT, SAMPLE_BASELINE, ITEM_SIZE, COLOUR_DIM_TEXT,
                   results->net_ip);
    }
    for (int item = 0; item < ITEM_COUNT; item++) {
        const int baseline = LIST_FIRST_BASELINE + item * LIST_ROW_HEIGHT;
        if (item == ui->selected) {
            draw_rect(0, baseline - SELECTION_ASCENT, SCREEN_WIDTH,
                      SELECTION_ASCENT + SELECTION_DESCENT, COLOUR_SELECTION);
        }
        print_text(ui->latin, TEXT_LEFT, baseline, ITEM_SIZE, COLOUR_TEXT, ui->items[item]);
    }
    print_text(ui->latin, TEXT_LEFT, HINT_BASELINE, HINT_SIZE, COLOUR_DIM_TEXT, HINT_TEXT);
}

/* Pixels in rows [top, bottom) of the frame just rendered whose colour is not the background's. */
static long count_drawn_pixels(const ui_state *ui, int top, int bottom) {
    const uintptr_t uncached_vram = (uintptr_t)sceGeEdramGetAddr() | 0x40000000U;
    const uint32_t *buffer =
        (const uint32_t *)(uncached_vram + (uintptr_t)ui->draw_buffer_index * FRAME_BUFFER_BYTES);
    long drawn = 0;
    for (int y = top; y < bottom; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            if ((buffer[y * BUFFER_WIDTH + x] & COLOUR_RGB_MASK) !=
                (COLOUR_BACKGROUND & COLOUR_RGB_MASK)) {
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
    begin_frame();
    print_text(ui->latin, TEXT_LEFT, CHECK_LATIN_BASELINE, SAMPLE_SIZE, COLOUR_TEXT, LATIN_SAMPLE);
    print_text(ui->latin, TEXT_LEFT, CHECK_JAPANESE_BASELINE, SAMPLE_SIZE, COLOUR_TEXT,
               JAPANESE_CHECK_TEXT);
    end_frame();
    results->latin_glyph_pixels = count_drawn_pixels(ui, CHECK_LATIN_BASELINE - GLYPH_BAND_ASCENT,
                                                     CHECK_LATIN_BASELINE + GLYPH_BAND_DESCENT);
    results->japanese_glyph_pixels =
        count_drawn_pixels(ui, CHECK_JAPANESE_BASELINE - GLYPH_BAND_ASCENT,
                           CHECK_JAPANESE_BASELINE + GLYPH_BAND_DESCENT);
    present_frame(ui);
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

/* Asks the dialog to close; returns when the run gives up waiting for it. */
static long long request_close(const ui_state *ui, const dialog_ops *ops, dialog_outcome *outcome) {
    const int result = ops->shutdown_start();
    if (result < 0 && outcome->shutdown_error == 0) {
        outcome->shutdown_error = result;
        log_dialog_error(ui, ops, "ShutdownStart", result);
    }
    return now_us() + DIALOG_CLOSE_GRACE_US;
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
    for (;;) {
        begin_frame();
        draw_list(ui, results);
        end_frame();
        const int status = ops->get_status();
        const long long now = now_us();
        if (status == PSP_UTILITY_DIALOG_NONE) {
            outcome->closed = 1;
            present_frame(ui);
            return;
        }
        if (status == PSP_UTILITY_DIALOG_VISIBLE) {
            outcome->reached_visible = 1;
            if (give_up_at == 0 && (now >= deadline || skiff_psp_exit_requested())) {
                outcome->timed_out = now >= deadline;
                log_step(ui, outcome->timed_out ? "dialog: timed out, closing it"
                                                : "dialog: HOME -> Quit, closing it");
                give_up_at = request_close(ui, ops, outcome);
            } else if (give_up_at == 0) {
                const int result = ops->update(DIALOG_UPDATE_SPEED);
                if (result < 0 && outcome->update_error == 0) {
                    outcome->update_error = result;
                    log_dialog_error(ui, ops, "Update", result);
                }
            }
        } else if (status == PSP_UTILITY_DIALOG_QUIT && give_up_at == 0) {
            give_up_at = request_close(ui, ops, outcome);
        }
        /* INIT and FINISHED: the system is still bringing it up or tearing it down. */
        const long long limit = give_up_at != 0 ? give_up_at : deadline + DIALOG_CLOSE_GRACE_US;
        if (now >= limit) {
            outcome->timed_out = 1;
            char line[SKIFF_SELFTEST_LINE_MAX];
            snprintf(line, sizeof line, "FAIL %s: still open (status %d), giving up", ops->name,
                     status);
            log_step(ui, line);
            present_frame(ui);
            return;
        }
        present_frame(ui);
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
typedef enum net_stage {
    NET_STAGE_NONE,
    NET_STAGE_COMMON_MODULE,
    NET_STAGE_INET_MODULE,
    NET_STAGE_NET,
    NET_STAGE_INET,
    NET_STAGE_APCTL,
} net_stage;

static int net_step(const ui_state *ui, const char *name, int result) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "%snetwork: %s 0x%08X", result >= 0 ? "" : "FAIL ", name,
             (unsigned)result);
    log_step(ui, line);
    return result >= 0;
}

static net_stage load_net_modules(const ui_state *ui) {
    if (!net_step(ui, "sceUtilityLoadNetModule(COMMON)",
                  sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON))) {
        return NET_STAGE_NONE;
    }
    if (!net_step(ui, "sceUtilityLoadNetModule(INET)",
                  sceUtilityLoadNetModule(PSP_NET_MODULE_INET))) {
        return NET_STAGE_COMMON_MODULE;
    }
    if (!net_step(ui, "sceNetInit",
                  sceNetInit(NET_POOL_BYTES, NET_CALLOUT_PRIORITY, NET_CALLOUT_STACK_BYTES,
                             NET_INTERRUPT_PRIORITY, NET_INTERRUPT_STACK_BYTES))) {
        return NET_STAGE_INET_MODULE;
    }
    if (!net_step(ui, "sceNetInetInit", sceNetInetInit())) {
        return NET_STAGE_NET;
    }
    if (!net_step(ui, "sceNetApctlInit", sceNetApctlInit(APCTL_STACK_BYTES, APCTL_PRIORITY))) {
        return NET_STAGE_INET;
    }
    return NET_STAGE_APCTL;
}

/* Undoes load_net_modules() in reverse; every step runs even if an earlier one fails. */
static int unload_net_modules(net_stage stage) {
    int ok = 1;
    if (stage >= NET_STAGE_APCTL) {
        ok &= sceNetApctlTerm() >= 0;
    }
    if (stage >= NET_STAGE_INET) {
        ok &= sceNetInetTerm() >= 0;
    }
    if (stage >= NET_STAGE_NET) {
        ok &= sceNetTerm() >= 0;
    }
    if (stage >= NET_STAGE_INET_MODULE) {
        ok &= sceUtilityUnloadNetModule(PSP_NET_MODULE_INET) >= 0;
    }
    if (stage >= NET_STAGE_COMMON_MODULE) {
        ok &= sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON) >= 0;
    }
    return ok;
}

/*
 * Drops the connection the picker made, if any, and waits until APCTL reports it gone: tearing the
 * modules down under a live connection is what the app must never do.
 */
static int disconnect(const ui_state *ui) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    int state = PSP_NET_APCTL_STATE_DISCONNECTED;
    int result = sceNetApctlGetState(&state);
    if (result < 0) {
        snprintf(line, sizeof line, "FAIL network: sceNetApctlGetState 0x%08X", (unsigned)result);
        log_step(ui, line);
        return 0;
    }
    if (state == PSP_NET_APCTL_STATE_DISCONNECTED) {
        log_step(ui, "network: not connected, nothing to disconnect");
        return 1;
    }
    result = sceNetApctlDisconnect();
    if (!net_step(ui, "sceNetApctlDisconnect", result)) {
        return 0;
    }
    const long long start_us = now_us();
    while (now_us() - start_us < DISCONNECT_TIMEOUT_US) {
        result = sceNetApctlGetState(&state);
        if (result >= 0 && state == PSP_NET_APCTL_STATE_DISCONNECTED) {
            snprintf(line, sizeof line, "network: disconnected in %lld ms",
                     (now_us() - start_us) / US_PER_MS);
            log_step(ui, line);
            return 1;
        }
        sceKernelDelayThread(DISCONNECT_POLL_US);
    }
    snprintf(line, sizeof line, "FAIL network: still in APCTL state %d (0x%08X) after %lld s",
             state, (unsigned)result, DISCONNECT_TIMEOUT_US / US_PER_S);
    log_step(ui, line);
    return 0;
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
    const net_stage stage = load_net_modules(ui);
    results->net_after_load = take_memory_snapshot();

    if (stage == NET_STAGE_APCTL) {
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
            SceNetApctlInfo info;
            const int got_ip = params.base.result == DIALOG_RESULT_CONFIRMED
                                   ? sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info)
                                   : -1;
            if (got_ip >= 0) {
                snprintf(results->net_ip, sizeof results->net_ip, "%s", info.ip);
            }
            log_step(ui, results->net_ip[0] != '\0' ? "network: connected, IP obtained"
                                                    : "network: no IP address");
        }
        results->net_disconnect_ok = disconnect(ui);
    }
    results->net_unload_ok = unload_net_modules(stage);
    results->net_after_unload = take_memory_snapshot();
    log_step(ui, results->net_unload_ok ? "network: modules unloaded"
                                        : "FAIL network: modules not unloaded cleanly");
}

static int network_ok(const proto_results *results) {
    return dialog_ok(&results->netconf) && results->net_ip[0] != '\0' &&
           results->net_disconnect_ok && results->net_unload_ok;
}

/*
 * Tester buttons pressed this frame, or a d-pad direction held long enough to repeat. A failed read
 * counts as no input (and is reported), so it can neither move the list nor end headless mode.
 */
static unsigned int read_buttons(ui_state *ui, proto_results *results) {
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
    const unsigned int buttons = pad.Buttons & TESTER_BUTTONS;
    const unsigned int pressed = buttons & ~ui->held_buttons;
    unsigned int repeated = 0;
    if (buttons != 0 && buttons == ui->held_buttons) {
        ui->held_frames++;
        if (ui->held_frames >= REPEAT_DELAY_FRAMES &&
            (ui->held_frames - REPEAT_DELAY_FRAMES) % REPEAT_INTERVAL_FRAMES == 0) {
            repeated = buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN);
        }
    } else {
        ui->held_frames = 0;
    }
    ui->held_buttons = buttons;
    return pressed | repeated;
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

static void run_list(ui_state *ui, proto_results *results) {
    const long long headless_deadline = now_us() + HEADLESS_EXIT_US;
    int any_input = 0;
    while (!skiff_psp_exit_requested()) {
        const unsigned int buttons = read_buttons(ui, results);
        if (buttons != 0 && !any_input) {
            any_input = 1;
            log_step(ui, "input: a tester is here, headless exit cancelled");
        }
        const long long headless_left_us = headless_deadline - now_us();
        if (!any_input && headless_left_us <= 0) {
            results->headless = 1;
            return;
        }
        if (buttons & PSP_CTRL_START) {
            return;
        }
        if (buttons & PSP_CTRL_UP) {
            ui->selected = (ui->selected + ITEM_COUNT - 1) % ITEM_COUNT;
        }
        if (buttons & PSP_CTRL_DOWN) {
            ui->selected = (ui->selected + 1) % ITEM_COUNT;
        }
        if (buttons & PSP_CTRL_TRIANGLE) {
            open_keyboard(ui, results);
            continue;
        }
        if (buttons & PSP_CTRL_SQUARE) {
            open_network_picker(ui, results);
            continue;
        }

        const long long start_us = now_us();
        begin_frame();
        draw_list(ui, results);
        if (!any_input) {
            char countdown[SKIFF_SELFTEST_LINE_MAX];
            snprintf(countdown, sizeof countdown, "No input: exits in %lld s",
                     headless_left_us / US_PER_S + 1);
            print_text(ui->latin, STATUS_LEFT, TITLE_BASELINE, ITEM_SIZE, COLOUR_DIM_TEXT,
                       countdown);
        }
        end_frame();
        record_frame(&results->frames, now_us() - start_us);
        present_frame(ui);
    }
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
    if (results->frames.frames > 0) {
        snprintf(line, sizeof line,
                 "frame (%d-item list, draw + render): mean %lld us, max %lld us, %d of %d over "
                 "16.7 ms",
                 ITEM_COUNT, results->frames.total_us / results->frames.frames,
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
                         results->frames.frames > 0 && results->controller_read_errors == 0;
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

    skiff_psp_install_exit_callback();
    skiff_psp_report_open(&report, argc > 0 ? argv[0] : NULL);
    ui.report = &report;
    skiff_psp_report_line(&report, TITLE_TEXT);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    /* The first row shows the accented Latin sample, so a tester can check those glyphs too. */
    snprintf(ui.items[0], ITEM_LABEL_MAX, "%s", LATIN_SAMPLE);
    for (int item = 1; item < ITEM_COUNT; item++) {
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
    start_gu();
    check_glyphs(&ui, &results);
    run_list(&ui, &results);
    stop_gu();
    unload_fonts(&ui);

    /* GU drew over the debug screen; give the report lines their screen back. */
    pspDebugScreenInit();
    report_results(&report, &results);
    skiff_psp_report_line(&report, result_marker(&results));
    skiff_psp_report_close(&report);
    sceKernelExitGame();
    return 0;
}
