#ifndef SKIFF_PSP_JOBS_PSP_H
#define SKIFF_PSP_JOBS_PSP_H

/*
 * The download queue's worker on the PSP (skiff/jobs.h): a thread that runs skiff_jobs_run_one()
 * until Skiff quits, and the platform hooks the runner asks for (skiff_jobs_env): the curl
 * transport, the Wi-Fi switch and access point, rejoining or reloading the network (net_psp.h), the
 * suspend count and keep-awake (lifecycle.h), and the system clock. Also the mutex the queue (its
 * state and commit locks) and the log lock with, and the real-time clock the log stamps its lines
 * with.
 *
 * The worker runs at a lower priority than the UI thread. The PSP's kernel never time-slices
 * threads of equal priority and always runs the highest-priority ready thread, so the UI, which
 * waits for every vertical blank, always draws first, and the worker downloads in what is left.
 *
 * Thread stacks come from the same memory as the network modules: once Wi-Fi is joined, a PSP-1000
 * has about 148 KB of it free, in blocks of at most 80 KB (docs/development/hardware-findings.md).
 * The stack's high-water mark is measured (skiff_psp_worker_stack_free()) so its size can be set
 * from what a real download uses.
 */

#include <pspkerneltypes.h>
#include <stdint.h>

#include "skiff/curl_transport.h"
#include "skiff/error.h"
#include "skiff/jobs.h"
#include "skiff/log.h"
#include "skiff/romm.h"

#include "net_psp.h"

/* The worker's stack; must fit in one free block once Wi-Fi is joined (see above). */
#define SKIFF_PSP_WORKER_STACK_BYTES (64 * 1024)
/* The main (UI) thread runs at 0x20; a larger number runs only when the UI waits. */
#define SKIFF_PSP_WORKER_PRIORITY 0x30
/* How often an idle worker looks for a queued job. */
#define SKIFF_PSP_WORKER_IDLE_US (250LL * 1000)
/* What skiff_psp_worker_stop() waits for the thread at most: the runner stops a transfer within a
 * second, but a join or a disconnect in progress runs to its own timeout. */
#define SKIFF_PSP_WORKER_STOP_TIMEOUT_US (5LL * 1000 * 1000)
/* Rejoining the access point: how long a join and a disconnect may take. A PSP-1000 joined in 7 to
 * 13 s. */
#define SKIFF_PSP_WORKER_JOIN_TIMEOUT_US (30LL * 1000 * 1000)
#define SKIFF_PSP_WORKER_DISCONNECT_TIMEOUT_US (10LL * 1000 * 1000)

/* ---- A mutex, for skiff_jobs_config and skiff_log_config's lock hooks ---- */

typedef struct skiff_psp_mutex {
    SceUID sema;
} skiff_psp_mutex;

/* SKIFF_ERR_INVALID_ARG for NULL, SKIFF_ERR_NO_MEMORY when the kernel refuses the semaphore. */
skiff_err skiff_psp_mutex_create(skiff_psp_mutex *mutex, const char *name);
void skiff_psp_mutex_destroy(skiff_psp_mutex *mutex);
/* ctx is the skiff_psp_mutex. Not recursive: the queue's state lock, its commit lock and the log
 * each need their own, since the queue takes its state lock and logs while it holds its commit
 * lock. */
void skiff_psp_mutex_lock(void *ctx);
void skiff_psp_mutex_unlock(void *ctx);
/* Takes the mutex if it is free: 1 then, 0 (without waiting) when another thread holds it. */
int skiff_psp_mutex_try_lock(skiff_psp_mutex *mutex);

/* skiff_log_clock_fn: milliseconds since 1970 in UTC from the real-time clock (the C library's
 * time() has no date on a PSP); 0 when the clock cannot be read. */
int skiff_psp_utc_ms(void *ctx, int64_t *unix_ms);

/* ---- The worker ---- */

typedef struct skiff_psp_worker_config {
    /* Not owned; must outlive the worker. */
    skiff_jobs *jobs;
    const skiff_romm_client *romm;
    /* The loaded network stack and the Network Settings profile to rejoin. */
    skiff_psp_net *net;
    int profile;
    /* Copied; the strings and headers it points to must outlive the worker. */
    skiff_curl_config curl;
    /* SKIFF_OK once TLS has started (skiff_net_global_init()); otherwise every job fails with it
     * before any request, e.g. SKIFF_ERR_NET_NEEDS_ARK. */
    skiff_err transport_status;
    /* Taken around rejoining and reloading the network, which other threads must not do at the
     * same time; NULL when no other thread does. */
    skiff_psp_mutex *net_lock;
    /* NULL for no log. */
    skiff_log *log;
} skiff_psp_worker_config;

typedef struct skiff_psp_worker {
    skiff_psp_worker_config config;
    skiff_jobs_env env;
    SceUID thread;
    volatile int stop;
    /* skiff_psp_worker_start() created the thread (0 in a zeroed worker). */
    int started;
    /* The thread is between its start and its end. */
    volatile int running;
    /* The lowest free stack measured, in bytes; -1 before the first measurement. */
    int stack_free_min;
} skiff_psp_worker;

/*
 * Starts the worker thread; worker must be zeroed, or stopped by skiff_psp_worker_stop(). A stop is
 * final for its queue (skiff_jobs_request_stop() is never cleared), so a worker started again after
 * a stop needs a new skiff_jobs; on the old one it would run nothing.
 * SKIFF_ERR_INVALID_ARG for a NULL argument, jobs, romm or net, a profile below 1, or a worker
 * whose thread still runs (one queue, one worker); SKIFF_ERR_NO_MEMORY when the kernel cannot
 * create the thread (its stack does not fit), with the firmware's result in the log.
 */
skiff_err skiff_psp_worker_start(skiff_psp_worker *worker, const skiff_psp_worker_config *config);

/*
 * Asks the runner to stop (skiff_jobs_request_stop(): the active job stays queued with its progress
 * saved) and waits for the thread up to timeout_us. SKIFF_OK once it has ended and been deleted;
 * SKIFF_ERR_NET_TIMEOUT when it is still busy (a join or a disconnect in progress). The thread may
 * then still use the queue, the log, the RomM client, TLS and the network: the caller must not free
 * or tear down any of them, and should exit (sceKernelExitGame() ends the thread with the process).
 * Does nothing (SKIFF_OK) for a worker never started, zeroed or whose start failed.
 */
skiff_err skiff_psp_worker_stop(skiff_psp_worker *worker, long long timeout_us);

/* The lowest free stack the thread has had since it started, in bytes (the kernel fills a new
 * stack with a pattern and counts what is still untouched); also kept after the thread ends.
 * -1 before the worker started. */
int skiff_psp_worker_stack_free(skiff_psp_worker *worker);

#endif
