#include "report.h"

#include <errno.h>
#include <pspdebug.h>
#include <string.h>

#include "skiff/selftest.h"

enum {
    /* Characters per debug screen line (480 pixels, 7 per character). */
    REPORT_SCREEN_COLUMNS = 68,
    REPORT_PATH_MAX = SKIFF_PSP_REPORT_PATH_MAX,
    /* A note quotes the whole path plus the reason, so it must hold more than a path. */
    REPORT_NOTE_MAX = REPORT_PATH_MAX + SKIFF_SELFTEST_LINE_MAX,
};

static void print_line(const char *line) {
    printf("%s\n", line);
    pspDebugScreenPrintf("%s\n", line);
}

void skiff_psp_report_open(skiff_psp_report *report, const char *program_path) {
    char path[REPORT_PATH_MAX];
    char note[REPORT_NOTE_MAX];

    report->file = NULL;
    report->path[0] = '\0';
    pspDebugScreenInit();

    const skiff_err err = skiff_selftest_result_path(program_path, path, sizeof path);
    if (err != SKIFF_OK) {
        snprintf(note, sizeof note, "result file: not written (%s)", skiff_err_name(err));
        print_line(note);
        return;
    }
    report->file = fopen(path, "w");
    if (report->file != NULL) {
        snprintf(report->path, sizeof report->path, "%s", path);
    } else {
        snprintf(note, sizeof note, "result file: not written (%s: %s)", path, strerror(errno));
        print_line(note);
    }
}

static int put_line(FILE *file, const char *line) {
    return fprintf(file, "%s\n", line) >= 0 && fflush(file) == 0;
}

/* A failed write means the file handle died (a suspend does that): reopen and write once more. A
 * reopen that fails is tried again with the next line, so a slow wake loses as little as possible.
 */
static void write_line(skiff_psp_report *report, const char *line) {
    if (report->file != NULL && put_line(report->file, line)) {
        return;
    }
    if (report->file != NULL) {
        fclose(report->file);
    }
    report->file = report->path[0] != '\0' ? fopen(report->path, "a") : NULL;
    if (report->file != NULL) {
        put_line(report->file, line);
    }
}

void skiff_psp_report_line(skiff_psp_report *report, const char *line) {
    print_line(line);
    write_line(report, line);
}

void skiff_psp_report_line_offscreen(skiff_psp_report *report, const char *line) {
    printf("%s\n", line);
    write_line(report, line);
}

void skiff_psp_report_status(skiff_psp_report *report, const char *line) {
    (void)report;
    const int x = pspDebugScreenGetX();
    const int y = pspDebugScreenGetY();
    /* Padded to the full width, so a shorter status wipes a longer one. */
    pspDebugScreenPrintf("%-*.*s", REPORT_SCREEN_COLUMNS - 1, REPORT_SCREEN_COLUMNS - 1, line);
    pspDebugScreenSetXY(x, y);
}

void skiff_psp_report_close(skiff_psp_report *report) {
    if (report->file != NULL) {
        fclose(report->file);
        report->file = NULL;
    }
    fflush(stdout);
}
