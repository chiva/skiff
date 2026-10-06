#include "lifecycle.h"

#include <pspkernel.h>
#include <psppower.h>

enum {
    CALLBACK_THREAD_PRIORITY = 0x11,
    CALLBACK_THREAD_STACK_BYTES = 0x1000,
    /* scePowerRegisterCallback(): any free slot, else the first one, as pspsdk's samples use. */
    POWER_SLOT_ANY = -1,
    POWER_SLOT_FIRST = 0,
    /* How long installing waits for the callback thread to register (it normally runs at once,
     * its priority being higher than the main thread's). */
    REGISTRATION_POLL_US = 1000,
    REGISTRATION_POLLS = 100,
};

static volatile int exit_requested = 0;
static volatile int suspends = 0;
static volatile int resumes = 0;
static volatile int last_power_info = 0;
static volatile int power_registration = -1;
static volatile int callbacks_registered = 0;
static long long last_tick_us = 0;

static int on_exit_requested(int arg1, int arg2, void *common) {
    (void)arg1;
    (void)arg2;
    (void)common;
    exit_requested = 1;
    return 0;
}

/* Runs on the callback thread at each power event; only counts, the main loop does the rest. */
static int on_power_event(int count, int power_info, void *common) {
    (void)count;
    (void)common;
    last_power_info = power_info;
    if ((power_info & PSP_POWER_CB_SUSPENDING) != 0) {
        suspends++;
    }
    if ((power_info & PSP_POWER_CB_RESUME_COMPLETE) != 0) {
        resumes++;
    }
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    const int exit_callback = sceKernelCreateCallback("skiff_exit", on_exit_requested, NULL);
    sceKernelRegisterExitCallback(exit_callback);
    const int power_callback = sceKernelCreateCallback("skiff_power", on_power_event, NULL);
    int slot = scePowerRegisterCallback(POWER_SLOT_ANY, power_callback);
    if (slot < 0) {
        slot = scePowerRegisterCallback(POWER_SLOT_FIRST, power_callback);
    }
    power_registration = slot;
    callbacks_registered = 1;
    sceKernelSleepThreadCB();
    return 0;
}

void skiff_psp_install_callbacks(void) {
    SceUID thread_id =
        sceKernelCreateThread("skiff_callbacks", callback_thread, CALLBACK_THREAD_PRIORITY,
                              CALLBACK_THREAD_STACK_BYTES, 0, NULL);
    if (thread_id < 0 || sceKernelStartThread(thread_id, 0, NULL) < 0) {
        return;
    }
    /* So skiff_psp_power_events_now() reports the registration, not its initial value. */
    for (int poll = 0; poll < REGISTRATION_POLLS && !callbacks_registered; poll++) {
        sceKernelDelayThread(REGISTRATION_POLL_US);
    }
}

int skiff_psp_exit_requested(void) { return exit_requested; }

skiff_psp_power_events skiff_psp_power_events_now(void) {
    const skiff_psp_power_events events = {suspends, resumes, last_power_info, power_registration};
    return events;
}

void skiff_psp_keep_awake(void) {
    const long long now_us = sceKernelGetSystemTimeWide();
    if (last_tick_us != 0 && now_us - last_tick_us < SKIFF_PSP_KEEP_AWAKE_INTERVAL_US) {
        return;
    }
    last_tick_us = now_us;
    scePowerTick(PSP_POWER_TICK_SUSPEND);
}
