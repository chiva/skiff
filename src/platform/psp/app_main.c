#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/app.h"
#include "skiff/i18n.h"
#include "skiff/version.h"

#include "app_psp.h"
#include "lifecycle.h"
#include "ui_psp.h"

#ifdef SKIFF_APP_SMOKE
#include "report.h"

/*
 * The emulator smoke test (skiff_app_smoke, never packaged): PPSSPPHeadless has no ARK, no
 * config.ini and nobody at the buttons, so the app must come up on the screen asking for the server
 * address and draw it. It quits by itself then, as it would on HOME → Quit.
 */
#define SMOKE_OK_MARKER "SKIFF APP SMOKE OK"
#define SMOKE_FAIL_MARKER "SKIFF APP SMOKE FAIL"
/* Frames drawn on the first screen before the run counts as up, and frames allowed to get there. */
#define SMOKE_FRAMES_SHOWN 60
#define SMOKE_FRAMES_MAX 1800
/* The browsing thread's check: how often to ask whether a call is done or cancelled, and for how
 * long at most (2 s). */
#define SMOKE_POLL_US (10LL * 1000)
#define SMOKE_POLLS_MAX 200

typedef struct smoke {
    skiff_psp_report report;
    int frames;
    int shown;
    int done;
    int ok;
} smoke;

static const char *const SCREEN_NAMES[] = {"starting", "server",  "connecting", "pair",
                                           "library",  "details", "queue",      "settings",
                                           "message",  "confirm"};

static void smoke_frame(smoke *s, const skiff_app_view *view) {
    s->frames++;
    if (view->screen == SKIFF_APP_SCREEN_SERVER) {
        s->shown++;
    }
    char line[SKIFF_APP_TITLE_MAX + 2 * SKIFF_TEXT_MAX];
    if (s->shown >= SMOKE_FRAMES_SHOWN) {
        snprintf(line, sizeof line, "first screen: %s (%s) after %d frames",
                 SCREEN_NAMES[view->screen], view->title, s->frames);
        skiff_psp_report_line_offscreen(&s->report, line);
        s->ok = 1;
        s->done = 1;
    } else if (view->screen == SKIFF_APP_SCREEN_MESSAGE || s->frames >= SMOKE_FRAMES_MAX) {
        snprintf(line, sizeof line, "stopped on %s (%s): %s", SCREEN_NAMES[view->screen],
                 view->title, view->line_count > 0 ? view->lines[0] : "");
        skiff_psp_report_line_offscreen(&s->report, line);
        s->done = 1;
    }
}

/* A call that counts that it ran. */
static void smoke_count(void *arg) { (*(int *)arg)++; }

/* A call that runs until it is cancelled, as a transfer's stop hook would see. */
typedef struct smoke_wait {
    const skiff_psp_caller *caller;
    int saw_cancel;
} smoke_wait;

static void smoke_wait_for_cancel(void *arg) {
    smoke_wait *wait = arg;
    for (int i = 0; i < SMOKE_POLLS_MAX && !skiff_psp_caller_cancelled(wait->caller); i++) {
        sceKernelDelayThread((SceUInt)SMOKE_POLL_US);
    }
    wait->saw_cancel = skiff_psp_caller_cancelled(wait->caller);
}

static int smoke_wait_done(const skiff_app_env *env) {
    for (int i = 0; i < SMOKE_POLLS_MAX; i++) {
        if (env->call_done(env->ctx)) {
            return 1;
        }
        sceKernelDelayThread((SceUInt)SMOKE_POLL_US);
    }
    return 0;
}

/* The browsing thread through the hooks the app uses: a call runs, and a cancelled one stops. */
static int smoke_calls(smoke *s, skiff_psp_app *platform) {
    const skiff_app_env env = skiff_psp_app_env(platform);
    int ran = 0;
    skiff_err err = env.call_start(env.ctx, smoke_count, &ran);
    const int counted = err == SKIFF_OK && smoke_wait_done(&env) && ran == 1;
    smoke_wait wait = {&platform->caller, 0};
    if (err == SKIFF_OK) {
        err = env.call_start(env.ctx, smoke_wait_for_cancel, &wait);
    }
    if (err == SKIFF_OK) {
        env.call_cancel(env.ctx);
    }
    const int cancelled = err == SKIFF_OK && smoke_wait_done(&env) && wait.saw_cancel;
    char line[SKIFF_TEXT_MAX];
    snprintf(line, sizeof line, "browse thread: call %s, cancel %s: %s (%d)",
             counted ? "ran" : "FAILED", cancelled ? "stopped it" : "FAILED", skiff_err_name(err),
             (int)err);
    skiff_psp_report_line_offscreen(&s->report, line);
    return counted && cancelled;
}

/* A cover as the details screen draws it: a red square from a 5650 texture, read back from the
 * frame. PPSSPP's software renderer draws what the GE would. */
#define SMOKE_COVER_SIDE 16
#define SMOKE_COVER_X 100
#define SMOKE_COVER_Y 100
#define SMOKE_RGB_MASK 0x00FFFFFFU
/* Full red in the 8888 frame buffer (ABGR). */
#define SMOKE_RED 0x000000FFU

static int smoke_cover(smoke *s, skiff_psp_app *platform) {
    skiff_cover *cover = aligned_alloc(SKIFF_COVER_ALIGNMENT, sizeof *cover);
    if (cover == NULL) {
        skiff_psp_report_line_offscreen(&s->report, "cover texture: no memory");
        return 0;
    }
    memset(cover, 0, sizeof *cover);
    cover->width = SMOKE_COVER_SIDE;
    cover->height = SMOKE_COVER_SIDE;
    for (int y = 0; y < SMOKE_COVER_SIDE; y++) {
        for (int x = 0; x < SMOKE_COVER_SIDE; x++) {
            cover->pixels[y * SKIFF_COVER_WIDTH + x] = skiff_cover_rgb565(0xFF, 0, 0);
        }
    }
    skiff_psp_ui_begin_frame(&platform->ui);
    skiff_psp_ui_cover(&platform->ui, SMOKE_COVER_X, SMOKE_COVER_Y, cover);
    skiff_psp_ui_end_frame(&platform->ui);
    const uint32_t *frame = skiff_psp_ui_drawn_frame(&platform->ui);
    int red = 0;
    for (int y = 0; y < SMOKE_COVER_SIDE; y++) {
        for (int x = 0; x < SMOKE_COVER_SIDE; x++) {
            const uint32_t pixel =
                frame[(SMOKE_COVER_Y + y) * SKIFF_PSP_UI_BUFFER_WIDTH + SMOKE_COVER_X + x];
            red += (pixel & SMOKE_RGB_MASK) == SMOKE_RED;
        }
    }
    const uint32_t beside =
        frame[SMOKE_COVER_Y * SKIFF_PSP_UI_BUFFER_WIDTH + SMOKE_COVER_X + SMOKE_COVER_SIDE];
    skiff_psp_ui_present(&platform->ui);
    free(cover);
    const int ok =
        red == SMOKE_COVER_SIDE * SMOKE_COVER_SIDE && (beside & SMOKE_RGB_MASK) != SMOKE_RED;
    char line[SKIFF_TEXT_MAX];
    snprintf(line, sizeof line, "cover texture: %d of %d pixels red, beside it %08lx: %s", red,
             SMOKE_COVER_SIDE * SMOKE_COVER_SIDE, (unsigned long)beside, ok ? "ok" : "FAILED");
    skiff_psp_report_line_offscreen(&s->report, line);
    return ok;
}
#endif

#ifndef SKIFF_APP_SMOKE
/* Without the app there is no GU screen: say why on the debug screen until the player quits. */
static void show_failure(skiff_err err) {
    char line[SKIFF_TEXT_MAX];
    (void)skiff_error_line(skiff_language_from_psp(skiff_psp_ui_system_language()), err, line,
                           sizeof line);
    pspDebugScreenInit();
    pspDebugScreenPrintf("Skiff %s\n\n%s\n\nPress HOME to quit.\n", skiff_version_string(), line);
    while (!skiff_psp_exit_requested()) {
        sceDisplayWaitVblankStart();
    }
}
#endif

int main(int argc, char *argv[]) {
    /* Static: the system writes into the dialog's parameters while it runs. */
    static skiff_psp_app platform;
    static skiff_psp_dialog dialog;
    skiff_psp_install_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
#ifdef SKIFF_APP_SMOKE
    static smoke s;
    skiff_psp_report_open(&s.report, argc > 0 ? argv[0] : NULL);
    skiff_psp_report_line_offscreen(&s.report, "Skiff app smoke test");
#endif

    skiff_err err = skiff_psp_app_start(&platform, argc > 0 ? argv[0] : NULL, &dialog);
    if (err == SKIFF_OK) {
        const skiff_app_config config = skiff_psp_app_config(&platform);
        const skiff_app_env env = skiff_psp_app_env(&platform);
        err = skiff_app_create(&config, &env, &platform.app);
    }
    if (err != SKIFF_OK) {
#ifdef SKIFF_APP_SMOKE
        char line[SKIFF_TEXT_MAX];
        snprintf(line, sizeof line, "start: %s (%d)", skiff_err_name(err), (int)err);
        skiff_psp_report_line(&s.report, line);
        skiff_psp_report_line(&s.report, SMOKE_FAIL_MARKER);
        skiff_psp_report_close(&s.report);
#else
        show_failure(err);
#endif
        sceKernelExitGame();
        return 1;
    }

    while (!skiff_psp_exit_requested() && !skiff_app_quit_requested(platform.app) &&
           !platform.dialog_stuck) {
#ifdef SKIFF_APP_SMOKE
        smoke_frame(&s, skiff_psp_app_step(&platform));
        if (s.done) {
            break;
        }
#else
        (void)skiff_psp_app_step(&platform);
#endif
    }

#ifdef SKIFF_APP_SMOKE
    if (s.ok) {
        s.ok = smoke_calls(&s, &platform);
    }
    if (s.ok) {
        s.ok = smoke_cover(&s, &platform);
    }
#endif
    const int released = skiff_psp_app_finish(&platform);
#ifdef SKIFF_APP_SMOKE
    pspDebugScreenInit();
    skiff_psp_report_line(&s.report, released ? "teardown: everything released"
                                              : "teardown: left to the process exit");
    skiff_psp_report_line(&s.report, s.ok && released ? SMOKE_OK_MARKER : SMOKE_FAIL_MARKER);
    skiff_psp_report_close(&s.report);
#else
    (void)released;
#endif
    sceKernelExitGame();
    return 0;
}
