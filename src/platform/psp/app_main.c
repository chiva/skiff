#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <stdio.h>

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
