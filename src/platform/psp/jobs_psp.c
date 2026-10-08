#include "jobs_psp.h"

#include <psprtc.h>
#include <pspthreadman.h>
#include <pspwlan.h>
#include <string.h>

#include "lifecycle.h"

#define WORKER_THREAD_NAME "skiff_jobs"
#define LOG_TAG "jobs"
#define US_PER_MS 1000LL
#define MS_PER_S 1000ULL
/* sceRtc ticks are microseconds since 0001-01-01 00:00 UTC; this many seconds precede 1970. */
#define RTC_SECONDS_BEFORE_UNIX_EPOCH 62135596800ULL
#define US_PER_S 1000000ULL
#define WLAN_SWITCH_OFF 0
#define SEMA_ONE 1

/* ---- The mutex ---- */

skiff_err skiff_psp_mutex_create(skiff_psp_mutex *mutex, const char *name) {
    if (mutex == NULL || name == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    mutex->sema = sceKernelCreateSema(name, 0, SEMA_ONE, SEMA_ONE, NULL);
    return mutex->sema < 0 ? SKIFF_ERR_NO_MEMORY : SKIFF_OK;
}

void skiff_psp_mutex_destroy(skiff_psp_mutex *mutex) {
    if (mutex != NULL && mutex->sema >= 0) {
        sceKernelDeleteSema(mutex->sema);
        mutex->sema = -1;
    }
}

void skiff_psp_mutex_lock(void *ctx) {
    const skiff_psp_mutex *mutex = ctx;
    sceKernelWaitSema(mutex->sema, SEMA_ONE, NULL);
}

void skiff_psp_mutex_unlock(void *ctx) {
    const skiff_psp_mutex *mutex = ctx;
    sceKernelSignalSema(mutex->sema, SEMA_ONE);
}

int skiff_psp_mutex_try_lock(skiff_psp_mutex *mutex) {
    return mutex != NULL && sceKernelPollSema(mutex->sema, SEMA_ONE) >= 0;
}

int skiff_psp_utc_ms(void *ctx, int64_t *unix_ms) {
    (void)ctx;
    u64 tick = 0;
    if (unix_ms == NULL || sceRtcGetCurrentTick(&tick) < 0 ||
        tick / US_PER_S <= RTC_SECONDS_BEFORE_UNIX_EPOCH) {
        return 0;
    }
    *unix_ms = (int64_t)(tick / US_PER_MS - RTC_SECONDS_BEFORE_UNIX_EPOCH * MS_PER_S);
    return 1;
}

/* ---- The runner's hooks ---- */

static skiff_err open_transport(void *ctx, skiff_transport **out) {
    const skiff_psp_worker *worker = ctx;
    if (worker->config.transport_status != SKIFF_OK) {
        *out = NULL;
        return worker->config.transport_status;
    }
    return skiff_curl_transport_create(&worker->config.curl, out);
}

static int switch_on(void *ctx) {
    (void)ctx;
    return sceWlanGetSwitchState() != WLAN_SWITCH_OFF;
}

static int online(void *ctx) {
    const skiff_psp_worker *worker = ctx;
    return skiff_psp_net_online(worker->config.net) == SKIFF_OK;
}

static void lock_network(const skiff_psp_worker *worker) {
    if (worker->config.net_lock != NULL) {
        skiff_psp_mutex_lock(worker->config.net_lock);
    }
}

static void unlock_network(const skiff_psp_worker *worker) {
    if (worker->config.net_lock != NULL) {
        skiff_psp_mutex_unlock(worker->config.net_lock);
    }
}

/* Drops what is left of the connection (a suspend or a lost access point leaves it half there),
 * then joins the profile again. */
static skiff_err rejoin(void *ctx) {
    const skiff_psp_worker *worker = ctx;
    skiff_psp_net *net = worker->config.net;
    lock_network(worker);
    skiff_err err = skiff_psp_net_disconnect(net, SKIFF_PSP_WORKER_DISCONNECT_TIMEOUT_US);
    if (err == SKIFF_OK) {
        err = skiff_psp_net_connect(net, worker->config.profile, SKIFF_PSP_WORKER_JOIN_TIMEOUT_US);
    }
    unlock_network(worker);
    skiff_log_write(worker->config.log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_WARN, LOG_TAG,
                    "rejoin profile %d: %s (%d), %s 0x%08X", worker->config.profile,
                    skiff_err_name(err), (int)err,
                    net->failed_call != NULL ? net->failed_call : "-", (unsigned)net->sce_result);
    return err;
}

/* Unloads and loads the network modules, then joins. Never unloads under a connection that could
 * not be shown to be gone: that can hang the PSP (net_psp.h). */
static skiff_err reload(void *ctx) {
    const skiff_psp_worker *worker = ctx;
    skiff_psp_net *net = worker->config.net;
    lock_network(worker);
    skiff_err err = net->stage == SKIFF_PSP_NET_APCTL
                        ? skiff_psp_net_disconnect(net, SKIFF_PSP_WORKER_DISCONNECT_TIMEOUT_US)
                        : SKIFF_OK;
    if (err == SKIFF_OK) {
        err = skiff_psp_net_unload(net);
    }
    if (err == SKIFF_OK) {
        err = skiff_psp_net_load(net, SKIFF_PSP_NET_CPU_MHZ);
    }
    if (err == SKIFF_OK) {
        err = skiff_psp_net_connect(net, worker->config.profile, SKIFF_PSP_WORKER_JOIN_TIMEOUT_US);
    }
    unlock_network(worker);
    skiff_log_write(worker->config.log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_WARN, LOG_TAG,
                    "reload network: %s (%d), %s 0x%08X, clock %d MHz", skiff_err_name(err),
                    (int)err, net->failed_call != NULL ? net->failed_call : "-",
                    (unsigned)net->sce_result, net->cpu_mhz);
    return err;
}

static uint32_t suspends(void *ctx) {
    (void)ctx;
    const skiff_psp_power_events events = skiff_psp_power_events_now();
    return (uint32_t)events.suspends;
}

static void keep_awake(void *ctx) {
    (void)ctx;
    skiff_psp_keep_awake();
}

static int64_t now_ms(void *ctx) {
    (void)ctx;
    return (int64_t)(sceKernelGetSystemTimeWide() / US_PER_MS);
}

static void sleep_ms(void *ctx, uint32_t ms) {
    (void)ctx;
    sceKernelDelayThread((SceUInt)((long long)ms * US_PER_MS));
}

/* ---- The thread ---- */

static void measure_stack(skiff_psp_worker *worker) {
    const int free_bytes = sceKernelGetThreadStackFreeSize(worker->thread);
    if (free_bytes >= 0 && (worker->stack_free_min < 0 || free_bytes < worker->stack_free_min)) {
        worker->stack_free_min = free_bytes;
    }
}

static int worker_thread(SceSize args, void *argp) {
    (void)args;
    skiff_psp_worker *worker = *(skiff_psp_worker **)argp;
    while (!worker->stop) {
        int ran = 0;
        const skiff_err err = skiff_jobs_run_one(worker->config.jobs, &worker->env, &ran);
        if (err != SKIFF_OK) {
            skiff_log_write(worker->config.log, SKIFF_LOG_ERROR, LOG_TAG, "runner: %s (%d)",
                            skiff_err_name(err), (int)err);
            break;
        }
        if (!ran && !worker->stop) {
            sceKernelDelayThread((SceUInt)SKIFF_PSP_WORKER_IDLE_US);
        }
    }
    worker->running = 0;
    return 0;
}

skiff_err skiff_psp_worker_start(skiff_psp_worker *worker, const skiff_psp_worker_config *config) {
    if (worker == NULL || config == NULL || config->jobs == NULL || config->romm == NULL ||
        config->net == NULL || config->profile < 1) {
        return SKIFF_ERR_INVALID_ARG;
    }
    /* One worker per queue: a thread still running keeps using this struct. */
    if (worker->running) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(worker, 0, sizeof *worker);
    worker->config = *config;
    worker->thread = -1;
    worker->stack_free_min = -1;
    const skiff_jobs_env env = {
        .open_transport = open_transport,
        .switch_on = switch_on,
        .online = online,
        .rejoin = rejoin,
        .reload = reload,
        .suspends = suspends,
        .keep_awake = keep_awake,
        .now_ms = now_ms,
        .sleep_ms = sleep_ms,
        .ctx = worker,
        .romm = config->romm,
    };
    worker->env = env;
    const SceUID thread =
        sceKernelCreateThread(WORKER_THREAD_NAME, worker_thread, SKIFF_PSP_WORKER_PRIORITY,
                              SKIFF_PSP_WORKER_STACK_BYTES, 0, NULL);
    if (thread < 0) {
        skiff_log_write(config->log, SKIFF_LOG_ERROR, LOG_TAG,
                        "worker: sceKernelCreateThread 0x%08X (stack %d bytes)", (unsigned)thread,
                        SKIFF_PSP_WORKER_STACK_BYTES);
        return SKIFF_ERR_NO_MEMORY;
    }
    worker->thread = thread;
    worker->started = 1;
    worker->running = 1;
    skiff_psp_worker *self = worker;
    const int started = sceKernelStartThread(thread, sizeof self, &self);
    if (started < 0) {
        worker->started = 0;
        worker->running = 0;
        sceKernelDeleteThread(thread);
        worker->thread = -1;
        skiff_log_write(config->log, SKIFF_LOG_ERROR, LOG_TAG,
                        "worker: sceKernelStartThread 0x%08X", (unsigned)started);
        return SKIFF_ERR_NO_MEMORY;
    }
    skiff_log_write(config->log, SKIFF_LOG_INFO, LOG_TAG,
                    "worker: started, priority 0x%X, stack %d bytes", SKIFF_PSP_WORKER_PRIORITY,
                    SKIFF_PSP_WORKER_STACK_BYTES);
    return SKIFF_OK;
}

skiff_err skiff_psp_worker_stop(skiff_psp_worker *worker, long long timeout_us) {
    /* A zeroed worker has thread 0; the kernel's thread UIDs are positive. */
    if (worker == NULL || !worker->started || worker->thread <= 0) {
        return SKIFF_OK;
    }
    worker->stop = 1;
    skiff_jobs_request_stop(worker->config.jobs);
    SceUInt timeout = (SceUInt)timeout_us;
    const int ended = sceKernelWaitThreadEnd(worker->thread, &timeout);
    measure_stack(worker);
    if (ended < 0) {
        skiff_log_write(worker->config.log, SKIFF_LOG_WARN, LOG_TAG,
                        "worker: still busy after %lld ms (0x%08X); left to the process exit",
                        timeout_us / US_PER_MS, (unsigned)ended);
        return SKIFF_ERR_NET_TIMEOUT;
    }
    sceKernelDeleteThread(worker->thread);
    worker->thread = -1;
    skiff_log_write(worker->config.log, SKIFF_LOG_INFO, LOG_TAG,
                    "worker: stopped, lowest free stack %d of %d bytes", worker->stack_free_min,
                    SKIFF_PSP_WORKER_STACK_BYTES);
    return SKIFF_OK;
}

int skiff_psp_worker_stack_free(skiff_psp_worker *worker) {
    if (worker == NULL || !worker->started) {
        return -1;
    }
    if (worker->thread > 0) {
        measure_stack(worker);
    }
    return worker->stack_free_min;
}
