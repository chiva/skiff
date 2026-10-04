#include "report.h"

#include <errno.h>
#include <pspdebug.h>
#include <string.h>

#include "skiff/selftest.h"

enum {
    REPORT_PATH_MAX = 256,
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
    pspDebugScreenInit();

    const skiff_err err = skiff_selftest_result_path(program_path, path, sizeof path);
    if (err != SKIFF_OK) {
        snprintf(note, sizeof note, "result file: not written (%s)", skiff_err_name(err));
        print_line(note);
        return;
    }
    report->file = fopen(path, "w");
    if (report->file == NULL) {
        snprintf(note, sizeof note, "result file: not written (%s: %s)", path, strerror(errno));
        print_line(note);
    }
}

void skiff_psp_report_line(skiff_psp_report *report, const char *line) {
    print_line(line);
    if (report->file != NULL) {
        fprintf(report->file, "%s\n", line);
        fflush(report->file);
    }
}

void skiff_psp_report_close(skiff_psp_report *report) {
    if (report->file != NULL) {
        fclose(report->file);
        report->file = NULL;
    }
    fflush(stdout);
}
