#ifndef SKIFF_PSP_REPORT_H
#define SKIFF_PSP_REPORT_H

#include <stdio.h>

/*
 * Output for check EBOOTs (self-test, probes). Every line goes to stdout (PPSSPPHeadless, PSPLINK),
 * the debug screen, and result.txt next to the EBOOT, flushed per line so a crash on hardware still
 * leaves the lines written before it. Running from the XMB then needs no PSPLINK: read result.txt
 * from the Memory Stick (scripts/memstick.sh results).
 *
 * A suspend invalidates files the program has open on the Memory Stick (seen on a PSP-1000: writes
 * after waking failed silently). A line that cannot be written reopens result.txt for appending
 * and is written again, so a report survives a suspend.
 */
#define SKIFF_PSP_REPORT_PATH_MAX 256

typedef struct skiff_psp_report {
    FILE *file;
    /* result.txt, to reopen after a suspend; empty when there is none. */
    char path[SKIFF_PSP_REPORT_PATH_MAX];
} skiff_psp_report;

/* program_path is argv[0]. Without a writable result file, stdout and the screen still work. */
void skiff_psp_report_open(skiff_psp_report *report, const char *program_path);

void skiff_psp_report_line(skiff_psp_report *report, const char *line);

/*
 * stdout and result.txt only, for EBOOTs that draw their own frames (GU): the debug screen writes
 * straight into VRAM and would scribble over them. Lets such an EBOOT log each step as it happens,
 * so a crash still leaves the steps before it.
 */
void skiff_psp_report_line_offscreen(skiff_psp_report *report, const char *line);

/*
 * The debug screen only, on the line the next report line will use, which then overwrites it: live
 * progress (a download's bytes and speed) that would flood result.txt if logged.
 */
void skiff_psp_report_status(skiff_psp_report *report, const char *line);

void skiff_psp_report_close(skiff_psp_report *report);

#endif
