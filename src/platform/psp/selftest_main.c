#include <pspdebug.h>
#include <pspkernel.h>
#include <stdio.h>

#include "skiff/selftest.h"

#include "lifecycle.h"

/* stdout reaches PPSSPPHeadless and PSPLINK's pspsh; the debug screen is for a player watching. */
static void log_line(void *ctx, const char *line) {
    (void)ctx;
    printf("%s\n", line);
    pspDebugScreenPrintf("%s\n", line);
}

int main(void) {
    skiff_psp_install_exit_callback();
    pspDebugScreenInit();
    skiff_selftest_result result = skiff_selftest_run(log_line, NULL);
    fflush(stdout);
    sceKernelExitGame();
    return result.failed == 0 ? 0 : 1;
}
