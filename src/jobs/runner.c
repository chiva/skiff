#include <stdio.h>
#include <string.h>

#include "jobs_internal.h"

/* How often the PSP is kept awake during a transfer. */
#define KEEP_AWAKE_INTERVAL_MS 1000
/* A speed is shown once an attempt has run this long. */
#define RATE_MIN_ELAPSED_MS 1000
#define MS_PER_S 1000U

/* One job being run: what the hooks the download engine calls need. */
typedef struct run {
    skiff_jobs *jobs;
    const skiff_jobs_env *env;
    uint32_t job_id;
    /* The suspend count when the attempt started: a different one means the PSP slept. */
    uint32_t suspends_seen;
    int suspended;
    int64_t last_awake_ms;
    /* Where and when this attempt's first bytes arrived, for its speed. */
    int has_rate_start;
    int64_t rate_start_ms;
    uint64_t rate_start_done;
} run;

static int env_valid(const skiff_jobs_env *env) {
    return env->open_transport != NULL && env->switch_on != NULL && env->online != NULL &&
           env->rejoin != NULL && env->reload != NULL && env->suspends != NULL &&
           env->keep_awake != NULL && env->now_ms != NULL && env->sleep_ms != NULL &&
           env->romm != NULL;
}

/* SKIFF_ERR_CANCELLED once the player cancelled the job or Skiff is quitting. */
static skiff_err asked_to_stop(run *r) {
    jobs_lock(r->jobs);
    const int stop = r->jobs->cancel_active || r->jobs->stop_requested;
    jobs_unlock(r->jobs);
    return stop ? SKIFF_ERR_CANCELLED : SKIFF_OK;
}

static skiff_err should_stop(void *ctx) {
    run *r = ctx;
    const skiff_jobs_env *env = r->env;
    const int64_t now = env->now_ms(env->ctx);
    if (now - r->last_awake_ms >= KEEP_AWAKE_INTERVAL_MS) {
        env->keep_awake(env->ctx);
        r->last_awake_ms = now;
    }
    const skiff_err stop = asked_to_stop(r);
    if (stop != SKIFF_OK) {
        return stop;
    }
    if (env->suspends(env->ctx) != r->suspends_seen) {
        r->suspended = 1;
        return SKIFF_ERR_NET_CONNECTION_LOST;
    }
    if (!env->switch_on(env->ctx)) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    return env->online(env->ctx) ? SKIFF_OK : SKIFF_ERR_NET_CONNECTION_LOST;
}

static void on_progress(void *ctx, uint64_t done, uint64_t total) {
    run *r = ctx;
    const int64_t now = r->env->now_ms(r->env->ctx);
    if (!r->has_rate_start) {
        r->has_rate_start = 1;
        r->rate_start_ms = now;
        r->rate_start_done = done;
    }
    skiff_jobs_event event;
    memset(&event, 0, sizeof event);
    event.kind = SKIFF_JOBS_EVENT_PROGRESS;
    event.job_id = r->job_id;
    event.done = done;
    event.total = total;
    const int64_t elapsed = now - r->rate_start_ms;
    if (elapsed >= RATE_MIN_ELAPSED_MS) {
        event.bytes_per_s = (done - r->rate_start_done) * MS_PER_S / (uint64_t)elapsed;
    }
    jobs_lock(r->jobs);
    jobs_set_progress(r->jobs, &event);
    jobs_unlock(r->jobs);
}

/* Reception is paused while the Memory Stick writes, so the log's batch goes out now for free. */
static void after_write(void *ctx) {
    const run *r = ctx;
    skiff_log_flush(r->jobs->log);
}

static void push_recovery(run *r, skiff_jobs_recovery step, uint32_t retry_in_ms) {
    skiff_jobs_event event;
    memset(&event, 0, sizeof event);
    event.kind = SKIFF_JOBS_EVENT_RECOVERY;
    event.job_id = r->job_id;
    event.step = step;
    event.retry_in_ms = retry_in_ms;
    jobs_lock(r->jobs);
    jobs_push_event(r->jobs, &event);
    jobs_unlock(r->jobs);
}

/* Sleeps ms in SKIFF_JOBS_POLL_MS steps; SKIFF_ERR_CANCELLED as soon as a stop is asked for. */
static skiff_err wait_ms(run *r, uint32_t ms) {
    while (ms > 0) {
        const skiff_err stop = asked_to_stop(r);
        if (stop != SKIFF_OK) {
            return stop;
        }
        const uint32_t step = ms < SKIFF_JOBS_POLL_MS ? ms : SKIFF_JOBS_POLL_MS;
        r->env->sleep_ms(r->env->ctx, step);
        ms -= step;
    }
    return asked_to_stop(r);
}

/* Waits for the Wi-Fi switch, however long it takes. */
static skiff_err wait_for_switch(run *r) {
    if (r->env->switch_on(r->env->ctx)) {
        return SKIFF_OK;
    }
    skiff_log_write(r->jobs->log, SKIFF_LOG_INFO, JOBS_LOG_TAG, "job %u: waiting for Wi-Fi",
                    (unsigned)r->job_id);
    push_recovery(r, SKIFF_JOBS_WAITING_FOR_WIFI, 0);
    while (!r->env->switch_on(r->env->ctx)) {
        const skiff_err stop = wait_ms(r, SKIFF_JOBS_POLL_MS);
        if (stop != SKIFF_OK) {
            return stop;
        }
    }
    return SKIFF_OK;
}

/* Joins the network again when it is gone: the access point first, then the modules. */
static skiff_err reconnect(run *r) {
    const skiff_jobs_env *env = r->env;
    if (!r->suspended && env->online(env->ctx)) {
        return SKIFF_OK;
    }
    r->suspended = 0;
    push_recovery(r, SKIFF_JOBS_REJOINING, 0);
    skiff_err err = env->rejoin(env->ctx);
    skiff_log_write(r->jobs->log, SKIFF_LOG_INFO, JOBS_LOG_TAG, "job %u: rejoin: %s (%d)",
                    (unsigned)r->job_id, skiff_err_name(err), (int)err);
    if (err == SKIFF_OK) {
        return SKIFF_OK;
    }
    push_recovery(r, SKIFF_JOBS_RELOADING, 0);
    err = env->reload(env->ctx);
    skiff_log_write(r->jobs->log, SKIFF_LOG_INFO, JOBS_LOG_TAG, "job %u: reload: %s (%d)",
                    (unsigned)r->job_id, skiff_err_name(err), (int)err);
    return err;
}

static uint32_t backoff_ms(int idle_attempts) {
    uint32_t delay = SKIFF_JOBS_BACKOFF_FIRST_MS;
    for (int i = 1; i < idle_attempts && delay < SKIFF_JOBS_BACKOFF_MAX_MS; i++) {
        delay *= 2;
    }
    return delay < SKIFF_JOBS_BACKOFF_MAX_MS ? delay : SKIFF_JOBS_BACKOFF_MAX_MS;
}

/* The folder target is in ("ms0:/ISO" for "ms0:/ISO/Game.iso"); 0 without one. */
static int folder_of(const char *target, char *out, size_t out_size) {
    const char *last_slash = strrchr(target, '/');
    if (last_slash == NULL || last_slash == target || (size_t)(last_slash - target) >= out_size) {
        return 0;
    }
    const size_t length = (size_t)(last_slash - target);
    memcpy(out, target, length);
    out[length] = '\0';
    return 1;
}

/* One attempt at job; result filled. */
static skiff_err attempt(run *r, skiff_transport *transport, const skiff_job *job,
                         skiff_download_result *result) {
    memset(result, 0, sizeof *result);
    char folder[SKIFF_JOBS_TARGET_MAX];
    if (!folder_of(job->target, folder, sizeof folder)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    /* A missing folder fails the attempt before its request (skiff_download_attempt()). */
    skiff_err err = skiff_storage_mkdirs(r->jobs->storage, folder);
    if (err != SKIFF_OK) {
        return err;
    }
    skiff_romm_file file;
    memset(&file, 0, sizeof file);
    memcpy(file.file_name, job->file_name, strlen(job->file_name) + 1);
    file.name_status = SKIFF_ROMM_NAME_OK;
    char url[SKIFF_ROMM_CONTENT_URL_MAX];
    err = skiff_romm_content_url(r->env->romm, job->rom_id, &file, url, sizeof url);
    if (err != SKIFF_OK) {
        return err;
    }
    skiff_http_header authorization;
    const size_t header_count = skiff_romm_auth_header(r->env->romm, &authorization);
    const skiff_download_spec spec = {
        .url = url,
        .headers = header_count > 0 ? &authorization : NULL,
        .header_count = header_count,
        .target_path = job->target,
        .replace_target = job->replace_target,
        .expected_size = job->size,
        .has_expected_crc32 = job->has_crc32,
        .expected_crc32 = job->crc32,
        .should_stop = should_stop,
        .stop_ctx = r,
        .on_progress = on_progress,
        .progress_ctx = r,
        .after_write = after_write,
        .write_ctx = r,
    };
    r->suspends_seen = r->env->suspends(r->env->ctx);
    r->has_rate_start = 0;
    return skiff_download_attempt(transport, r->jobs->storage, &spec, result);
}

/* Takes the first queued job, marks it active and copies it into *out; 0 when none is queued. */
static int take_next(skiff_jobs *jobs, skiff_job *out) {
    jobs_lock(jobs);
    /* Skiff is quitting: no job starts, however late the request came. */
    if (jobs->stop_requested) {
        jobs_unlock(jobs);
        return 0;
    }
    for (size_t i = 0; i < jobs->count; i++) {
        skiff_job *job = &jobs->jobs[i];
        if (job->state == SKIFF_JOB_QUEUED) {
            jobs->active_id = job->id;
            jobs->cancel_active = 0;
            (void)jobs_set_state(jobs, job, SKIFF_JOB_ACTIVE, SKIFF_OK);
            *out = *job;
            jobs_unlock(jobs);
            return 1;
        }
    }
    jobs_unlock(jobs);
    return 0;
}

/* How the job ended. The job may have moved in the table (UI calls), so it is found by id. */
static void conclude(run *r, const skiff_job *taken, skiff_job_state state, skiff_err error,
                     uint32_t attempts) {
    skiff_jobs *jobs = r->jobs;
    jobs_lock(jobs);
    skiff_job *job = jobs_find(jobs, r->job_id);
    if (job != NULL) {
        job->attempts = attempts;
        const skiff_err saved = jobs_set_state(jobs, job, state, error);
        /* A cancel's partial files go only once the queue file says cancelled: if it could not be
         * saved, a restart finds the job queued with its progress intact. The lock is held until
         * they are gone, so no other job can claim the target meanwhile. Files that cannot be
         * deleted fail the job with the Memory Stick's error; a retry resumes from them. */
        if (state == SKIFF_JOB_CANCELLED && saved == SKIFF_OK) {
            const skiff_err err = skiff_download_discard(jobs->storage, taken->target);
            if (err != SKIFF_OK) {
                state = SKIFF_JOB_FAILED;
                error = err;
                (void)jobs_set_state(jobs, job, state, error);
            }
        }
    }
    jobs->active_id = 0;
    jobs->cancel_active = 0;
    jobs_unlock(jobs);
    skiff_log_write(jobs->log, state == SKIFF_JOB_FAILED ? SKIFF_LOG_ERROR : SKIFF_LOG_INFO,
                    JOBS_LOG_TAG, "job %u %s after %u attempt(s): %s (%d)", (unsigned)r->job_id,
                    skiff_job_state_name(state), (unsigned)attempts, skiff_err_name(error),
                    (int)error);
}

/* The state a stopped attempt leaves: cancelled by the player, or queued again for later. */
static skiff_job_state stopped_state(skiff_jobs *jobs) {
    jobs_lock(jobs);
    const int cancelled = jobs->cancel_active;
    jobs_unlock(jobs);
    return cancelled ? SKIFF_JOB_CANCELLED : SKIFF_JOB_QUEUED;
}

skiff_err skiff_jobs_run_one(skiff_jobs *jobs, const skiff_jobs_env *env, int *ran) {
    if (ran != NULL) {
        *ran = 0;
    }
    if (jobs == NULL || env == NULL || ran == NULL || !env_valid(env)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_job job;
    if (!take_next(jobs, &job)) {
        return SKIFF_OK;
    }
    *ran = 1;
    run r;
    memset(&r, 0, sizeof r);
    r.jobs = jobs;
    r.env = env;
    r.job_id = job.id;
    r.last_awake_ms = env->now_ms(env->ctx);
    skiff_log_write(jobs->log, SKIFF_LOG_INFO, JOBS_LOG_TAG, "job %u: start, %llu bytes",
                    (unsigned)job.id, (unsigned long long)job.size);
    skiff_transport *transport = NULL;
    uint32_t attempts = job.attempts;
    int idle_attempts = 0;
    skiff_job_state state = SKIFF_JOB_FAILED;
    skiff_err err = SKIFF_OK;
    for (;;) {
        if (transport == NULL) {
            err = env->open_transport(env->ctx, &transport);
            if (err != SKIFF_OK) {
                transport = NULL;
                break;
            }
        }
        skiff_download_result result;
        err = attempt(&r, transport, &job, &result);
        attempts++;
        if (err == SKIFF_OK) {
            state = SKIFF_JOB_DONE;
            break;
        }
        if (err == SKIFF_ERR_CANCELLED) {
            state = stopped_state(jobs);
            break;
        }
        skiff_log_write(jobs->log, SKIFF_LOG_WARN, JOBS_LOG_TAG,
                        "job %u attempt %u: %s (%d), %llu bytes received", (unsigned)job.id,
                        (unsigned)attempts, skiff_err_name(err), (int)err,
                        (unsigned long long)result.bytes_received);
        /* A suspend invalidates open Memory Stick files: a write that failed across one costs the
         * attempt, not the job, and the download resumes from its last checkpoint. */
        if (env->suspends(env->ctx) != r.suspends_seen) {
            r.suspended = 1;
        }
        if (!skiff_download_retryable(err) && !(r.suspended && err == SKIFF_ERR_STORAGE_IO)) {
            break;
        }
        /* A broken connection is not reused; the next attempt opens a new one. */
        skiff_transport_destroy(transport);
        transport = NULL;
        if (result.bytes_received > 0) {
            idle_attempts = 0;
        } else if (err != SKIFF_ERR_NET_UNAVAILABLE) {
            idle_attempts++;
        }
        if (idle_attempts >= SKIFF_JOBS_IDLE_ATTEMPTS_MAX) {
            break;
        }
        skiff_err step = SKIFF_OK;
        if (idle_attempts > 0) {
            const uint32_t delay = backoff_ms(idle_attempts);
            push_recovery(&r, SKIFF_JOBS_RETRYING, delay);
            step = wait_ms(&r, delay);
        }
        if (step == SKIFF_OK) {
            step = wait_for_switch(&r);
        }
        if (step == SKIFF_OK && reconnect(&r) != SKIFF_OK) {
            /* Counted like an attempt that received nothing; the next one tries again. */
            idle_attempts++;
            if (idle_attempts >= SKIFF_JOBS_IDLE_ATTEMPTS_MAX) {
                break;
            }
        }
        if (step == SKIFF_ERR_CANCELLED) {
            err = SKIFF_ERR_CANCELLED;
            state = stopped_state(jobs);
            break;
        }
    }
    skiff_transport_destroy(transport);
    if (state == SKIFF_JOB_DONE || state == SKIFF_JOB_QUEUED) {
        err = SKIFF_OK;
    } else if (state == SKIFF_JOB_CANCELLED) {
        err = SKIFF_ERR_CANCELLED;
    }
    conclude(&r, &job, state, err, attempts);
    return SKIFF_OK;
}
