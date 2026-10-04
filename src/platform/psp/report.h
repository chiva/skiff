#ifndef SKIFF_PSP_REPORT_H
#define SKIFF_PSP_REPORT_H

#include <stdio.h>

/*
 * Output for check EBOOTs (self-test, probes). Every line goes to stdout (PPSSPPHeadless, PSPLINK),
 * the debug screen, and result.txt next to the EBOOT, flushed per line so a crash on hardware still
 * leaves the lines written before it. Running from the XMB then needs no PSPLINK: read result.txt
 * from the Memory Stick (scripts/memstick.sh results).
 */
typedef struct skiff_psp_report {
    FILE *file;
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

void skiff_psp_report_close(skiff_psp_report *report);

#endif
