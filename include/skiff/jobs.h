#ifndef SKIFF_JOBS_H
#define SKIFF_JOBS_H

/*
 * The download queue: what the player asked to download, kept in a file on the Memory Stick
 * (PSP/GAME/Skiff/queue.json) so a quit, a crash or a flat battery loses nothing, and the runner
 * that works through it one job at a time. The UI thread adds, cancels and lists jobs; a worker
 * thread calls skiff_jobs_run_one(). Two locks share the hooks (both hooks, or neither for one
 * thread): the state lock (lock_ctx) guards what the UI reads every frame (the jobs, the events,
 * the progress) and is held only for moments, never across Memory Stick I/O; the commit lock
 * (save_lock_ctx) is held for a whole change that saves the queue file, so changes reach the file
 * in order and the UI never waits for a save. The commit lock is always taken first. The runner
 * holds neither during a transfer.
 *
 * A job is one file of one ROM. Its target path is decided when it is added (the installer picks
 * it, skiff/storage_paths.h), and the download URL is built from the RomM client when the job runs,
 * so a changed server address or token applies to jobs already queued.
 *
 * The runner owns retrying (skiff_download_attempt() is one attempt):
 *   - an error a reconnect cannot fix (skiff_download_retryable() is 0) fails the job with it;
 *   - with the Wi-Fi switch off the runner waits for it, however long, and that is not a failure;
 *   - after any other network failure it retries at once if the attempt received bytes, otherwise
 *     after 1, 2, 4, 8 and 16 s (doubling, at most SKIFF_JOBS_BACKOFF_MAX_MS); it rejoins the
 *     access point when the network is gone (reloading the network modules when rejoining fails),
 *     and gives up after SKIFF_JOBS_IDLE_ATTEMPTS_MAX attempts in a row that received nothing (a
 *     failed rejoin counts as one); any attempt that receives bytes resets that count;
 *   - after a suspend it rejoins before the next attempt (the connection is gone), and a Memory
 *     Stick error from an attempt the PSP slept through costs only that attempt (open files do not
 *     survive a suspend);
 *   - a cancelled job stops within about a second, and its partial files are deleted.
 * Every attempt resumes from the .part file (skiff/download.h), so retrying costs only time.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/download.h"
#include "skiff/error.h"
#include "skiff/log.h"
#include "skiff/romm.h"
#include "skiff/storage.h"
#include "skiff/transport.h"

#define SKIFF_JOBS_FILE_NAME "queue.json"
#define SKIFF_JOBS_VERSION 1
/* Jobs the queue holds, finished ones included until cleared. */
#define SKIFF_JOBS_MAX 64
/* The queue file is at most this large: every string field of every job at its longest, escaped. */
#define SKIFF_JOBS_FILE_MAX ((size_t)128 * 1024)
#define SKIFF_JOBS_TITLE_MAX SKIFF_ROMM_NAME_MAX
#define SKIFF_JOBS_FILE_NAME_MAX SKIFF_ROMM_FILE_NAME_MAX
#define SKIFF_JOBS_TARGET_MAX SKIFF_DOWNLOAD_PATH_MAX

/* The retry policy (see above). */
#define SKIFF_JOBS_IDLE_ATTEMPTS_MAX 6
#define SKIFF_JOBS_BACKOFF_FIRST_MS 1000U
#define SKIFF_JOBS_BACKOFF_MAX_MS 30000U
/* How often a wait looks at cancel and stop requests and at the Wi-Fi switch. */
#define SKIFF_JOBS_POLL_MS 250U
/* Discrete events kept for the UI; past this the oldest is dropped. */
#define SKIFF_JOBS_EVENTS_MAX 16

typedef enum skiff_job_state {
    SKIFF_JOB_QUEUED,
    SKIFF_JOB_ACTIVE,
    SKIFF_JOB_DONE,
    SKIFF_JOB_FAILED,
    SKIFF_JOB_CANCELLED,
} skiff_job_state;

typedef struct skiff_job {
    uint64_t rom_id;
    uint64_t size;
    /* Never reused while the queue file lives. */
    uint32_t id;
    int has_crc32;
    uint32_t crc32;
    skiff_job_state state;
    /* Why the job failed, or the last attempt's error while it retries; SKIFF_OK otherwise. */
    skiff_err error;
    uint32_t attempts;
    /* The ROM's name, for the queue screen. */
    char title[SKIFF_JOBS_TITLE_MAX];
    /* The file's name in RomM, which its download URL is built from. */
    char file_name[SKIFF_JOBS_FILE_NAME_MAX];
    /* Where the file goes (a real path, e.g. "ms0:/ISO/Game.iso"). */
    char target[SKIFF_JOBS_TARGET_MAX];
    /* The file at target is Skiff's own earlier copy, replace_size bytes long, and may be replaced
     * (skiff_install_plan's replaces_own and own_size); otherwise a file found there when the
     * download finishes is never removed (skiff_download_spec.replace_target). */
    int replace_target;
    uint64_t replace_size;
} skiff_job;

/* What skiff_jobs_add() and skiff_jobs_add_many() take: a job before it has an id and a state. */
typedef struct skiff_job_request {
    uint64_t rom_id;
    const char *title;
    const char *file_name;
    const char *target;
    uint64_t size;
    int has_crc32;
    uint32_t crc32;
    /* See skiff_job.replace_target and replace_size. */
    int replace_target;
    uint64_t replace_size;
} skiff_job_request;

typedef void (*skiff_jobs_lock_fn)(void *ctx);

/*
 * A job's file is complete at its target. Called on the runner's thread once per download that
 * finished, with the job as it was taken (its rom_id, file_name, target, size and CRC-32), before
 * the job is saved as done, so the UI's done event comes after it; neither queue lock is held, so
 * it may call skiff_jobs_list() and the like. The installer records the file here
 * (skiff/install.h): what it cannot record counts as copied by hand, which is never overwritten.
 */
typedef void (*skiff_jobs_downloaded_fn)(void *ctx, const skiff_job *job);

typedef struct skiff_jobs_config {
    /* Not owned; must outlive the queue. */
    skiff_storage *storage;
    /* The queue file (copied). */
    const char *path;
    /* NULL for no log. */
    skiff_log *log;
    /* Both or neither; NULL for one thread. */
    skiff_jobs_lock_fn lock;
    skiff_jobs_lock_fn unlock;
    /* The state lock, passed to the hooks. */
    void *lock_ctx;
    /* The commit lock, passed to the same hooks: required with them, and not lock_ctx (the hooks
     * are not expected to nest one lock). */
    void *save_lock_ctx;
    /* NULL for none. */
    skiff_jobs_downloaded_fn downloaded;
    void *downloaded_ctx;
} skiff_jobs_config;

typedef struct skiff_jobs skiff_jobs;

/* ---- Events for the UI ---- */

typedef enum skiff_jobs_event_kind {
    /* A job changed state (queued, active, done, failed, cancelled). */
    SKIFF_JOBS_EVENT_STATE,
    /* The runner is recovering the network; step says how. */
    SKIFF_JOBS_EVENT_RECOVERY,
    /* How far the active job is. Only the latest is kept. */
    SKIFF_JOBS_EVENT_PROGRESS,
} skiff_jobs_event_kind;

typedef enum skiff_jobs_recovery {
    SKIFF_JOBS_WAITING_FOR_WIFI,
    SKIFF_JOBS_REJOINING,
    SKIFF_JOBS_RELOADING,
    /* The next attempt starts in retry_in_ms. */
    SKIFF_JOBS_RETRYING,
} skiff_jobs_recovery;

typedef struct skiff_jobs_event {
    skiff_jobs_event_kind kind;
    uint32_t job_id;
    /* SKIFF_JOBS_EVENT_STATE */
    skiff_job_state state;
    skiff_err error;
    /* SKIFF_JOBS_EVENT_RECOVERY */
    skiff_jobs_recovery step;
    uint32_t retry_in_ms;
    /* SKIFF_JOBS_EVENT_PROGRESS: bytes of the file on the Memory Stick or buffered, the file's
     * size, and the speed of the current attempt in bytes per second (0 until measurable). */
    uint64_t done;
    uint64_t total;
    uint64_t bytes_per_s;
} skiff_jobs_event;

/* ---- What the runner needs from the platform ---- */

typedef struct skiff_jobs_env {
    /* Opens a transport for the next attempts (the runner destroys it after a reconnect and at the
     * end of the job). */
    skiff_err (*open_transport)(void *ctx, skiff_transport **out);
    /* The Wi-Fi switch is on. */
    int (*switch_on)(void *ctx);
    /* The access point is joined and the network can carry a request. */
    int (*online)(void *ctx);
    /* Joins the saved access point again; then reloads the network modules and joins. */
    skiff_err (*rejoin)(void *ctx);
    skiff_err (*reload)(void *ctx);
    /* How many times the PSP has suspended since Skiff started. */
    uint32_t (*suspends)(void *ctx);
    /* Keeps the PSP from sleeping during a download; called about once a second. */
    void (*keep_awake)(void *ctx);
    /* A monotonic clock in milliseconds, and a sleep. */
    int64_t (*now_ms)(void *ctx);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    void *ctx;
    /* The server and token the download URL and its Authorization header come from. */
    const skiff_romm_client *romm;
} skiff_jobs_env;

/* ---- The queue ---- */

/*
 * Creates the queue and reads its file (free it with skiff_jobs_destroy()). A missing file is an
 * empty queue. A file that is not a queue (damaged, another version) is not used: the queue starts
 * empty, the log says why, and the next save replaces the file; a job that cannot be used (a field
 * missing or too long) is dropped the same way, the rest kept. A job left active by a quit or a
 * crash is queued again: its .part file lets it resume. Returns SKIFF_ERR_INVALID_ARG for a NULL
 * argument or storage, an empty path, only one lock hook, lock hooks without a save_lock_ctx or
 * with save_lock_ctx equal to lock_ctx, or a path too long for
 * SKIFF_STORAGE_PATH_MAX; SKIFF_ERR_NO_MEMORY; the storage's error when the file cannot be read.
 * *out is NULL on error.
 */
skiff_err skiff_jobs_create(const skiff_jobs_config *config, skiff_jobs **out);

/* Frees the queue (the file stays). Does nothing for NULL. */
void skiff_jobs_destroy(skiff_jobs *jobs);

/*
 * Queues a download and saves the queue; its id goes to *id. The same file of the same ROM is not
 * queued twice: a job for it that is queued or active is kept (its id returned), one that ended
 * (done, failed, cancelled) is queued again. When the queue is full, finished jobs (done and
 * cancelled, oldest first) make room. Returns SKIFF_ERR_INVALID_ARG for a NULL argument, a missing
 * file name or target, a field too long, a target too long for its .part and .resume files
 * (SKIFF_DOWNLOAD_PATH_MAX) or whose files (target, .part, .resume) another unfinished job also
 * uses (ignoring case, as FAT does), a control character, a zero size, or a ROM id of 10^15 or
 * more (the queue file could not hold it exactly); the storage's error when the queue cannot be
 * saved (the job is not added). SKIFF_ERR_BUFFER_TOO_SMALL when SKIFF_JOBS_MAX jobs are queued,
 * active or failed is only a safety net: the UI checks the count (skiff_jobs_list()) before it
 * offers a download, and tells the player the queue is full.
 */
skiff_err skiff_jobs_add(skiff_jobs *jobs, const skiff_job_request *request, uint32_t *id);

/*
 * Queues count downloads, in order, with one save of the queue (a save blocks for a moment on the
 * Memory Stick, so queueing many games one by one would cost one each). Each request follows
 * skiff_jobs_add()'s rules: the same file of the same ROM queued or active keeps its job (a second
 * request for it in the batch gets the first one's id), one that ended is queued again, and
 * finished jobs make room. The batch stops at the first request that finds SKIFF_JOBS_MAX jobs
 * queued, active or failed: the requests before it are queued and *added says how many (fewer than
 * count; the player is told how many were left out). *added counts requests that have a job, new
 * or not, and ids[i] is set for every i below it. Returns SKIFF_ERR_INVALID_ARG, adding nothing,
 * for a NULL queue or added, NULL requests or ids with count above 0, or any request
 * skiff_jobs_add() would refuse, including one whose files (target, .part, .resume) another request
 * of the batch for another file also uses (ignoring case, as FAT does), all checked before anything
 * is queued; the storage's error when the queue cannot be saved (nothing is added). count 0 queues
 * nothing and saves nothing, as does a batch whose files are all queued or active already.
 */
skiff_err skiff_jobs_add_many(skiff_jobs *jobs, const skiff_job_request *requests, size_t count,
                              uint32_t *ids, size_t *added);

/*
 * Cancels a job. A queued or failed one is cancelled now and its partial files deleted; the active
 * one stops within about a second, and skiff_jobs_run_one() cancels it then (or fails it with the
 * Memory Stick's error when its partial files cannot be deleted). A finished job is left as it is.
 * The cancel is saved before the files are deleted; files that cannot be deleted fail the job with
 * the Memory Stick's error (a retry resumes from them). SKIFF_ERR_INVALID_ARG for a NULL queue,
 * SKIFF_ERR_STORAGE_NOT_FOUND for an unknown id, otherwise the storage's error: from saving the
 * queue (the job is then unchanged) or from deleting the files.
 */
skiff_err skiff_jobs_cancel(skiff_jobs *jobs, uint32_t id);

/* Queues a failed or cancelled job again (its attempts and error reset). SKIFF_ERR_INVALID_ARG for
 * a NULL queue or a job in another state, SKIFF_ERR_STORAGE_NOT_FOUND for an unknown id, the
 * storage's error when the queue cannot be saved (the job is then unchanged). */
skiff_err skiff_jobs_retry(skiff_jobs *jobs, uint32_t id);

/* Drops done and cancelled jobs from the queue and saves it; on a failed save they stay. */
skiff_err skiff_jobs_clear_finished(skiff_jobs *jobs);

/* Copies up to capacity jobs, in queue order, into out; returns how many the queue holds. */
size_t skiff_jobs_list(skiff_jobs *jobs, skiff_job *out, size_t capacity);

/* Takes the oldest event into *out: 1, or 0 when there is none. Discrete events come before the
 * latest progress. */
int skiff_jobs_next_event(skiff_jobs *jobs, skiff_jobs_event *out);

/* Asks the runner to stop as soon as it can (Skiff is quitting): the active job stays queued, with
 * its progress saved, and no other job starts on this queue (the request is never cleared). */
void skiff_jobs_request_stop(skiff_jobs *jobs);

/*
 * Runs the first queued job to its end: done, failed or cancelled, or still queued after
 * skiff_jobs_request_stop(); once a stop is requested no job starts (*ran is 0). The runner's state
 * changes stand for the session even when the queue file cannot be saved (the log says so, and
 * every later save writes the whole queue). *ran is 0 when nothing is queued. Returns
 * SKIFF_ERR_INVALID_ARG for a NULL argument or an env without its hooks, SKIFF_OK otherwise: how
 * the job ended is in its state and the events.
 */
skiff_err skiff_jobs_run_one(skiff_jobs *jobs, const skiff_jobs_env *env, int *ran);

/* ---- The queue file, exposed for the tests and the self-test ---- */

/*
 * Writes jobs (count of them, next_id the id the next one gets) as the queue file's JSON into out.
 * SKIFF_ERR_BUFFER_TOO_SMALL when it does not fit, SKIFF_ERR_NO_MEMORY, SKIFF_ERR_INVALID_ARG for
 * a NULL argument or more than SKIFF_JOBS_MAX jobs.
 */
skiff_err skiff_jobs_format(const skiff_job *jobs, size_t count, uint32_t next_id, char *out,
                            size_t out_size, size_t *length);

/*
 * Reads a queue file into out (SKIFF_JOBS_MAX entries) and its count, next_id and how many jobs
 * were dropped as unusable. SKIFF_ERR_CONFIG_PARSE for a file that is not a queue of this version,
 * SKIFF_ERR_INVALID_ARG for a NULL argument.
 */
skiff_err skiff_jobs_parse(const char *json, size_t length, skiff_job *out, size_t *count,
                           uint32_t *next_id, size_t *dropped);

const char *skiff_job_state_name(skiff_job_state state);

#endif
