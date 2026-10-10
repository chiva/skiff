/*
 * Backlight probe (hardware, never packaged): what Skiff can do about the backlight during a long
 * download without kernel calls, before it does it. The plan is to shorten the firmware's own
 * backlight-off time while a download runs (sceImposeSetBacklightOffTime(), a user-mode call) and
 * put the player's setting back afterwards; the firmware keeps doing the dimming and the waking.
 *
 * Run 1 records the player's setting, tries the values the plan needs, then watches, with the
 * person holding the PSP, whether the screen dims and goes dark on battery and on AC power, and
 * whether the press that wakes it reaches the program. It leaves a short setting behind on purpose.
 * Run 2, after HOME → Quit and a power cycle, says whether that setting lasted and puts the
 * player's back. A state file next to the EBOOT tells the runs apart and keeps the player's value,
 * so a run that ends early (HOME, a crash) is still undone by the next one.
 *
 * Nothing here reads the brightness: sceDisplayGetBrightness() has no user-mode stub in pspsdk, so
 * the person answers Up (yes) or Down (no). Hardware only: PPSSPPHeadless has no one to answer.
 */
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspimpose.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lifecycle.h"
#include "report.h"

#define OK_MARKER "SKIFF BACKLIGHT PROBE OK"
#define PART_ONE_MARKER "SKIFF BACKLIGHT PROBE PART 1 OK"
#define FAIL_MARKER "SKIFF BACKLIGHT PROBE FAIL"
#define STATE_FILE_NAME "backlight-probe.state"
#define STATE_FORMAT "original=%d left=%d\n"
#define STATE_FIELDS 2
#define ACTION_PREFIX "ACTION: "

enum {
    LINE_MAX_LENGTH = 192,
    PATH_MAX_LENGTH = 256,
    STATE_TEXT_MAX = 64,
    /* Seconds: what the plan would set (the XMB's shortest choice is 120), and the shorter values
     * worth knowing about. */
    PLAN_OFF_TIME_S = 120,
    WATCH_OFF_TIME_S = 60,
    /* The firmware dims the screen after a minute, whatever the off time. */
    DIM_AFTER_S = 60,
    /* Watched past the off time before giving up on the screen going dark. */
    WATCH_MARGIN_S = 30,
    ANSWER_TIMEOUT_S = 120,
    US_PER_SECOND = 1000 * 1000,
    US_PER_MS = 1000,
};

static const int TRIED_VALUES[] = {30, WATCH_OFF_TIME_S, PLAN_OFF_TIME_S};

typedef enum press {
    PRESS_NONE,
    PRESS_YES,
    PRESS_NO,
    PRESS_CONFIRM,
    PRESS_SKIP,
} press;

static int64_t now_us(void) { return (int64_t)sceKernelGetSystemTimeWide(); }

/* The next new press among mask within timeout_s, or PRESS_NONE (also on HOME → Quit). Frames are
 * waited on, never the power timers ticked, so the firmware's idle timers keep running. *waited_ms
 * says when it came. */
static press wait_press(unsigned mask, int timeout_s, int *waited_ms) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned held = pad.Buttons;
    const int64_t started = now_us();
    while (!skiff_psp_exit_requested() && now_us() - started < (int64_t)timeout_s * US_PER_SECOND) {
        sceDisplayWaitVblankStart();
        sceCtrlPeekBufferPositive(&pad, 1);
        const unsigned pressed = pad.Buttons & ~held & mask;
        held = pad.Buttons;
        if (pressed == 0) {
            continue;
        }
        *waited_ms = (int)((now_us() - started) / US_PER_MS);
        return (pressed & PSP_CTRL_UP)      ? PRESS_YES
               : (pressed & PSP_CTRL_DOWN)  ? PRESS_NO
               : (pressed & PSP_CTRL_CROSS) ? PRESS_CONFIRM
                                            : PRESS_SKIP;
    }
    *waited_ms = (int)((now_us() - started) / US_PER_MS);
    return PRESS_NONE;
}

/* question, answered Up (yes) or Down (no): "answer <key>=yes|no|none". */
static void ask(skiff_psp_report *report, const char *key, const char *question) {
    char line[LINE_MAX_LENGTH];
    snprintf(line, sizeof line, ACTION_PREFIX "%s Up = yes, Down = no", question);
    skiff_psp_report_line(report, line);
    int waited_ms = 0;
    const press answer = wait_press(PSP_CTRL_UP | PSP_CTRL_DOWN, ANSWER_TIMEOUT_S, &waited_ms);
    snprintf(line, sizeof line, "answer %s=%s", key,
             answer == PRESS_YES  ? "yes"
             : answer == PRESS_NO ? "no"
                                  : "none");
    skiff_psp_report_line(report, line);
}

/* Sets the off time and reads it back: "set <value> -> <returned>, reads <value>". */
static int set_off_time(skiff_psp_report *report, int value) {
    const int returned = sceImposeSetBacklightOffTime(value);
    const int reads = sceImposeGetBacklightOffTime();
    char line[LINE_MAX_LENGTH];
    snprintf(line, sizeof line, "set %d -> %d, reads %d", value, returned, reads);
    skiff_psp_report_line(report, line);
    return reads;
}

/* Leaves the PSP alone with off_s set and asks what the screen did; label names the power. */
static void watch(skiff_psp_report *report, const char *label, int off_s) {
    char line[LINE_MAX_LENGTH];
    snprintf(line, sizeof line, "watch %s: off time %d s, power online %d, battery %d %%", label,
             off_s, scePowerIsPowerOnline(), scePowerGetBatteryLifePercent());
    skiff_psp_report_line(report, line);
    skiff_psp_report_line(report,
                          ACTION_PREFIX "put the PSP down and do not touch it. Once the "
                                        "screen has gone dark, press X once (it lights it again).");
    int waited_ms = 0;
    const press woke = wait_press(PSP_CTRL_CROSS, off_s + DIM_AFTER_S + WATCH_MARGIN_S, &waited_ms);
    snprintf(line, sizeof line, "watch %s: %s after %d ms", label,
             woke == PRESS_CONFIRM ? "X reached the probe" : "no press", waited_ms);
    skiff_psp_report_line(report, line);
    ask(report, "dimmed", "Did the screen dim before going dark (or before you pressed)?");
    ask(report, "dark", "Did the screen go dark before you pressed X?");
    ask(report, "woke", "Did that X light the screen again?");
}

static int sibling_path(const char *program_path, const char *name, char *out) {
    const char *slash = strrchr(program_path, '/');
    if (slash == NULL) {
        return 0;
    }
    const int written =
        snprintf(out, PATH_MAX_LENGTH, "%.*s/%s", (int)(slash - program_path), program_path, name);
    return written > 0 && written < PATH_MAX_LENGTH;
}

static int read_state(const char *path, int *original, int *left) {
    char text[STATE_TEXT_MAX] = "";
    const SceUID file = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (file < 0) {
        return 0;
    }
    const int read = sceIoRead(file, text, sizeof text - 1);
    sceIoClose(file);
    if (read <= 0) {
        return 0;
    }
    text[read] = '\0';
    return sscanf(text, STATE_FORMAT, original, left) == STATE_FIELDS;
}

static int write_state(const char *path, int original, int left) {
    char text[STATE_TEXT_MAX];
    const int length = snprintf(text, sizeof text, STATE_FORMAT, original, left);
    const SceUID file = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (file < 0) {
        return 0;
    }
    const int written = sceIoWrite(file, text, (SceSize)length);
    sceIoClose(file);
    return written == length;
}

/* Run 1: the values, the watches, and a short setting left behind. */
static int part_one(skiff_psp_report *report, const char *state_path) {
    const int original = sceImposeGetBacklightOffTime();
    char line[LINE_MAX_LENGTH];
    snprintf(line, sizeof line, "player's off time %d s (0 = Off); power online %d, battery %d %%",
             original, scePowerIsPowerOnline(), scePowerGetBatteryLifePercent());
    skiff_psp_report_line(report, line);
    if (original < 0) {
        return 0;
    }
    /* Kept first, so whatever happens next, run 2 puts it back. */
    if (!write_state(state_path, original, WATCH_OFF_TIME_S)) {
        skiff_psp_report_line(report, "FAIL cannot write " STATE_FILE_NAME);
        return 0;
    }
    for (size_t i = 0; i < sizeof TRIED_VALUES / sizeof TRIED_VALUES[0]; i++) {
        (void)set_off_time(report, TRIED_VALUES[i]);
    }
    const int watch_s = set_off_time(report, WATCH_OFF_TIME_S) == WATCH_OFF_TIME_S
                            ? WATCH_OFF_TIME_S
                            : set_off_time(report, PLAN_OFF_TIME_S);
    (void)write_state(state_path, original, watch_s);
    watch(report, scePowerIsPowerOnline() ? "ac" : "battery", watch_s);
    if (!scePowerIsPowerOnline()) {
        skiff_psp_report_line(report,
                              ACTION_PREFIX "plug in the AC adapter, then press X (O to skip).");
        int waited_ms = 0;
        if (wait_press(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE, ANSWER_TIMEOUT_S, &waited_ms) ==
                PRESS_CONFIRM &&
            scePowerIsPowerOnline()) {
            watch(report, "ac", watch_s);
        } else {
            skiff_psp_report_line(report, "watch ac: skipped");
        }
    }
    snprintf(line, sizeof line,
             "left %d s set on purpose (player's %d s is in " STATE_FILE_NAME ")", watch_s,
             original);
    skiff_psp_report_line(report, line);
    skiff_psp_report_line(report,
                          ACTION_PREFIX "HOME -> Quit. Check Settings > Power Save Settings > "
                                        "Backlight Auto-Off, then turn the PSP off and on and "
                                        "run this probe again: it puts your setting back.");
    return 1;
}

/* Run 2: did the setting last, and the player's back. */
static int part_two(skiff_psp_report *report, const char *state_path, int original, int left) {
    const int now = sceImposeGetBacklightOffTime();
    char line[LINE_MAX_LENGTH];
    snprintf(line, sizeof line, "run 2: off time %d s now, %d s left by run 1, player's %d s", now,
             left, original);
    skiff_psp_report_line(report, line);
    skiff_psp_report_line(report, now == left ? "setting lasted: yes" : "setting lasted: no");
    const int restored = set_off_time(report, original);
    if (restored != original) {
        snprintf(line, sizeof line, "FAIL could not put back %d s (reads %d)", original, restored);
        skiff_psp_report_line(report, line);
        return 0;
    }
    sceIoRemove(state_path);
    skiff_psp_report_line(report, "player's setting put back; " STATE_FILE_NAME " removed");
    return 1;
}

int main(int argc, char *argv[]) {
    static skiff_psp_report report;
    char state_path[PATH_MAX_LENGTH];
    skiff_psp_install_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    const char *program = argc > 0 ? argv[0] : "";
    skiff_psp_report_open(&report, program);
    skiff_psp_report_line(&report, "Skiff backlight probe");

    int ok = sibling_path(program, STATE_FILE_NAME, state_path);
    int original = 0;
    int left = 0;
    const char *marker = FAIL_MARKER;
    if (!ok) {
        skiff_psp_report_line(&report, "FAIL no folder for " STATE_FILE_NAME);
    } else if (read_state(state_path, &original, &left)) {
        ok = part_two(&report, state_path, original, left);
        marker = ok ? OK_MARKER : FAIL_MARKER;
    } else {
        ok = part_one(&report, state_path);
        marker = ok ? PART_ONE_MARKER : FAIL_MARKER;
    }
    skiff_psp_report_line(&report, marker);
    skiff_psp_report_close(&report);
    int waited_ms = 0;
    (void)wait_press(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE, ANSWER_TIMEOUT_S, &waited_ms);
    sceKernelExitGame();
    return 0;
}
