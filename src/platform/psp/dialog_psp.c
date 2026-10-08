#include "dialog_psp.h"

#include <pspthreadman.h>
#include <string.h>

#include "skiff/ui.h"

enum {
    /* Thread priorities for the system dialogs, as pspsdk's utility samples use them. */
    DIALOG_GRAPHICS_PRIORITY = 0x11,
    DIALOG_ACCESS_PRIORITY = 0x13,
    DIALOG_FONT_PRIORITY = 0x12,
    DIALOG_SOUND_PRIORITY = 0x10,
    DIALOG_UPDATE_SPEED = 1,
    /* pspUtilityDialogCommon.result once closed; below 0 is an error. */
    DIALOG_RESULT_CONFIRMED = 0,
    DIALOG_RESULT_CANCELLED = 1,
    KEYBOARD_LINES = 1,
};

/* What each dialog calls, so one update can run either. */
typedef struct dialog_calls {
    const char *init_start_name;
    const char *closed_with_error;
    const char *still_open;
    int (*get_status)(void);
    int (*update)(int speed);
    int (*shutdown_start)(void);
} dialog_calls;

static const dialog_calls KEYBOARD_CALLS = {
    "sceUtilityOskInitStart",
    "sceUtilityOskGetStatus (closed with an error)",
    "sceUtilityOskGetStatus (still open)",
    sceUtilityOskGetStatus,
    sceUtilityOskUpdate,
    sceUtilityOskShutdownStart,
};

static const dialog_calls NETWORK_CALLS = {
    "sceUtilityNetconfInitStart",
    "sceUtilityNetconfGetStatus (closed with an error)",
    "sceUtilityNetconfGetStatus (still open)",
    sceUtilityNetconfGetStatus,
    sceUtilityNetconfUpdate,
    sceUtilityNetconfShutdownStart,
};

static const dialog_calls *calls_of(const skiff_psp_dialog *dialog) {
    return dialog->kind == SKIFF_PSP_DIALOG_KEYBOARD ? &KEYBOARD_CALLS : &NETWORK_CALLS;
}

static long long now_us(void) { return (long long)sceKernelGetSystemTimeWide(); }

/* A dialog may start unless one is on screen or was given up on (it may still be). */
static int may_start(const skiff_psp_dialog *dialog) {
    return dialog != NULL && dialog->state != SKIFF_PSP_DIALOG_RUNNING &&
           dialog->state != SKIFF_PSP_DIALOG_STUCK;
}

static void reset(skiff_psp_dialog *dialog, skiff_psp_dialog_kind kind) {
    memset(dialog, 0, sizeof *dialog);
    dialog->kind = kind;
}

/* The language and confirm button follow the console's settings, as the XMB's own dialogs do. */
static void fill_common(pspUtilityDialogCommon *base, unsigned int size) {
    memset(base, 0, sizeof *base);
    base->size = size;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &base->language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP, &base->buttonSwap);
    base->graphicsThread = DIALOG_GRAPHICS_PRIORITY;
    base->accessThread = DIALOG_ACCESS_PRIORITY;
    base->fontThread = DIALOG_FONT_PRIORITY;
    base->soundThread = DIALOG_SOUND_PRIORITY;
}

/* What *InitStart returned: RUNNING, or FAILED with the refusal for the first update to return. */
static skiff_err started(skiff_psp_dialog *dialog, int result) {
    if (result < 0) {
        dialog->state = SKIFF_PSP_DIALOG_FAILED;
        dialog->failed_call = calls_of(dialog)->init_start_name;
        dialog->sce_result = result;
    } else {
        dialog->state = SKIFF_PSP_DIALOG_RUNNING;
    }
    return SKIFF_OK;
}

skiff_err skiff_psp_dialog_start_keyboard(skiff_psp_dialog *dialog, const char *description,
                                          const char *initial, size_t max_units) {
    if (!may_start(dialog) || description == NULL || initial == NULL || max_units == 0 ||
        max_units > SKIFF_PSP_DIALOG_TEXT_MAX) {
        return SKIFF_ERR_INVALID_ARG;
    }
    reset(dialog, SKIFF_PSP_DIALOG_KEYBOARD);
    size_t initial_units = 0;
    if (skiff_ui_utf8_to_utf16(description, dialog->description,
                               sizeof dialog->description / sizeof dialog->description[0],
                               NULL) != SKIFF_OK ||
        skiff_ui_utf8_to_utf16(initial, dialog->initial_text,
                               sizeof dialog->initial_text / sizeof dialog->initial_text[0],
                               &initial_units) != SKIFF_OK ||
        initial_units > max_units) {
        reset(dialog, SKIFF_PSP_DIALOG_KEYBOARD);
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    SceUtilityOskData *field = &dialog->field;
    field->language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    field->inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
    field->lines = KEYBOARD_LINES;
    field->desc = dialog->description;
    field->intext = dialog->initial_text;
    /* As the prototype passed them on hardware: both the limit, with room for the end besides. */
    field->outtextlength = (int)max_units;
    field->outtextlimit = (int)max_units;
    field->outtext = dialog->text;
    SceUtilityOskParams *params = &dialog->params.keyboard;
    fill_common(&params->base, sizeof *params);
    params->datacount = 1;
    params->data = field;
    return started(dialog, sceUtilityOskInitStart(params));
}

skiff_err skiff_psp_dialog_start_network(skiff_psp_dialog *dialog) {
    if (!may_start(dialog)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    reset(dialog, SKIFF_PSP_DIALOG_NETWORK);
    pspUtilityNetconfData *params = &dialog->params.network;
    fill_common(&params->base, sizeof *params);
    params->action = PSP_NETCONF_ACTION_CONNECTAP;
    return started(dialog, sceUtilityNetconfInitStart(params));
}

void skiff_psp_dialog_close(skiff_psp_dialog *dialog) {
    if (dialog == NULL || dialog->state != SKIFF_PSP_DIALOG_RUNNING || dialog->close_requested) {
        return;
    }
    dialog->close_requested = 1;
    dialog->give_up_at_us = now_us() + SKIFF_PSP_DIALOG_CLOSE_GRACE_US;
}

/* Asks the system to close the dialog; a refusal is kept and asked again on the next update. */
static void request_shutdown(skiff_psp_dialog *dialog, const dialog_calls *calls) {
    const int result = calls->shutdown_start();
    if (result < 0) {
        dialog->shutdown_error = result;
        return;
    }
    dialog->shutdown_error = 0;
    dialog->shutdown_accepted = 1;
}

/* The system reports the dialog gone: its outcome from what it wrote back. */
static void finish(skiff_psp_dialog *dialog, const dialog_calls *calls) {
    const int is_keyboard = dialog->kind == SKIFF_PSP_DIALOG_KEYBOARD;
    const int result =
        is_keyboard ? dialog->params.keyboard.base.result : dialog->params.network.base.result;
    dialog->dialog_result = result;
    dialog->keyboard_result = is_keyboard ? dialog->field.result : 0;
    if (result == DIALOG_RESULT_CONFIRMED &&
        !(is_keyboard && dialog->field.result == PSP_UTILITY_OSK_RESULT_CANCELLED)) {
        dialog->state = SKIFF_PSP_DIALOG_ACCEPTED;
    } else if (result == DIALOG_RESULT_CONFIRMED || result == DIALOG_RESULT_CANCELLED) {
        dialog->state = SKIFF_PSP_DIALOG_CANCELLED;
    } else {
        dialog->state = SKIFF_PSP_DIALOG_FAILED;
        dialog->failed_call = calls->closed_with_error;
        dialog->sce_result = result;
    }
}

skiff_psp_dialog_state skiff_psp_dialog_update(skiff_psp_dialog *dialog) {
    if (dialog == NULL) {
        return SKIFF_PSP_DIALOG_FAILED;
    }
    if (dialog->state != SKIFF_PSP_DIALOG_RUNNING) {
        return dialog->state;
    }
    const dialog_calls *calls = calls_of(dialog);
    const int status = calls->get_status();
    dialog->status = status;
    if (status == PSP_UTILITY_DIALOG_NONE) {
        finish(dialog, calls);
        return dialog->state;
    }
    if (status == PSP_UTILITY_DIALOG_QUIT && !dialog->close_requested) {
        /* The player is done: the dialog waits to be closed, and gets the same grace. */
        skiff_psp_dialog_close(dialog);
    }
    const int may_close = status == PSP_UTILITY_DIALOG_VISIBLE || status == PSP_UTILITY_DIALOG_QUIT;
    if (may_close && dialog->close_requested && !dialog->shutdown_accepted) {
        request_shutdown(dialog, calls);
    }
    if (status == PSP_UTILITY_DIALOG_VISIBLE) {
        dialog->shown = 1;
        /* Kept running while closing too: the dialog only reaches QUIT through updates. */
        const int result = calls->update(DIALOG_UPDATE_SPEED);
        if (result < 0 && dialog->update_error == 0) {
            dialog->update_error = result;
        }
    }
    /* INIT and FINISHED: the system is still bringing it up or taking it down. */
    if (dialog->close_requested && now_us() >= dialog->give_up_at_us) {
        dialog->state = SKIFF_PSP_DIALOG_STUCK;
        dialog->failed_call = calls->still_open;
        dialog->sce_result = status;
    }
    return dialog->state;
}

skiff_err skiff_psp_dialog_text(const skiff_psp_dialog *dialog, char *out, size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (dialog == NULL || out == NULL || out_size == 0 ||
        dialog->kind != SKIFF_PSP_DIALOG_KEYBOARD || dialog->state != SKIFF_PSP_DIALOG_ACCEPTED) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const uint16_t *text = dialog->keyboard_result == PSP_UTILITY_OSK_RESULT_CHANGED
                               ? dialog->text
                               : dialog->initial_text;
    return skiff_ui_utf16_to_utf8(text, SKIFF_PSP_DIALOG_TEXT_MAX + 1, out, out_size);
}
