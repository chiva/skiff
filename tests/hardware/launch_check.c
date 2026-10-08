/*
 * Launch check (hardware row A1, never packaged): a tiny homebrew game that scripts/dev.sh
 * launch-disc packs as a UMD disc image, an .iso and a .cso (tests/hardware/make_launch_disc.py),
 * which romm-lan seeds into the test RomM. Skiff downloads them like any PSP game; launching one
 * from the XMB then proves the file landed where the custom firmware's ISO loader finds it, and
 * reading the pattern file back proves the whole image reads back, through the loader's CSO
 * decompression too.
 *
 * The disc is read-only, so result.txt goes into Skiff's own folder (ms0:, or ef0: on a PSP Go),
 * which scripts/memstick.sh results prints. The result stays on screen for SHOW_SECONDS, or until a
 * button is pressed; PPSSPPHeadless (scripts/dev.sh launch-check) boots both images the same way.
 */
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdint.h>
#include <stdio.h>

#include "lifecycle.h"
#include "report.h"

#define OK_MARKER "SKIFF LAUNCH CHECK OK"
#define FAIL_MARKER "SKIFF LAUNCH CHECK FAIL"
/* The pattern file make_launch_disc.py writes: PATTERN_BYTES bytes, each pattern_byte(offset). */
#define PATTERN_PATH "disc0:/PSP_GAME/USRDIR/PATTERN.BIN"
#define PATTERN_BYTES (256 * 1024)
/* Where result.txt goes: Skiff's folder, whose EBOOT path the report takes its folder from. */
#define APP_FOLDER_MS "ms0:/PSP/GAME/Skiff"
#define APP_FOLDER_EF "ef0:/PSP/GAME/Skiff"
#define APP_EBOOT_NAME "/EBOOT.PBP"
#define SHOW_SECONDS 10
#define FRAMES_PER_SECOND 60

enum {
    CHECK_LINE_MAX = 160,
    PATH_MAX_LENGTH = 64,
    READ_CHUNK = 16 * 1024,
    /* make_launch_disc.py's pattern: a sector term, so sectors read in the wrong order differ. */
    SECTOR_SHIFT = 11,
    PATTERN_STEP = 131,
    PATTERN_SECTOR_STEP = 37,
    PATTERN_OFFSET = 7,
    BYTE_MASK = 0xFF,
};

static uint8_t pattern_byte(uint32_t offset) {
    return (uint8_t)((offset * PATTERN_STEP + (offset >> SECTOR_SHIFT) * PATTERN_SECTOR_STEP +
                      PATTERN_OFFSET) &
                     BYTE_MASK);
}

static int folder_exists(const char *path) {
    const SceUID dir = sceIoDopen(path);
    if (dir < 0) {
        return 0;
    }
    sceIoDclose(dir);
    return 1;
}

/* Reads the pattern file whole; 1 when every byte matches. */
static int check_pattern(skiff_psp_report *report) {
    static uint8_t chunk[READ_CHUNK];
    char line[CHECK_LINE_MAX];
    const SceUID file = sceIoOpen(PATTERN_PATH, PSP_O_RDONLY, 0);
    if (file < 0) {
        snprintf(line, sizeof line, "FAIL open %s: 0x%08X", PATTERN_PATH, (unsigned)file);
        skiff_psp_report_line(report, line);
        return 0;
    }
    uint32_t offset = 0;
    int ok = 1;
    for (;;) {
        const int read = sceIoRead(file, chunk, sizeof chunk);
        if (read < 0) {
            snprintf(line, sizeof line, "FAIL read at %u: 0x%08X", (unsigned)offset,
                     (unsigned)read);
            ok = 0;
            break;
        }
        if (read == 0) {
            break;
        }
        for (int i = 0; i < read && ok; i++) {
            if (chunk[i] != pattern_byte(offset + (uint32_t)i)) {
                snprintf(line, sizeof line, "FAIL byte %u: 0x%02X, expected 0x%02X",
                         (unsigned)(offset + (uint32_t)i), chunk[i],
                         pattern_byte(offset + (uint32_t)i));
                ok = 0;
            }
        }
        if (!ok) {
            break;
        }
        offset += (uint32_t)read;
    }
    sceIoClose(file);
    if (ok && offset != PATTERN_BYTES) {
        snprintf(line, sizeof line, "FAIL %s: %u bytes, expected %u", PATTERN_PATH,
                 (unsigned)offset, (unsigned)PATTERN_BYTES);
        ok = 0;
    }
    if (ok) {
        snprintf(line, sizeof line, "ok   %s: %u bytes read back", PATTERN_PATH, (unsigned)offset);
    }
    skiff_psp_report_line(report, line);
    return ok;
}

/* Until a button, HOME → Quit, or SHOW_SECONDS. */
static void show_result(void) {
    SceCtrlData pad;
    for (int frame = 0; frame < SHOW_SECONDS * FRAMES_PER_SECOND && !skiff_psp_exit_requested();
         frame++) {
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons != 0) {
            break;
        }
        sceDisplayWaitVblankStart();
    }
}

int main(int argc, char *argv[]) {
    static skiff_psp_report report;
    char line[CHECK_LINE_MAX];
    char result_owner[PATH_MAX_LENGTH];
    skiff_psp_install_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    snprintf(result_owner, sizeof result_owner, "%s%s",
             folder_exists(APP_FOLDER_MS) || !folder_exists(APP_FOLDER_EF) ? APP_FOLDER_MS
                                                                           : APP_FOLDER_EF,
             APP_EBOOT_NAME);
    skiff_psp_report_open(&report, result_owner);
    skiff_psp_report_line(&report, "Skiff launch check");
    snprintf(line, sizeof line, "launched as %s", argc > 0 ? argv[0] : "(no argv[0])");
    skiff_psp_report_line(&report, line);

    const int ok = check_pattern(&report);
    skiff_psp_report_line(&report, ok ? OK_MARKER : FAIL_MARKER);
    skiff_psp_report_line(&report, "Press any button to quit.");
    skiff_psp_report_close(&report);
    show_result();
    sceKernelExitGame();
    return 0;
}
