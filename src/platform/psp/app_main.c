#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>

#include "skiff/version.h"

#include "lifecycle.h"

int main(void) {
    skiff_psp_install_exit_callback();
    pspDebugScreenInit();
    pspDebugScreenPrintf("Skiff %s\n\n", skiff_version_string());
    pspDebugScreenPrintf(
        "A RomM client for the PSP. Nothing to do yet: this is a scaffold build.\n");
    pspDebugScreenPrintf("Press START to exit.\n");

    SceCtrlData pad;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    while (!skiff_psp_exit_requested()) {
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START) {
            break;
        }
        sceDisplayWaitVblankStart();
    }

    sceKernelExitGame();
    return 0;
}
