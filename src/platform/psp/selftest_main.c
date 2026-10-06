#include <pspkernel.h>

#include "skiff/selftest.h"

#include "lifecycle.h"
#include "report.h"

static void log_line(void *ctx, const char *line) { skiff_psp_report_line(ctx, line); }

int main(int argc, char *argv[]) {
    skiff_psp_report report;

    skiff_psp_install_callbacks();
    skiff_psp_report_open(&report, argc > 0 ? argv[0] : NULL);
    skiff_selftest_result result = skiff_selftest_run(log_line, &report);
    skiff_psp_report_close(&report);
    sceKernelExitGame();
    return result.failed == 0 ? 0 : 1;
}
