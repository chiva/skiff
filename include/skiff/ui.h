#ifndef SKIFF_UI_H
#define SKIFF_UI_H

/*
 * The parts of the UI that are not drawing, so they run and are tested on the host: buttons to
 * actions (with repeat for a held d-pad), a scrolling list, text cut to a width, UTF-8 to and from
 * the UTF-16 the system dialogs use, download progress with its rate and time left, and QR codes.
 * src/platform/psp/ui_psp.h draws them with GU and intraFont; src/platform/psp/dialog_psp.h runs
 * the dialogs.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"

/* ---- Buttons and actions ---- */

/* Button bits, with the values of pspctrl.h's PSP_CTRL_* (ui_psp.c checks they agree), so platform
 * code passes SceCtrlData.Buttons through unchanged. */
enum {
    SKIFF_UI_BUTTON_SELECT = 0x000001,
    SKIFF_UI_BUTTON_START = 0x000008,
    SKIFF_UI_BUTTON_UP = 0x000010,
    SKIFF_UI_BUTTON_RIGHT = 0x000020,
    SKIFF_UI_BUTTON_DOWN = 0x000040,
    SKIFF_UI_BUTTON_LEFT = 0x000080,
    SKIFF_UI_BUTTON_L = 0x000100,
    SKIFF_UI_BUTTON_R = 0x000200,
    SKIFF_UI_BUTTON_TRIANGLE = 0x001000,
    SKIFF_UI_BUTTON_CIRCLE = 0x002000,
    SKIFF_UI_BUTTON_CROSS = 0x004000,
    SKIFF_UI_BUTTON_SQUARE = 0x008000,
};

/* What a button means to a screen. Confirm and back follow the PSP's own setting: Circle confirms
 * on Japanese consoles and Cross elsewhere (PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP). */
enum {
    SKIFF_UI_ACTION_UP = 1U << 0,
    SKIFF_UI_ACTION_DOWN = 1U << 1,
    SKIFF_UI_ACTION_LEFT = 1U << 2,
    SKIFF_UI_ACTION_RIGHT = 1U << 3,
    SKIFF_UI_ACTION_PAGE_UP = 1U << 4,
    SKIFF_UI_ACTION_PAGE_DOWN = 1U << 5,
    SKIFF_UI_ACTION_CONFIRM = 1U << 6,
    SKIFF_UI_ACTION_BACK = 1U << 7,
    /* Triangle and Square: what a screen offers besides confirm and back. */
    SKIFF_UI_ACTION_MENU = 1U << 8,
    SKIFF_UI_ACTION_EXTRA = 1U << 9,
    SKIFF_UI_ACTION_START = 1U << 10,
    SKIFF_UI_ACTION_SELECT = 1U << 11,
};

/* A held d-pad direction or shoulder button repeats after this many frames, then every
 * SKIFF_UI_REPEAT_INTERVAL_FRAMES (at 60 frames per second: a third of a second, then 15 a second).
 */
#define SKIFF_UI_REPEAT_DELAY_FRAMES 20
#define SKIFF_UI_REPEAT_INTERVAL_FRAMES 4

typedef struct skiff_ui_input {
    unsigned held;
    unsigned held_moves;
    int held_frames;
    int confirm_is_cross;
} skiff_ui_input;

void skiff_ui_input_init(skiff_ui_input *input, int confirm_is_cross);

/* Actions for this frame from the buttons down now: a button counts when pressed, and a held
 * direction or shoulder button again when it repeats. Call once per frame. 0 for NULL. */
unsigned skiff_ui_input_update(skiff_ui_input *input, unsigned buttons);

/* ---- A scrolling list ---- */

typedef struct skiff_ui_list {
    size_t count;
    size_t selected;
    /* The first item on screen, and how many rows the screen shows. */
    size_t first;
    size_t rows;
} skiff_ui_list;

/* An empty list selects 0. rows below 1 counts as 1. */
void skiff_ui_list_init(skiff_ui_list *list, size_t count, size_t rows);

/* The list now has count items (a page arrived, one was removed): the selection stays where it
 * was, or moves to the last item, and stays on screen. */
void skiff_ui_list_set_count(skiff_ui_list *list, size_t count);

/* Selects index (clamped to the list) and scrolls it onto the screen. */
void skiff_ui_list_select(skiff_ui_list *list, size_t index);

/* Up and down move by one and wrap around the ends; page up and down move by a screen and stop at
 * the ends. Returns 1 when the selection moved. */
int skiff_ui_list_apply(skiff_ui_list *list, unsigned actions);

/* ---- Text cut to a width ---- */

/* Drawn after a cut: ASCII, since the PSP's Latin firmware font has no "…". */
#define SKIFF_UI_ELLIPSIS "..."

/* The width text takes when drawn, in the caller's units (pixels on the PSP). */
typedef float (*skiff_ui_measure_fn)(void *ctx, const char *text);

/*
 * Copies text into out, or as much of it as fits in max_width followed by SKIFF_UI_ELLIPSIS, cut
 * between UTF-8 characters with blanks before the ellipsis dropped. out_size limits it as well:
 * a text longer than out is cut the same way. When even the ellipsis does not fit, out is empty.
 * Measures a few times (a binary search), so fit text once when it changes, not every frame.
 * SKIFF_ERR_INVALID_ARG for a NULL argument or an out_size too small for the ellipsis.
 */
skiff_err skiff_ui_fit_text(const char *text, float max_width, skiff_ui_measure_fn measure,
                            void *measure_ctx, char *out, size_t out_size);

/* ---- UTF-16, the system dialogs' text ---- */

/* The character put in place of a sequence that is not valid UTF-8 or UTF-16. */
#define SKIFF_UI_REPLACEMENT_CHARACTER 0xFFFDU

/* The most UTF-8 bytes one UTF-16 unit becomes (a pair of two becomes 4): out_size for
 * skiff_ui_utf16_to_utf8() of n units is at most n * SKIFF_UI_UTF8_BYTES_PER_UTF16_UNIT + 1. */
#define SKIFF_UI_UTF8_BYTES_PER_UTF16_UNIT 3U

/*
 * UTF-8 text as UTF-16 units into out, ended by a 0 unit; characters above U+FFFF become surrogate
 * pairs. A byte sequence that is not UTF-8 (a stray continuation byte, a cut or overlong sequence,
 * a surrogate or a code point above U+10FFFF) becomes one SKIFF_UI_REPLACEMENT_CHARACTER per
 * maximal invalid part, so text from a hand-edited file still reaches the keyboard. *length (when
 * not NULL) gets the units written, without the end. SKIFF_ERR_INVALID_ARG for a NULL text or out,
 * or no out_units; SKIFF_ERR_BUFFER_TOO_SMALL when it does not fit (out empty, *length 0).
 */
skiff_err skiff_ui_utf8_to_utf16(const char *text, uint16_t *out, size_t out_units, size_t *length);

/*
 * UTF-16 text, up to its first 0 unit or max_units, as UTF-8 into out. A surrogate without its
 * other half becomes SKIFF_UI_REPLACEMENT_CHARACTER. Errors as for skiff_ui_utf8_to_utf16() (out
 * empty when it does not fit); a NULL text is invalid even with max_units 0.
 */
skiff_err skiff_ui_utf16_to_utf8(const uint16_t *text, size_t max_units, char *out,
                                 size_t out_size);

/* ---- Progress ---- */

/* The rate is measured over windows this long and smoothed across them. */
#define SKIFF_UI_RATE_WINDOW_MS 2000U

typedef struct skiff_ui_progress {
    uint64_t done;
    uint64_t total;
    /* Bytes per second, smoothed; 0 until a window has passed. */
    uint64_t rate;
    int has_rate;
    uint64_t window_done;
    uint64_t window_start_ms;
} skiff_ui_progress;

/* A download of total bytes that has done bytes at now_ms (a resumed one starts above 0). */
void skiff_ui_progress_start(skiff_ui_progress *progress, uint64_t done, uint64_t total,
                             uint64_t now_ms);

/* done bytes at now_ms. Going backwards (the download restarted) starts the rate again. */
void skiff_ui_progress_update(skiff_ui_progress *progress, uint64_t done, uint64_t now_ms);

/* 0-100, rounded down: 100 only once every byte is there; 0 for an unknown total. */
unsigned skiff_ui_progress_percent(const skiff_ui_progress *progress);

/* Seconds left at the current rate, into *seconds; 0 (and *seconds untouched) without a rate or
 * a known total. */
int skiff_ui_progress_eta(const skiff_ui_progress *progress, uint64_t *seconds);

/* "999 B", "512 KB", "1.5 MB", "640 MB", "1.2 GB" (1 KB = 1024 bytes, as the PSP's XMB counts),
 * with decimal_separator between the units and the tenth ("," in Spanish). SKIFF_ERR_INVALID_ARG
 * for NULL, SKIFF_ERR_BUFFER_TOO_SMALL when it does not fit (out empty). */
skiff_err skiff_ui_format_bytes(uint64_t bytes, const char *decimal_separator, char *out,
                                size_t out_size);

/* "45 s", "12 min" (rounded up), "2 h 05 min": symbols both English and Spanish use. Errors as for
 * skiff_ui_format_bytes(). */
skiff_err skiff_ui_format_duration(uint64_t seconds, char *out, size_t out_size);

/* ---- QR codes ---- */

/* The largest QR code Skiff makes: version 10, 57 modules a side, holding up to 213 bytes at error
 * correction level M. A pairing address is about 60 bytes (version 4, 33 modules). */
#define SKIFF_UI_QR_VERSION_MAX 10
#define SKIFF_UI_QR_SIZE_MAX (SKIFF_UI_QR_VERSION_MAX * 4 + 17)
/* The light border a reader needs around the code, in modules (the standard's quiet zone). */
#define SKIFF_UI_QR_QUIET_ZONE 4

typedef struct skiff_ui_qr {
    /* Modules a side; 0 for no code. */
    int size;
    /* Row by row, one bit per module, set for a dark one. */
    uint8_t modules[(SKIFF_UI_QR_SIZE_MAX * SKIFF_UI_QR_SIZE_MAX + 7) / 8];
} skiff_ui_qr;

/*
 * Encodes text (UTF-8, as bytes) in the smallest QR code that holds it at error correction level M,
 * with a higher level when that fits the same size (Nayuki's QR Code generator).
 * SKIFF_ERR_INVALID_ARG for NULL, SKIFF_ERR_BUFFER_TOO_SMALL when it needs more than
 * SKIFF_UI_QR_VERSION_MAX; out->size is 0 on error.
 */
skiff_err skiff_ui_qr_encode(const char *text, skiff_ui_qr *out);

/* 1 for a dark module at (x, y), 0 for a light one, outside the code (its quiet zone) or for NULL.
 */
int skiff_ui_qr_dark(const skiff_ui_qr *qr, int x, int y);

/* The most whole pixels a module can take for qr and its quiet zone to fit in max_pixels a side;
 * 0 when not even one does, or for no code. */
int skiff_ui_qr_scale(const skiff_ui_qr *qr, int max_pixels);

#endif
