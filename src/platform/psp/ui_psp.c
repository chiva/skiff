#include "ui_psp.h"

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psputility.h>

#define LATIN_FONT_PATH "flash0:/font/ltn0.pgf"
/* intraFont's shadow colour; with a shadow size of 0 none is drawn. */
#define COLOUR_SHADOW 0xFF000000U
/* The lines under the header and over the footer. */
#define COLOUR_RULE 0xFF604030U
/* Button symbols in the colours PlayStation players know them by. */
#define COLOUR_CIRCLE 0xFF5050F0U
#define COLOUR_CROSS 0xFFFFA080U
#define COLOUR_TRIANGLE 0xFFA0D040U
#define COLOUR_SQUARE 0xFFD090F0U
/* Uncached view of VRAM: reading the frame back must not hit stale cache lines. */
#define VRAM_UNCACHED_BIT 0x40000000U

/* skiff/ui.h's bits are an anonymous enum, pspctrl.h's a named one: compare them as numbers. */
#define SAME_BUTTON(ours, sdk) ((unsigned)(ours) == (unsigned)(sdk))
_Static_assert(SAME_BUTTON(SKIFF_UI_BUTTON_SELECT, PSP_CTRL_SELECT) &&
                   SAME_BUTTON(SKIFF_UI_BUTTON_START, PSP_CTRL_START) &&
                   SAME_BUTTON(SKIFF_UI_BUTTON_UP, PSP_CTRL_UP) &&
                   SAME_BUTTON(SKIFF_UI_BUTTON_RIGHT, PSP_CTRL_RIGHT) &&
                   SAME_BUTTON(SKIFF_UI_BUTTON_DOWN, PSP_CTRL_DOWN) &&
                   SAME_BUTTON(SKIFF_UI_BUTTON_LEFT, PSP_CTRL_LEFT) &&
                   SAME_BUTTON(SKIFF_UI_BUTTON_L, PSP_CTRL_LTRIGGER) &&
                   SAME_BUTTON(SKIFF_UI_BUTTON_R, PSP_CTRL_RTRIGGER) &&
                   SAME_BUTTON(SKIFF_UI_BUTTON_TRIANGLE, PSP_CTRL_TRIANGLE) &&
                   SAME_BUTTON(SKIFF_UI_BUTTON_CIRCLE, PSP_CTRL_CIRCLE) &&
                   SAME_BUTTON(SKIFF_UI_BUTTON_CROSS, PSP_CTRL_CROSS) &&
                   SAME_BUTTON(SKIFF_UI_BUTTON_SQUARE, PSP_CTRL_SQUARE),
               "skiff/ui.h's button bits must be pspctrl.h's");

enum {
    BYTES_PER_PIXEL = 4,
    FRAME_BUFFER_BYTES = SKIFF_PSP_UI_BUFFER_WIDTH * SKIFF_PSP_UI_SCREEN_HEIGHT * BYTES_PER_PIXEL,
    /* GU's virtual coordinate space is 4096 wide; the screen sits at its centre. */
    GU_VIRTUAL_CENTRE = 2048,
    DISPLAY_LIST_WORDS = 0x40000,

    TITLE_BASELINE = 16,
    RULE_Y = 21,
    FOOTER_RULE_Y = 254,
    FOOTER_BASELINE = 267,
    /* A button symbol: its size, where it sits against the baseline, and the gap after it. */
    SYMBOL_SIZE = 9,
    SYMBOL_RISE = 9,
    SYMBOL_GAP = 4,
    HINT_GAP = 16,
    /* Where a row's text sits in it, and the scroll bar on the right. */
    ROW_BASELINE = 10,
    SCROLL_BAR_WIDTH = 3,
    SCROLL_BAR_RIGHT = 4,
    SCROLL_THUMB_MIN = 6,
    LIST_TEXT_RIGHT = 12,

    PERCENT_FULL = 100,
};

static unsigned int __attribute__((aligned(16))) display_list[DISPLAY_LIST_WORDS];

typedef struct ui_vertex {
    unsigned int colour;
    short x;
    short y;
    short z;
} ui_vertex;

/* The Circle symbol, pixel by pixel ('#'): a midpoint circle of radius 4. Drawn as an outline, an
 * octagon that small rounds to a diamond. */
static const char CIRCLE_PIXELS[SYMBOL_SIZE][SYMBOL_SIZE + 1] = {
    "   ###   ", " ##   ## ", " #     # ", "#       #", "#       #",
    "#       #", " #     # ", " ##   ## ", "   ###   ",
};

intraFont *skiff_psp_ui_load_font(void) {
    intraFontInit();
    /* Not INTRAFONT_CACHE_ASCII: that keeps only ASCII glyphs, and accented letters fall back. */
    return intraFontLoad(LATIN_FONT_PATH, INTRAFONT_CACHE_MED | INTRAFONT_STRING_UTF8);
}

void skiff_psp_ui_unload_font(intraFont *font) {
    if (font != NULL) {
        intraFontUnload(font);
    }
    intraFontShutdown();
}

int skiff_psp_ui_system_language(void) {
    int language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
    if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &language) < 0) {
        return PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
    }
    return language;
}

int skiff_psp_ui_confirm_is_cross(void) {
    int swap = PSP_UTILITY_ACCEPT_CROSS;
    if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP, &swap) < 0) {
        return 1;
    }
    return swap == PSP_UTILITY_ACCEPT_CROSS;
}

void skiff_psp_ui_start(skiff_psp_ui *ui, intraFont *font) {
    ui->font = font;
    ui->draw_buffer = 0;
    ui->confirm_is_cross = skiff_psp_ui_confirm_is_cross();
    sceGuInit();
    sceGuStart(GU_DIRECT, display_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, SKIFF_PSP_UI_BUFFER_WIDTH);
    sceGuDispBuffer(SKIFF_PSP_UI_SCREEN_WIDTH, SKIFF_PSP_UI_SCREEN_HEIGHT,
                    (void *)FRAME_BUFFER_BYTES, SKIFF_PSP_UI_BUFFER_WIDTH);
    sceGuOffset(GU_VIRTUAL_CENTRE - SKIFF_PSP_UI_SCREEN_WIDTH / 2,
                GU_VIRTUAL_CENTRE - SKIFF_PSP_UI_SCREEN_HEIGHT / 2);
    sceGuViewport(GU_VIRTUAL_CENTRE, GU_VIRTUAL_CENTRE, SKIFF_PSP_UI_SCREEN_WIDTH,
                  SKIFF_PSP_UI_SCREEN_HEIGHT);
    sceGuScissor(0, 0, SKIFF_PSP_UI_SCREEN_WIDTH, SKIFF_PSP_UI_SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

void skiff_psp_ui_stop(skiff_psp_ui *ui) {
    (void)ui;
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
}

void skiff_psp_ui_begin_frame(skiff_psp_ui *ui) {
    (void)ui;
    sceGuStart(GU_DIRECT, display_list);
    sceGuClearColor(SKIFF_PSP_UI_COLOUR_BACKGROUND);
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

void skiff_psp_ui_end_frame(skiff_psp_ui *ui) {
    (void)ui;
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}

void skiff_psp_ui_present(skiff_psp_ui *ui) {
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
    ui->draw_buffer ^= 1;
}

const uint32_t *skiff_psp_ui_drawn_frame(const skiff_psp_ui *ui) {
    const uintptr_t vram = (uintptr_t)sceGeEdramGetAddr() | VRAM_UNCACHED_BIT;
    return (const uint32_t *)(vram + (uintptr_t)ui->draw_buffer * FRAME_BUFFER_BYTES);
}

/* ---- Geometry ---- */

/*
 * Draws vertices written to GU memory. The GE reads RAM, not the CPU's data cache, so they are
 * written back first; and intraFont turns the depth test back on after every print with no depth
 * buffer, which on hardware discards geometry drawn after text (PPSSPP lets it through).
 */
static void draw_vertices(int primitive, ui_vertex *vertices, int count) {
    sceKernelDcacheWritebackRange(vertices, (unsigned int)count * sizeof(ui_vertex));
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(primitive, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, count, NULL,
                   vertices);
    sceGuEnable(GU_TEXTURE_2D);
}

static ui_vertex vertex(unsigned int colour, int x, int y) {
    const ui_vertex v = {colour, (short)x, (short)y, 0};
    return v;
}

void skiff_psp_ui_rect(const skiff_psp_ui *ui, int x, int y, int width, int height,
                       unsigned int colour) {
    (void)ui;
    ui_vertex *vertices = sceGuGetMemory((int)(2 * sizeof(ui_vertex)));
    vertices[0] = vertex(colour, x, y);
    vertices[1] = vertex(colour, x + width, y + height);
    draw_vertices(GU_SPRITES, vertices, 2);
}

void skiff_psp_ui_backdrop(const skiff_psp_ui *ui) {
    skiff_psp_ui_rect(ui, 0, 0, SKIFF_PSP_UI_SCREEN_WIDTH, SKIFF_PSP_UI_SCREEN_HEIGHT,
                      SKIFF_PSP_UI_COLOUR_BACKDROP);
}

/* An outline through points (x, y) pairs, closed back to the first. */
static void draw_outline(const int (*points)[2], int count, int left, int top,
                         unsigned int colour) {
    ui_vertex *vertices = sceGuGetMemory((int)((size_t)(count + 1) * sizeof(ui_vertex)));
    for (int i = 0; i <= count; i++) {
        vertices[i] = vertex(colour, left + points[i % count][0], top + points[i % count][1]);
    }
    draw_vertices(GU_LINE_STRIP, vertices, count + 1);
}

/* The '#' pixels of a SYMBOL_SIZE square at (left, top), one sprite each. */
static void draw_pixels(const char (*pixels)[SYMBOL_SIZE + 1], int left, int top,
                        unsigned int colour) {
    int count = 0;
    for (int y = 0; y < SYMBOL_SIZE; y++) {
        for (int x = 0; x < SYMBOL_SIZE; x++) {
            count += pixels[y][x] == '#';
        }
    }
    ui_vertex *vertices = sceGuGetMemory((int)((size_t)count * 2 * sizeof(ui_vertex)));
    int at = 0;
    for (int y = 0; y < SYMBOL_SIZE; y++) {
        for (int x = 0; x < SYMBOL_SIZE; x++) {
            if (pixels[y][x] == '#') {
                vertices[at++] = vertex(colour, left + x, top + y);
                vertices[at++] = vertex(colour, left + x + 1, top + y + 1);
            }
        }
    }
    draw_vertices(GU_SPRITES, vertices, at);
}

/* The symbol of a face button in a SYMBOL_SIZE square at (left, top). */
static void draw_symbol(unsigned button, int left, int top) {
    const int size = SYMBOL_SIZE - 1;
    if (button == SKIFF_UI_BUTTON_CROSS) {
        ui_vertex *vertices = sceGuGetMemory((int)(4 * sizeof(ui_vertex)));
        vertices[0] = vertex(COLOUR_CROSS, left, top);
        vertices[1] = vertex(COLOUR_CROSS, left + size, top + size);
        vertices[2] = vertex(COLOUR_CROSS, left + size, top);
        vertices[3] = vertex(COLOUR_CROSS, left, top + size);
        draw_vertices(GU_LINES, vertices, 4);
    } else if (button == SKIFF_UI_BUTTON_CIRCLE) {
        draw_pixels(CIRCLE_PIXELS, left, top, COLOUR_CIRCLE);
    } else if (button == SKIFF_UI_BUTTON_TRIANGLE) {
        const int points[3][2] = {{size / 2, 0}, {size, size}, {0, size}};
        draw_outline(points, 3, left, top, COLOUR_TRIANGLE);
    } else {
        const int points[4][2] = {{0, 0}, {size, 0}, {size, size}, {0, size}};
        draw_outline(points, 4, left, top, COLOUR_SQUARE);
    }
}

/* ---- Text ---- */

/* Sets the style on the font and its fallbacks: a fallback draws characters with its own style. */
static void set_style(intraFont *font, float size, unsigned int colour, unsigned int options) {
    for (intraFont *styled = font; styled != NULL;
         styled = styled->altFont != font ? styled->altFont : NULL) {
        intraFontSetStyle(styled, size, colour, COLOUR_SHADOW, 0.0f, options);
    }
}

static float print_aligned(const skiff_psp_ui *ui, int x, int y, float size, unsigned int colour,
                           unsigned int align, const char *text) {
    if (ui->font == NULL || text == NULL) {
        return (float)x;
    }
    set_style(ui->font, size, colour, align);
    return intraFontPrint(ui->font, (float)x, (float)y, text);
}

void skiff_psp_ui_text(const skiff_psp_ui *ui, int x, int y, float size, unsigned int colour,
                       const char *text) {
    (void)print_aligned(ui, x, y, size, colour, INTRAFONT_ALIGN_LEFT, text);
}

float skiff_psp_ui_measure(void *style, const char *text) {
    const skiff_psp_ui_style *text_style = style;
    if (text_style == NULL || text_style->ui == NULL || text_style->ui->font == NULL ||
        text == NULL) {
        return 0.0f;
    }
    set_style(text_style->ui->font, text_style->size, SKIFF_PSP_UI_COLOUR_TEXT,
              INTRAFONT_ALIGN_LEFT);
    return intraFontMeasureText(text_style->ui->font, text);
}

/* ---- Header, footer, list, progress ---- */

void skiff_psp_ui_header(const skiff_psp_ui *ui, const char *title, const char *status) {
    (void)print_aligned(ui, SKIFF_PSP_UI_MARGIN, TITLE_BASELINE, SKIFF_PSP_UI_TITLE_SIZE,
                        SKIFF_PSP_UI_COLOUR_TEXT, INTRAFONT_ALIGN_LEFT, title);
    if (status != NULL) {
        (void)print_aligned(ui, SKIFF_PSP_UI_SCREEN_WIDTH - SKIFF_PSP_UI_MARGIN, TITLE_BASELINE,
                            SKIFF_PSP_UI_HINT_SIZE, SKIFF_PSP_UI_COLOUR_DIM_TEXT,
                            INTRAFONT_ALIGN_RIGHT, status);
    }
    skiff_psp_ui_rect(ui, 0, RULE_Y, SKIFF_PSP_UI_SCREEN_WIDTH, 1, COLOUR_RULE);
}

/* The face button an action is on, or 0 for one shown by name. */
static unsigned symbol_button(const skiff_psp_ui *ui, unsigned action) {
    switch (action) {
    case SKIFF_UI_ACTION_CONFIRM:
        return ui->confirm_is_cross ? SKIFF_UI_BUTTON_CROSS : SKIFF_UI_BUTTON_CIRCLE;
    case SKIFF_UI_ACTION_BACK:
        return ui->confirm_is_cross ? SKIFF_UI_BUTTON_CIRCLE : SKIFF_UI_BUTTON_CROSS;
    case SKIFF_UI_ACTION_MENU:
        return SKIFF_UI_BUTTON_TRIANGLE;
    case SKIFF_UI_ACTION_EXTRA:
        return SKIFF_UI_BUTTON_SQUARE;
    default:
        return 0;
    }
}

/* The name a button without a symbol goes by in the hints. */
static const char *button_name(unsigned action) {
    switch (action) {
    case SKIFF_UI_ACTION_PAGE_UP:
        return "L";
    case SKIFF_UI_ACTION_PAGE_DOWN:
        return "R";
    case SKIFF_UI_ACTION_START:
        return "START";
    case SKIFF_UI_ACTION_SELECT:
        return "SELECT";
    default:
        return "?";
    }
}

void skiff_psp_ui_footer(const skiff_psp_ui *ui, const skiff_psp_ui_hint *hints, size_t count) {
    skiff_psp_ui_rect(ui, 0, FOOTER_RULE_Y, SKIFF_PSP_UI_SCREEN_WIDTH, 1, COLOUR_RULE);
    float x = (float)SKIFF_PSP_UI_MARGIN;
    for (size_t i = 0; hints != NULL && i < count; i++) {
        const unsigned button = symbol_button(ui, hints[i].action);
        if (button != 0) {
            draw_symbol(button, (int)x, FOOTER_BASELINE - SYMBOL_RISE);
            x += (float)(SYMBOL_SIZE + SYMBOL_GAP);
        } else {
            x = print_aligned(ui, (int)x, FOOTER_BASELINE, SKIFF_PSP_UI_HINT_SIZE,
                              SKIFF_PSP_UI_COLOUR_DIM_TEXT, INTRAFONT_ALIGN_LEFT,
                              button_name(hints[i].action)) +
                (float)SYMBOL_GAP;
        }
        x = print_aligned(ui, (int)x, FOOTER_BASELINE, SKIFF_PSP_UI_HINT_SIZE,
                          SKIFF_PSP_UI_COLOUR_TEXT, INTRAFONT_ALIGN_LEFT, hints[i].label) +
            (float)HINT_GAP;
    }
}

void skiff_psp_ui_rows(const skiff_psp_ui *ui, const skiff_ui_list *list, int y,
                       skiff_psp_ui_row_fn row, void *row_ctx) {
    if (list == NULL || row == NULL) {
        return;
    }
    const int track_height = (int)list->rows * SKIFF_PSP_UI_ROW_HEIGHT;
    for (size_t shown = 0; shown < list->rows && list->first + shown < list->count; shown++) {
        const size_t index = list->first + shown;
        const int top = y + (int)shown * SKIFF_PSP_UI_ROW_HEIGHT;
        if (index == list->selected) {
            skiff_psp_ui_rect(ui, 0, top, SKIFF_PSP_UI_SCREEN_WIDTH - LIST_TEXT_RIGHT,
                              SKIFF_PSP_UI_ROW_HEIGHT, SKIFF_PSP_UI_COLOUR_SELECTION);
        }
        skiff_psp_ui_row item = {NULL, NULL, 0};
        row(row_ctx, index, &item);
        const unsigned int colour =
            item.dim ? SKIFF_PSP_UI_COLOUR_DIM_TEXT : SKIFF_PSP_UI_COLOUR_TEXT;
        skiff_psp_ui_text(ui, SKIFF_PSP_UI_MARGIN, top + ROW_BASELINE, SKIFF_PSP_UI_TEXT_SIZE,
                          colour, item.label);
        if (item.detail != NULL && item.detail[0] != '\0') {
            (void)print_aligned(ui, SKIFF_PSP_UI_SCREEN_WIDTH - LIST_TEXT_RIGHT - SCROLL_BAR_RIGHT,
                                top + ROW_BASELINE, SKIFF_PSP_UI_TEXT_SIZE,
                                SKIFF_PSP_UI_COLOUR_DIM_TEXT, INTRAFONT_ALIGN_RIGHT, item.detail);
        }
    }
    if (list->count <= list->rows) {
        return;
    }
    const int track_x = SKIFF_PSP_UI_SCREEN_WIDTH - SCROLL_BAR_RIGHT - SCROLL_BAR_WIDTH;
    int thumb = (int)((size_t)track_height * list->rows / list->count);
    if (thumb < SCROLL_THUMB_MIN) {
        thumb = SCROLL_THUMB_MIN;
    }
    const int thumb_top =
        y + (int)((size_t)(track_height - thumb) * list->first / (list->count - list->rows));
    skiff_psp_ui_rect(ui, track_x, y, SCROLL_BAR_WIDTH, track_height, SKIFF_PSP_UI_COLOUR_BAR);
    skiff_psp_ui_rect(ui, track_x, thumb_top, SCROLL_BAR_WIDTH, thumb,
                      SKIFF_PSP_UI_COLOUR_DIM_TEXT);
}

/* A plain list is rows with only a label. */
typedef struct label_rows {
    skiff_psp_ui_label_fn label;
    void *ctx;
} label_rows;

static void label_row(void *ctx, size_t index, skiff_psp_ui_row *row) {
    const label_rows *rows = ctx;
    row->label = rows->label(rows->ctx, index);
}

void skiff_psp_ui_list(const skiff_psp_ui *ui, const skiff_ui_list *list, int y,
                       skiff_psp_ui_label_fn label, void *label_ctx) {
    if (label == NULL) {
        return;
    }
    label_rows rows = {label, label_ctx};
    skiff_psp_ui_rows(ui, list, y, label_row, &rows);
}

void skiff_psp_ui_qr(const skiff_psp_ui *ui, int x, int y, int scale, const skiff_ui_qr *qr) {
    if (qr == NULL || qr->size <= 0 || scale <= 0) {
        return;
    }
    const int side = (qr->size + 2 * SKIFF_UI_QR_QUIET_ZONE) * scale;
    skiff_psp_ui_rect(ui, x, y, side, side, SKIFF_PSP_UI_COLOUR_QR_LIGHT);
    const int left = x + SKIFF_UI_QR_QUIET_ZONE * scale;
    const int top = y + SKIFF_UI_QR_QUIET_ZONE * scale;
    /* One sprite per run of dark modules in a row: a few hundred for a pairing address. */
    for (int row = 0; row < qr->size; row++) {
        int column = 0;
        while (column < qr->size) {
            if (!skiff_ui_qr_dark(qr, column, row)) {
                column++;
                continue;
            }
            const int start = column;
            while (column < qr->size && skiff_ui_qr_dark(qr, column, row)) {
                column++;
            }
            skiff_psp_ui_rect(ui, left + start * scale, top + row * scale, (column - start) * scale,
                              scale, SKIFF_PSP_UI_COLOUR_QR_DARK);
        }
    }
}

void skiff_psp_ui_progress_bar(const skiff_psp_ui *ui, int x, int y, int width, int height,
                               unsigned percent) {
    if (percent > PERCENT_FULL) {
        percent = PERCENT_FULL;
    }
    skiff_psp_ui_rect(ui, x, y, width, height, SKIFF_PSP_UI_COLOUR_BAR);
    const int filled = (int)((unsigned)width * percent / PERCENT_FULL);
    if (filled > 0) {
        skiff_psp_ui_rect(ui, x, y, filled, height, SKIFF_PSP_UI_COLOUR_BAR_FILL);
    }
}
