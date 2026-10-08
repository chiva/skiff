#ifndef SKIFF_PSP_DIALOG_PSP_H
#define SKIFF_PSP_DIALOG_PSP_H

/*
 * The PSP's system dialogs Skiff uses, one step per frame from the UI's GU loop: the on-screen
 * keyboard (sceUtilityOsk) and the network picker (sceUtilityNetconf). The system runs one dialog
 * at a time, and draws it over the frame Skiff has just rendered:
 *
 *     start (skiff_psp_dialog_start_keyboard() or skiff_psp_dialog_start_network()), then per
 *     frame: skiff_psp_ui_begin_frame(), draw the screen under it, skiff_psp_ui_backdrop(),
 *     skiff_psp_ui_end_frame(), skiff_psp_dialog_update(), skiff_psp_ui_present(); until update
 *     returns something other than SKIFF_PSP_DIALOG_RUNNING.
 *
 * What the hardware taught (tests/prototype/ui_proto.c):
 *   - the system may refuse to close a dialog (ShutdownStart fails): it is asked again on every
 *     update until it accepts;
 *   - a dialog asked to close that is still there SKIFF_PSP_DIALOG_CLOSE_GRACE_US later is given up
 *     on (SKIFF_PSP_DIALOG_STUCK). It may still own the screen and the buttons, so the player might
 *     never reach a way out: end the app there, after writing what the log needs;
 *   - the network picker uses the network modules while it runs: never unload them
 *     (skiff_psp_net_unload()) while it is RUNNING or STUCK.
 *
 * Text crosses as UTF-8; the conversion to and from the keyboard's UTF-16 is skiff/ui.h's.
 * Keep a dialog in static storage (or zero it) before its first start: the system reads and writes
 * its parameters until it reports the dialog gone.
 */

#include <psputility.h>
#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"

/* The most UTF-16 units the keyboard takes (a URL of SKIFF_CONFIG_URL_MAX bytes, less its end). */
#define SKIFF_PSP_DIALOG_TEXT_MAX 255U
/* The most UTF-16 units of the line the keyboard shows above the text. */
#define SKIFF_PSP_DIALOG_DESCRIPTION_MAX 63U
/* The most UTF-8 bytes skiff_psp_dialog_text() writes, with the end. */
#define SKIFF_PSP_DIALOG_TEXT_UTF8_MAX (SKIFF_PSP_DIALOG_TEXT_MAX * 3U + 1U)
/* How long a dialog asked to close gets to go, by the system timer, before it is given up on. */
#define SKIFF_PSP_DIALOG_CLOSE_GRACE_US (10LL * 1000 * 1000)

typedef enum skiff_psp_dialog_kind {
    SKIFF_PSP_DIALOG_KEYBOARD,
    SKIFF_PSP_DIALOG_NETWORK,
} skiff_psp_dialog_kind;

typedef enum skiff_psp_dialog_state {
    /* Never started. */
    SKIFF_PSP_DIALOG_IDLE,
    /* On screen, or being brought up or taken down: call skiff_psp_dialog_update() every frame. */
    SKIFF_PSP_DIALOG_RUNNING,
    /* Keyboard: the player confirmed (skiff_psp_dialog_text() has the text). Network picker: it
     * connected to an access point. */
    SKIFF_PSP_DIALOG_ACCEPTED,
    /* The player backed out; or the dialog was asked to close (skiff_psp_dialog_close()). */
    SKIFF_PSP_DIALOG_CANCELLED,
    /* The system refused to open it, or it closed with an error: failed_call and sce_result. */
    SKIFF_PSP_DIALOG_FAILED,
    /* Asked to close and still there after the grace: see above. */
    SKIFF_PSP_DIALOG_STUCK,
} skiff_psp_dialog_state;

typedef struct skiff_psp_dialog {
    skiff_psp_dialog_kind kind;
    skiff_psp_dialog_state state;
    /* The last status the system reported (PSP_UTILITY_DIALOG_*), and whether it was ever VISIBLE.
     */
    int status;
    int shown;
    /* Set by skiff_psp_dialog_close() or by the dialog's own QUIT: a close is in progress, whether
     * the system has accepted it, and when the dialog is given up on (system time). */
    int close_requested;
    int shutdown_accepted;
    long long give_up_at_us;
    /* The first failing *Update return, and the last failing *ShutdownStart return (cleared once a
     * retry is accepted); 0 while none has failed. Kept apart from the outcome: a dialog can fail
     * an update and still close normally. */
    int update_error;
    int shutdown_error;
    /* Why it FAILED or got STUCK: the system call and what it returned (for a dialog that closed
     * with an error, pspUtilityDialogCommon.result). NULL and 0 otherwise. */
    const char *failed_call;
    int sce_result;
    /* pspUtilityDialogCommon.result once closed (0 confirmed, 1 cancelled), and the keyboard's
     * field result (PSP_UTILITY_OSK_RESULT_*). */
    int dialog_result;
    int keyboard_result;
    /* Owned by the system while RUNNING. */
    union {
        SceUtilityOskParams keyboard;
        pspUtilityNetconfData network;
    } params;
    SceUtilityOskData field;
    uint16_t description[SKIFF_PSP_DIALOG_DESCRIPTION_MAX + 1];
    uint16_t initial_text[SKIFF_PSP_DIALOG_TEXT_MAX + 1];
    uint16_t text[SKIFF_PSP_DIALOG_TEXT_MAX + 1];
} skiff_psp_dialog;

/*
 * Opens the keyboard with description (UTF-8) on the line above the text, initial (UTF-8) as the
 * text to edit, and room for max_units UTF-16 units (1 to SKIFF_PSP_DIALOG_TEXT_MAX; a character
 * above U+FFFF takes two). Invalid UTF-8 shows as U+FFFD (skiff/ui.h). SKIFF_OK once the dialog is
 * RUNNING, or FAILED when the system refused it (the first update returns that). Otherwise nothing
 * was opened: SKIFF_ERR_INVALID_ARG for a NULL argument, a max_units out of range, or a dialog
 * RUNNING or STUCK; SKIFF_ERR_BUFFER_TOO_SMALL when description or initial does not fit.
 */
skiff_err skiff_psp_dialog_start_keyboard(skiff_psp_dialog *dialog, const char *description,
                                          const char *initial, size_t max_units);

/*
 * Opens the network picker to connect to an access point through the player's Network Settings
 * (PSP_NETCONF_ACTION_CONNECTAP). The network modules must be loaded (skiff_psp_net_load()) and
 * stay loaded until it is over. Once ACCEPTED the connection stands: skiff_psp_net_online() and
 * skiff_psp_net_connected_profile() see it. Returns as skiff_psp_dialog_start_keyboard() does.
 */
skiff_err skiff_psp_dialog_start_network(skiff_psp_dialog *dialog);

/*
 * One frame of a RUNNING dialog: lets the system draw it and take input (sceUtility*Update), closes
 * it when the player is done, and returns its state. Call once per frame, after
 * skiff_psp_ui_end_frame() and before skiff_psp_ui_present(). For a dialog that is not RUNNING (a
 * NULL one is FAILED), it returns the state without calling the system.
 */
skiff_psp_dialog_state skiff_psp_dialog_update(skiff_psp_dialog *dialog);

/*
 * Asks a RUNNING dialog to close as if the player backed out (HOME -> Quit, a timeout of the
 * caller's), starting its SKIFF_PSP_DIALOG_CLOSE_GRACE_US. Keep calling skiff_psp_dialog_update()
 * until it ends: CANCELLED, or what the player had already chosen, or STUCK. Does nothing
 * otherwise, or when asked again.
 */
void skiff_psp_dialog_close(skiff_psp_dialog *dialog);

/*
 * The keyboard's text once ACCEPTED, as UTF-8 into out (SKIFF_PSP_DIALOG_TEXT_UTF8_MAX always
 * fits): what the player typed, or the initial text when they confirmed it unchanged.
 * SKIFF_ERR_INVALID_ARG for a NULL argument or a dialog that is not an ACCEPTED keyboard;
 * SKIFF_ERR_BUFFER_TOO_SMALL when the text does not fit (out empty).
 */
skiff_err skiff_psp_dialog_text(const skiff_psp_dialog *dialog, char *out, size_t out_size);

#endif
