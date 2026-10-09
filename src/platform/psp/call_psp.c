#include "call_psp.h"

#include <pspthreadman.h>
#include <string.h>

#define CALLER_THREAD_NAME "skiff_browse"
#define CALLER_SEMA_NAME "skiff_browse_wake"
#define LOG_TAG "app"
#define US_PER_MS 1000LL
#define SEMA_ONE 1
#define SEMA_MAX 1

/* The thread's writes (what the call returned, then done) and the UI's reads (done, then the
 * results) stay in that order: the PSP runs both on one CPU, so the compiler is all that could
 * reorder them. */
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

static void measure_stack(skiff_psp_caller *caller) {
    const int free_bytes = sceKernelGetThreadStackFreeSize(caller->thread);
    if (free_bytes >= 0 && (caller->stack_free_min < 0 || free_bytes < caller->stack_free_min)) {
        caller->stack_free_min = free_bytes;
    }
}

static int caller_thread(SceSize args, void *argp) {
    (void)args;
    skiff_psp_caller *caller = *(skiff_psp_caller **)argp;
    for (;;) {
        sceKernelWaitSema(caller->wake, SEMA_ONE, NULL);
        if (caller->stop) {
            break;
        }
        COMPILER_BARRIER();
        caller->fn(caller->arg);
        COMPILER_BARRIER();
        caller->done = 1;
    }
    return 0;
}

skiff_err skiff_psp_caller_start(skiff_psp_caller *caller, skiff_log *log) {
    if (caller == NULL || caller->started) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(caller, 0, sizeof *caller);
    caller->log = log;
    caller->thread = -1;
    caller->stack_free_min = -1;
    caller->wake = sceKernelCreateSema(CALLER_SEMA_NAME, 0, 0, SEMA_MAX, NULL);
    if (caller->wake < 0) {
        skiff_log_write(log, SKIFF_LOG_ERROR, LOG_TAG, "browse: sceKernelCreateSema 0x%08X",
                        (unsigned)caller->wake);
        return SKIFF_ERR_NO_MEMORY;
    }
    const SceUID thread =
        sceKernelCreateThread(CALLER_THREAD_NAME, caller_thread, SKIFF_PSP_CALLER_PRIORITY,
                              SKIFF_PSP_CALLER_STACK_BYTES, 0, NULL);
    skiff_psp_caller *self = caller;
    const int started = thread < 0 ? thread : sceKernelStartThread(thread, sizeof self, &self);
    if (started < 0) {
        skiff_log_write(log, SKIFF_LOG_ERROR, LOG_TAG, "browse: thread 0x%08X (stack %d bytes)",
                        (unsigned)started, SKIFF_PSP_CALLER_STACK_BYTES);
        if (thread >= 0) {
            sceKernelDeleteThread(thread);
        }
        sceKernelDeleteSema(caller->wake);
        return SKIFF_ERR_NO_MEMORY;
    }
    caller->thread = thread;
    caller->started = 1;
    skiff_log_write(log, SKIFF_LOG_INFO, LOG_TAG, "browse: started, priority 0x%X, stack %d bytes",
                    SKIFF_PSP_CALLER_PRIORITY, SKIFF_PSP_CALLER_STACK_BYTES);
    return SKIFF_OK;
}

skiff_err skiff_psp_caller_call(skiff_psp_caller *caller, skiff_app_call_fn fn, void *arg) {
    if (caller == NULL || fn == NULL || !caller->started || caller->busy || caller->stop) {
        return SKIFF_ERR_INVALID_ARG;
    }
    caller->fn = fn;
    caller->arg = arg;
    caller->cancel = 0;
    caller->done = 0;
    caller->busy = 1;
    COMPILER_BARRIER();
    sceKernelSignalSema(caller->wake, SEMA_ONE);
    return SKIFF_OK;
}

int skiff_psp_caller_done(skiff_psp_caller *caller) {
    if (caller == NULL || !caller->busy) {
        return 1;
    }
    if (!caller->done) {
        return 0;
    }
    COMPILER_BARRIER();
    caller->busy = 0;
    measure_stack(caller);
    return 1;
}

void skiff_psp_caller_cancel(skiff_psp_caller *caller) {
    if (caller != NULL) {
        caller->cancel = 1;
    }
}

int skiff_psp_caller_cancelled(const skiff_psp_caller *caller) {
    return caller != NULL && (caller->cancel || caller->stop);
}

skiff_err skiff_psp_caller_stop(skiff_psp_caller *caller, long long timeout_us) {
    if (caller == NULL || !caller->started) {
        return SKIFF_OK;
    }
    caller->stop = 1;
    caller->cancel = 1;
    COMPILER_BARRIER();
    sceKernelSignalSema(caller->wake, SEMA_ONE);
    SceUInt timeout = (SceUInt)timeout_us;
    const int ended = sceKernelWaitThreadEnd(caller->thread, &timeout);
    measure_stack(caller);
    if (ended < 0) {
        skiff_log_write(caller->log, SKIFF_LOG_WARN, LOG_TAG,
                        "browse: still in a request after %lld ms (0x%08X); left to the process "
                        "exit",
                        timeout_us / US_PER_MS, (unsigned)ended);
        return SKIFF_ERR_NET_TIMEOUT;
    }
    sceKernelDeleteThread(caller->thread);
    sceKernelDeleteSema(caller->wake);
    caller->thread = -1;
    caller->started = 0;
    skiff_log_write(caller->log, SKIFF_LOG_INFO, LOG_TAG,
                    "browse: stopped, lowest free stack %d of %d bytes", caller->stack_free_min,
                    SKIFF_PSP_CALLER_STACK_BYTES);
    return SKIFF_OK;
}

int skiff_psp_caller_stack_free(skiff_psp_caller *caller) {
    if (caller == NULL || caller->thread < 0) {
        return caller != NULL ? caller->stack_free_min : -1;
    }
    measure_stack(caller);
    return caller->stack_free_min;
}
