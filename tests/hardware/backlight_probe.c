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
 * player's back. A state file next to the EBOOT, written once before anything changes, tells the
 * runs apart and keeps the player's value, so a run that ends early (HOME, a crash) is still undone
 * by the next one. result.txt holds the last run; backlight-log.txt keeps every run's lines.
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
/* The value run 1 left, apart from the state file so that is written once. */
#define LEFT_FILE_NAME "backlight-probe.left"
#define LOG_FILE_NAME "backlight-log.txt"
#define STATE_KEY_ORIGINAL "original"
#define STATE_KEY_LEFT "left"
#define STATE_KEY_MAX 16
#define STATE_FIELDS 2
/* What sceIoGetstat() returns for a file that is not there (SCE_ERROR_ERRNO_ENOENT); any other
 * failure may hide a file that is. */
#define IO_NOT_FOUND ((int)0x80010002)
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

/* Next to the EBOOT; empty when there is no folder for them. */
static char log_path[PATH_MAX_LENGTH];
static char left_path[PATH_MAX_LENGTH];
/* A line could not be added to backlight-log.txt: the run is not reported as complete. */
static int log_failed;

static int64_t now_us(void) { return (int64_t)sceKernelGetSystemTimeWide(); }

/* Appends line to backlight-log.txt (opened per line: a suspend invalidates open files); 0 if it
 * could not be written whole. */
static int append_log(const char *line) {
    const SceUID file = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (file < 0) {
        return 0;
    }
    const int length = (int)strlen(line);
    const int ok =
        sceIoWrite(file, line, (SceSize)length) == length && sceIoWrite(file, "\n", 1) == 1;
    sceIoClose(file);
    return ok;
}

/* A report line, also kept in backlight-log.txt, so the second run's result.txt does not lose the
 * first run's answers. A line the log loses fails the run. */
static void note(skiff_psp_report *report, const char *line) {
    skiff_psp_report_line(report, line);
    if (!append_log(line) && !log_failed) {
        log_failed = 1;
        skiff_psp_report_line(report, "FAIL cannot write " LOG_FILE_NAME);
    }
}

/* The next new press among mask within timeout_s, or PRESS_NONE (also on HOME → Quit). Only the
 * auto-sleep timer is ticked, as during a download (skiff_psp_keep_awake()), so the PSP does not
 * sleep mid-watch while the backlight's timers keep running. *waited_ms says when it came. */
static press wait_press(unsigned mask, int timeout_s, int *waited_ms) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned held = pad.Buttons;
    const int64_t started = now_us();
    while (!skiff_psp_exit_requested() && now_us() - started < (int64_t)timeout_s * US_PER_SECOND) {
        sceDisplayWaitVblankStart();
        skiff_psp_keep_awake();
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
    note(report, line);
    int waited_ms = 0;
    const press answer = wait_press(PSP_CTRL_UP | PSP_CTRL_DOWN, ANSWER_TIMEOUT_S, &waited_ms);
    snprintf(line, sizeof line, "answer %s=%s", key,
             answer == PRESS_YES  ? "yes"
             : answer == PRESS_NO ? "no"
                                  : "none");
    note(report, line);
}

/* Sets the off time and reads it back: "set <value> -> <returned>, reads <value>". */
static int set_off_time(skiff_psp_report *report, int value) {
    const int returned = sceImposeSetBacklightOffTime(value);
    const int reads = sceImposeGetBacklightOffTime();
    char line[LINE_MAX_LENGTH];
    snprintf(line, sizeof line, "set %d -> %d, reads %d", value, returned, reads);
    note(report, line);
    return reads;
}

/* Leaves the PSP alone with off_s set and asks what the screen did; label names the power. */
static void watch(skiff_psp_report *report, const char *label, int off_s) {
    char line[LINE_MAX_LENGTH];
    snprintf(line, sizeof line, "watch %s: off time %d s, power online %d, battery %d %%", label,
             off_s, scePowerIsPowerOnline(), scePowerGetBatteryLifePercent());
    note(report, line);
    note(report, ACTION_PREFIX "put the PSP down and do not touch it. Once the "
                               "screen has gone dark, press X once (it lights it again).");
    int waited_ms = 0;
    const press woke = wait_press(PSP_CTRL_CROSS, off_s + DIM_AFTER_S + WATCH_MARGIN_S, &waited_ms);
    snprintf(line, sizeof line, "watch %s: %s after %d ms", label,
             woke == PRESS_CONFIRM ? "X reached the probe" : "no press", waited_ms);
    note(report, line);
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

typedef enum state_found {
    STATE_MISSING,
    STATE_READ,
    /* There, but it cannot be read: the player's value may be in it, so nothing is touched. */
    STATE_DAMAGED,
} state_found;

/* "key=value" from path. */
static state_found read_value(const char *path, const char *key, int *value) {
    SceIoStat stat;
    const int found = sceIoGetstat(path, &stat);
    if (found == IO_NOT_FOUND) {
        return STATE_MISSING;
    }
    if (found < 0) {
        return STATE_DAMAGED;
    }
    char text[STATE_TEXT_MAX] = "";
    const SceUID file = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (file < 0) {
        return STATE_DAMAGED;
    }
    const int read = sceIoRead(file, text, sizeof text - 1);
    sceIoClose(file);
    if (read <= 0) {
        return STATE_DAMAGED;
    }
    text[read] = '\0';
    /* Exactly what write_value() wrote, newline included: a cut "original=300" must not read as
     * 30 s. */
    char read_key[STATE_KEY_MAX] = "";
    int used = 0;
    const int complete = sscanf(text, "%15[^=]=%d%n", read_key, value, &used) == STATE_FIELDS &&
                         text[used] == '\n' && text[used + 1] == '\0';
    return complete && strcmp(read_key, key) == 0 ? STATE_READ : STATE_DAMAGED;
}

static int write_value(const char *path, const char *key, int value) {
    char text[STATE_TEXT_MAX];
    const int length = snprintf(text, sizeof text, "%s=%d\n", key, value);
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
    note(report, line);
    if (original < 0 || log_failed) {
        return 0;
    }
    /* Battery first, then AC: the watches compare them, and nothing has changed yet. */
    if (scePowerIsPowerOnline()) {
        note(report, ACTION_PREFIX "unplug the AC adapter, then press X (O to stop).");
        int waited_ms = 0;
        if (wait_press(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE, ANSWER_TIMEOUT_S, &waited_ms) !=
                PRESS_CONFIRM ||
            scePowerIsPowerOnline()) {
            note(report, "FAIL still on AC power: start on battery (nothing changed)");
            return 0;
        }
    }
    /* Kept first and never rewritten, so whatever happens next, run 2 puts it back. */
    if (!write_value(state_path, STATE_KEY_ORIGINAL, original)) {
        note(report, "FAIL cannot write " STATE_FILE_NAME);
        return 0;
    }
    for (size_t i = 0; i < sizeof TRIED_VALUES / sizeof TRIED_VALUES[0]; i++) {
        (void)set_off_time(report, TRIED_VALUES[i]);
    }
    const int watch_s = set_off_time(report, WATCH_OFF_TIME_S) == WATCH_OFF_TIME_S
                            ? WATCH_OFF_TIME_S
                            : set_off_time(report, PLAN_OFF_TIME_S);
    if (left_path[0] == '\0' || !write_value(left_path, STATE_KEY_LEFT, watch_s)) {
        note(report, "cannot write " LEFT_FILE_NAME ": run 2 will not tell whether it lasted");
    }
    watch(report, "battery", watch_s);
    note(report, ACTION_PREFIX "plug in the AC adapter, then press X (O to skip).");
    int waited_ms = 0;
    if (wait_press(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE, ANSWER_TIMEOUT_S, &waited_ms) ==
            PRESS_CONFIRM &&
        scePowerIsPowerOnline()) {
        watch(report, "ac", watch_s);
    } else {
        note(report, "watch ac: skipped");
    }

    snprintf(line, sizeof line,
             "left %d s set on purpose (player's %d s is in " STATE_FILE_NAME ")", watch_s,
             original);
    note(report, line);
    note(report, ACTION_PREFIX "HOME -> Quit. Check Settings > Power Save Settings > "
                               "Backlight Auto-Off, then turn the PSP off and on and "
                               "run this probe again: it puts your setting back.");
    /* HOME → Quit mid-run leaves the answers missing: not complete, and the state file stays. */
    return !log_failed && !skiff_psp_exit_requested();
}

/* Run 2: did the value run 1 left last, and the player's back. It cannot tell when run 1 left the
 * player's own value, when that value was not recorded, or when the getter fails. */
static int part_two(skiff_psp_report *report, const char *state_path, int original) {
    const int now = sceImposeGetBacklightOffTime();
    int left = 0;
    const int left_known =
        left_path[0] != '\0' && read_value(left_path, STATE_KEY_LEFT, &left) == STATE_READ;
    char line[LINE_MAX_LENGTH];
    snprintf(line, sizeof line,
             "run 2: off time %d s now, run 1 left %d s (known %d), player's %d s", now, left,
             left_known, original);
    note(report, line);
    note(report, !left_known || now < 0 || left == original ? "setting lasted: unknown"
                 : now == left                              ? "setting lasted: yes"
                                                            : "setting lasted: no");
    const int restored = set_off_time(report, original);
    if (restored != original) {
        snprintf(line, sizeof line, "FAIL could not put back %d s (reads %d)", original, restored);
        note(report, line);
        return 0;
    }
    /* A state file left behind would make the next launch a second run again, putting back a
     * setting the player may since have changed. */
    if (sceIoRemove(state_path) < 0) {
        note(report, "FAIL player's setting put back, but " STATE_FILE_NAME " remains: delete it");
        return 0;
    }
    if (left_path[0] != '\0') {
        sceIoRemove(left_path);
    }
    note(report, "player's setting put back; " STATE_FILE_NAME " removed");
    return !log_failed;
}

int main(int argc, char *argv[]) {
    static skiff_psp_report report;
    char state_path[PATH_MAX_LENGTH];
    skiff_psp_install_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    const char *program = argc > 0 ? argv[0] : "";
    skiff_psp_report_open(&report, program);
    if (!sibling_path(program, LOG_FILE_NAME, log_path)) {
        log_path[0] = '\0';
    }
    note(&report, "Skiff backlight probe");
    if (!sibling_path(program, LEFT_FILE_NAME, left_path)) {
        left_path[0] = '\0';
    }
    int ok = sibling_path(program, STATE_FILE_NAME, state_path);
    int original = 0;
    state_found found = STATE_MISSING;
    const char *marker = FAIL_MARKER;
    if (!ok) {
        note(&report, "FAIL no folder for " STATE_FILE_NAME);
    } else if ((found = read_value(state_path, STATE_KEY_ORIGINAL, &original)) == STATE_READ) {
        ok = part_two(&report, state_path, original);
        marker = ok ? OK_MARKER : FAIL_MARKER;
    } else if (found == STATE_DAMAGED) {
        ok = 0;
        note(&report, "FAIL " STATE_FILE_NAME " cannot be read: nothing changed. Set Backlight "
                      "Auto-Off by hand (your value is in backlight-log.txt), then delete it.");
    } else {
        ok = part_one(&report, state_path);
        marker = ok ? PART_ONE_MARKER : FAIL_MARKER;
    }
    note(&report, marker);
    skiff_psp_report_close(&report);
    int waited_ms = 0;
    (void)wait_press(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE, ANSWER_TIMEOUT_S, &waited_ms);
    sceKernelExitGame();
    return 0;
}
