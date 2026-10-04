#include "lifecycle.h"

#include <pspkernel.h>

enum {
    CALLBACK_THREAD_PRIORITY = 0x11,
    CALLBACK_THREAD_STACK_BYTES = 0x1000,
};

static volatile int exit_requested = 0;

static int on_exit_requested(int arg1, int arg2, void *common) {
    (void)arg1;
    (void)arg2;
    (void)common;
    exit_requested = 1;
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    int callback_id = sceKernelCreateCallback("skiff_exit", on_exit_requested, NULL);
    sceKernelRegisterExitCallback(callback_id);
    sceKernelSleepThreadCB();
    return 0;
}

void skiff_psp_install_exit_callback(void) {
    SceUID thread_id =
        sceKernelCreateThread("skiff_callbacks", callback_thread, CALLBACK_THREAD_PRIORITY,
                              CALLBACK_THREAD_STACK_BYTES, 0, NULL);
    if (thread_id >= 0) {
        sceKernelStartThread(thread_id, 0, NULL);
    }
}

int skiff_psp_exit_requested(void) { return exit_requested; }
