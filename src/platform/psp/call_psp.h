#ifndef SKIFF_PSP_CALL_PSP_H
#define SKIFF_PSP_CALL_PSP_H

/*
 * The app's browsing thread (skiff_app_env's call_start, call_done and call_cancel): it runs one
 * request to RomM at a time while the UI thread keeps drawing, so a slow network never freezes
 * the screen. Like the download worker (jobs_psp.h), it runs at a lower priority than the UI and
 * its stack comes from the memory the network modules use; its high-water mark is measured.
 *
 * The UI thread starts a call and asks once a frame whether it is done. The thread writes what
 * the call returns before it says so, and the UI reads it only after (both on the PSP's one CPU,
 * with a compiler barrier between).
 */

#include <pspkerneltypes.h>

#include "skiff/app.h"
#include "skiff/error.h"
#include "skiff/log.h"

/* A request uses what a download does (curl, TLS, cJSON): the worker's 32 KB, measured. */
#define SKIFF_PSP_CALLER_STACK_BYTES (32 * 1024)
/* Below the UI (0x20), like the worker. */
#define SKIFF_PSP_CALLER_PRIORITY 0x30
/* What skiff_psp_caller_stop() waits for the thread: a cancelled request ends within about a
 * second, but a name lookup runs to its own timeout. */
#define SKIFF_PSP_CALLER_STOP_TIMEOUT_US (5LL * 1000 * 1000)

typedef struct skiff_psp_caller {
    SceUID thread;
    /* Signalled for each call, and to stop. */
    SceUID wake;
    skiff_app_call_fn fn;
    void *arg;
    skiff_log *log;
    int stack_free_min;
    /* Set by the UI thread, read by the browsing thread. */
    volatile int cancel;
    volatile int stop;
    /* A call was started and has not been seen done; set by the thread once fn has returned. */
    volatile int busy;
    volatile int done;
    int started;
} skiff_psp_caller;

/* Starts the thread (caller zeroed). SKIFF_OK, or SKIFF_ERR_NO_MEMORY when the system has no room
 * for its stack or semaphore. log may be NULL. */
skiff_err skiff_psp_caller_start(skiff_psp_caller *caller, skiff_log *log);

/* Runs fn(arg) on the thread. SKIFF_ERR_INVALID_ARG while a call is busy or before start. */
skiff_err skiff_psp_caller_call(skiff_psp_caller *caller, skiff_app_call_fn fn, void *arg);

/* 1 once the call fn has returned (its writes are then visible); 1 too when none was started. */
int skiff_psp_caller_done(skiff_psp_caller *caller);

/* Asks the running call's transfers to stop (skiff_psp_caller_cancelled()). */
void skiff_psp_caller_cancel(skiff_psp_caller *caller);

/* Whether the running call should stop: cancelled, or the thread is stopping. */
int skiff_psp_caller_cancelled(const skiff_psp_caller *caller);

/*
 * Cancels the running call, ends the thread and waits up to timeout_us for it. SKIFF_OK once it has
 * ended (or never started); SKIFF_ERR_NET_TIMEOUT when it is still inside a call, which then keeps
 * using what it was given: leave that, and the thread, to the process exit.
 */
skiff_err skiff_psp_caller_stop(skiff_psp_caller *caller, long long timeout_us);

/* The least stack the thread had free so far, in bytes; -1 before it ran. */
int skiff_psp_caller_stack_free(skiff_psp_caller *caller);

#endif
