#ifndef SKIFF_PSP_UI_PSP_H
#define SKIFF_PSP_UI_PSP_H

/*
 * Draws the UI's models (skiff/ui.h) with GU and intraFont: a frame with a header and a footer of
 * button hints, text, rectangles, a scrolling list, a progress bar and the dim backdrop laid under
 * a system dialog. One frame: begin_frame(), draw, end_frame(), present().
 *
 * Fonts come from the PSP's firmware (flash0:/font/), so nothing is bundled: Skiff loads only the
 * Latin font (skiff_psp_ui_load_font()); a caller may give it a fallback font with
 * intraFontSetAltFont(), and text styles reach the fallback too.
 */

#include <intraFont.h>
#include <stddef.h>
#include <stdint.h>

#include "skiff/ui.h"

enum {
    SKIFF_PSP_UI_SCREEN_WIDTH = 480,
    SKIFF_PSP_UI_SCREEN_HEIGHT = 272,
    /* Frame buffer rows are this many pixels apart (the screen is 480 of them). */
    SKIFF_PSP_UI_BUFFER_WIDTH = 512,
    /* Where the header and footer end and begin: screens draw between them. */
    SKIFF_PSP_UI_CONTENT_TOP = 24,
    SKIFF_PSP_UI_CONTENT_BOTTOM = 252,
    /* A list row, in pixels, and the left margin text starts at. */
    SKIFF_PSP_UI_ROW_HEIGHT = 13,
    SKIFF_PSP_UI_MARGIN = 12,
};

/* Colours are 0xAABBGGRR. */
#define SKIFF_PSP_UI_COLOUR_BACKGROUND 0xFF302010U
#define SKIFF_PSP_UI_COLOUR_TEXT 0xFFFFFFFFU
#define SKIFF_PSP_UI_COLOUR_DIM_TEXT 0xFFB0B0B0U
#define SKIFF_PSP_UI_COLOUR_SELECTION 0xFF805020U
#define SKIFF_PSP_UI_COLOUR_BAR 0xFF404040U
#define SKIFF_PSP_UI_COLOUR_BAR_FILL 0xFF30B060U
/* Laid over the screen while a system dialog is open, as games do, so the dialog stands out. */
#define SKIFF_PSP_UI_COLOUR_BACKDROP 0xB0000000U

#define SKIFF_PSP_UI_TITLE_SIZE 0.8f
#define SKIFF_PSP_UI_TEXT_SIZE 0.6f
#define SKIFF_PSP_UI_HINT_SIZE 0.5f

typedef struct skiff_psp_ui {
    /* Not owned: load it with skiff_psp_ui_load_font(). */
    intraFont *font;
    /* Which of the two frame buffers GU draws into. */
    int draw_buffer;
    /* The console's setting: Cross confirms (else Circle), for the button hints. */
    int confirm_is_cross;
} skiff_psp_ui;

/* A button hint in the footer: the button an action is on, drawn as its symbol, then label. */
typedef struct skiff_psp_ui_hint {
    unsigned action;
    const char *label;
} skiff_psp_ui_hint;

/* A text style, as the context of skiff_ui_fit_text()'s measure callback. */
typedef struct skiff_psp_ui_style {
    const skiff_psp_ui *ui;
    float size;
} skiff_psp_ui_style;

/* The Latin firmware font (ltn0.pgf), its glyphs cached as they are drawn; NULL when it cannot be
 * loaded. Unload with skiff_psp_ui_unload_font(). */
intraFont *skiff_psp_ui_load_font(void);
void skiff_psp_ui_unload_font(intraFont *font);

/* The system language (PSP_SYSTEMPARAM_LANGUAGE_*) and whether Cross confirms. */
int skiff_psp_ui_system_language(void);
int skiff_psp_ui_confirm_is_cross(void);

/* Starts GU with two frame buffers, drawing with font. */
void skiff_psp_ui_start(skiff_psp_ui *ui, intraFont *font);
void skiff_psp_ui_stop(skiff_psp_ui *ui);

void skiff_psp_ui_begin_frame(skiff_psp_ui *ui);
/* Waits until GU has drawn the frame; skiff_psp_ui_drawn_frame() can read it until present(). */
void skiff_psp_ui_end_frame(skiff_psp_ui *ui);
void skiff_psp_ui_present(skiff_psp_ui *ui);
/* The frame just drawn, SKIFF_PSP_UI_BUFFER_WIDTH pixels per row, read past the CPU's cache. */
const uint32_t *skiff_psp_ui_drawn_frame(const skiff_psp_ui *ui);

/* Text with its baseline at y. */
void skiff_psp_ui_text(const skiff_psp_ui *ui, int x, int y, float size, unsigned int colour,
                       const char *text);
/* skiff_ui_measure_fn: the width text takes in the style (a skiff_psp_ui_style). */
float skiff_psp_ui_measure(void *style, const char *text);

void skiff_psp_ui_rect(const skiff_psp_ui *ui, int x, int y, int width, int height,
                       unsigned int colour);
/* The dim backdrop over the whole screen, under a system dialog. */
void skiff_psp_ui_backdrop(const skiff_psp_ui *ui);

/* The title on the left of the header and, when not NULL, status on its right. */
void skiff_psp_ui_header(const skiff_psp_ui *ui, const char *title, const char *status);
/* Button hints along the footer, left to right. */
void skiff_psp_ui_footer(const skiff_psp_ui *ui, const skiff_psp_ui_hint *hints, size_t count);

/* The label of item index, already fitted to the list's width (skiff_ui_fit_text()). */
typedef const char *(*skiff_psp_ui_label_fn)(void *ctx, size_t index);

/* The rows of list on screen from y (the top of the first row), with the selection highlighted and
 * a scroll bar on the right when the list is longer than the screen. */
void skiff_psp_ui_list(const skiff_psp_ui *ui, const skiff_ui_list *list, int y,
                       skiff_psp_ui_label_fn label, void *label_ctx);

/* A bar from x to x + width, filled to percent (0-100). */
void skiff_psp_ui_progress_bar(const skiff_psp_ui *ui, int x, int y, int width, int height,
                               unsigned percent);

#endif
