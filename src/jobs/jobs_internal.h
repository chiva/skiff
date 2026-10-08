#ifndef SKIFF_JOBS_INTERNAL_H
#define SKIFF_JOBS_INTERNAL_H

/* What queue.c and runner.c share: the queue object, behind skiff/jobs.h's opaque type. */

#include "skiff/jobs.h"

#define JOBS_LOG_TAG "jobs"

/* What the queue file holds: the jobs, how many, and the id the next one gets. */
typedef struct jobs_table {
    skiff_job jobs[SKIFF_JOBS_MAX];
    size_t count;
    uint32_t next_id;
} jobs_table;

/*
 * Locking (skiff/jobs.h): the state lock guards table, the runner's flags, the events and the
 * progress, and is held only for moments. The commit lock is held for a whole change that saves
 * the queue file. A change is made to staged (a copy of table), staged is saved without the state
 * lock, and only then does it replace table under the state lock. Only a commit-lock holder writes
 * table, so it may read table without the state lock.
 */
struct skiff_jobs {
    skiff_storage *storage;
    char path[SKIFF_STORAGE_PATH_MAX];
    skiff_log *log;
    skiff_jobs_lock_fn lock;
    skiff_jobs_lock_fn unlock;
    void *lock_ctx;
    void *save_lock_ctx;
    jobs_table table;
    /* The commit-lock holder's copy of table, changed and saved before it is published. */
    jobs_table staged;
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

/* How a state change treats a queue file that could not be saved. */
typedef enum jobs_commit_mode {
    /* The player's change: kept only once the file holds it, so the queue never shows what a
     * restart would undo. */
    JOBS_COMMIT_IF_SAVED,
    /* What happened (the runner's): stands for the session either way; every later save writes
     * the whole queue again. */
    JOBS_COMMIT_ALWAYS,
} jobs_commit_mode;

/* The state lock and the commit lock. Take the commit lock first, never while holding the state
 * lock. */
void jobs_lock(skiff_jobs *jobs);
void jobs_unlock(skiff_jobs *jobs);
void jobs_commit_lock(skiff_jobs *jobs);
void jobs_commit_unlock(skiff_jobs *jobs);

/* The job with id in table, or NULL. */
skiff_job *jobs_table_find(jobs_table *table, uint32_t id);

/* Commit lock held: copies table into staged and returns staged, for a change. */
jobs_table *jobs_stage(skiff_jobs *jobs);

/* Commit lock held, state lock not: writes staged as the queue file; a failure is logged and
 * returned. */
skiff_err jobs_save_staged(skiff_jobs *jobs);

/* Both locks held: staged becomes table. */
void jobs_publish(skiff_jobs *jobs);

/* State lock held. A state event drops the progress pending for its job, which it supersedes. */
void jobs_push_event(skiff_jobs *jobs, const skiff_jobs_event *event);
void jobs_push_state_event(skiff_jobs *jobs, const skiff_job *job);
void jobs_set_progress(skiff_jobs *jobs, const skiff_jobs_event *event);

/* Commit lock held, state lock not: sets the job's state, error and attempts, saves the queue and,
 * as mode says, publishes the change and tells the UI. Returns the save's result, or
 * SKIFF_ERR_STORAGE_NOT_FOUND when no job has id. */
skiff_err jobs_commit_state(skiff_jobs *jobs, uint32_t id, skiff_job_state state, skiff_err error,
                            uint32_t attempts, jobs_commit_mode mode);

#endif
