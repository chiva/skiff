#ifndef SKIFF_JOBS_INTERNAL_H
#define SKIFF_JOBS_INTERNAL_H

/* What queue.c and runner.c share: the queue object, behind skiff/jobs.h's opaque type. */

#include "skiff/jobs.h"

#define JOBS_LOG_TAG "jobs"

struct skiff_jobs {
    skiff_storage *storage;
    char path[SKIFF_STORAGE_PATH_MAX];
    skiff_log *log;
    skiff_jobs_lock_fn lock;
    skiff_jobs_lock_fn unlock;
    void *lock_ctx;
    skiff_job jobs[SKIFF_JOBS_MAX];
    size_t count;
    uint32_t next_id;
    /* The job skiff_jobs_run_one() is running (0 for none), and what was asked of it. */
    uint32_t active_id;
    int cancel_active;
    int stop_requested;
    /* Discrete events, oldest at events[event_head], and the latest progress. */
    skiff_jobs_event events[SKIFF_JOBS_EVENTS_MAX];
    size_t event_head;
    size_t event_count;
    int has_progress;
    skiff_jobs_event progress;
};

void jobs_lock(skiff_jobs *jobs);
void jobs_unlock(skiff_jobs *jobs);

/* The job with id, or NULL. Lock held. */
skiff_job *jobs_find(skiff_jobs *jobs, uint32_t id);

/* Writes the queue file; a failure is logged and returned. Lock held. */
skiff_err jobs_save(skiff_jobs *jobs);

/* Lock held. A state event drops the progress pending for its job, which it supersedes. */
void jobs_push_event(skiff_jobs *jobs, const skiff_jobs_event *event);
void jobs_set_progress(skiff_jobs *jobs, const skiff_jobs_event *event);

/* Sets a job's state, saves the queue and tells the UI; returns the save's result (the state stands
 * for the session either way). Lock held. */
skiff_err jobs_set_state(skiff_jobs *jobs, skiff_job *job, skiff_job_state state, skiff_err error);

#endif
