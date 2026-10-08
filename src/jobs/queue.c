#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jobs_internal.h"

#define KEY_VERSION "version"
#define KEY_NEXT_ID "next_id"
#define KEY_JOBS "jobs"
#define KEY_ID "id"
#define KEY_ROM_ID "rom_id"
#define KEY_TITLE "title"
#define KEY_FILE_NAME "file_name"
#define KEY_TARGET "target"
#define KEY_SIZE "size"
#define KEY_CRC32 "crc32"
#define KEY_STATE "state"
#define KEY_ERROR "error"
#define KEY_ATTEMPTS "attempts"
#define KEY_REPLACE "replace"
#define KEY_REPLACE_SIZE "replace_size"
#define CRC32_HEX_DIGITS 8
#define HEX_BASE 16
#define ASCII_DELETE 0x7F
/* cJSON prints a number with 15 significant digits when that reads back as nearly the same double,
 * so a larger id could come back changed: ids are kept below 10^15 (RomM's are far smaller). */
#define JSON_ID_LIMIT 999999999999999.0
#define JSON_UINT32_LIMIT 4294967295.0
#define JSON_ERROR_LIMIT 999.0

static const char *const STATE_NAMES[] = {"queued", "active", "done", "failed", "cancelled"};

enum { STATE_COUNT = sizeof STATE_NAMES / sizeof STATE_NAMES[0] };

const char *skiff_job_state_name(skiff_job_state state) {
    return (unsigned)state < STATE_COUNT ? STATE_NAMES[state] : "unknown";
}

/* ---- Fields ---- */

/* Shorter than size and without control characters: a name is shown, logged and used as a path. */
static int text_fits(const char *text, size_t size, int may_be_empty) {
    const size_t length = strlen(text);
    if (length >= size || (length == 0 && !may_be_empty)) {
        return 0;
    }
    for (size_t i = 0; i < length; i++) {
        const unsigned char byte = (unsigned char)text[i];
        if (byte < (unsigned char)' ' || byte == ASCII_DELETE) {
            return 0;
        }
    }
    return 1;
}

/* The target and its .part and .resume files fit the download engine's paths. */
static int target_fits(const char *target) {
    const size_t suffix_max = sizeof SKIFF_DOWNLOAD_STATE_SUFFIX > sizeof SKIFF_DOWNLOAD_PART_SUFFIX
                                  ? sizeof SKIFF_DOWNLOAD_STATE_SUFFIX - 1
                                  : sizeof SKIFF_DOWNLOAD_PART_SUFFIX - 1;
    return strlen(target) + suffix_max < SKIFF_DOWNLOAD_PATH_MAX;
}

static int read_number(const cJSON *object, const char *name, double limit, double *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) {
        return 0;
    }
    const double value = item->valuedouble;
    /* Below the limit the cast is exact, so a fraction shows as a difference. */
    if (!(value >= 0.0) || value > limit || (double)(uint64_t)value != value) {
        return 0;
    }
    *out = value;
    return 1;
}

static int read_text(const cJSON *object, const char *name, char *out, size_t size,
                     int may_be_empty) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        !text_fits(item->valuestring, size, may_be_empty)) {
        return 0;
    }
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    return 1;
}

/* Exactly eight hexadecimal digits, or absent. */
/* Whether the job may replace the file at its target (Skiff's own earlier copy); absent is no, as
 * in a queue written before the field existed. */
static int read_replace(const cJSON *object, skiff_job *job) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, KEY_REPLACE);
    job->replace_target = 0;
    job->replace_size = 0;
    if (item == NULL || cJSON_IsFalse(item)) {
        return 1;
    }
    double size = 0;
    if (!cJSON_IsTrue(item) ||
        !read_number(object, KEY_REPLACE_SIZE, (double)SKIFF_STORAGE_MAX_FILE_BYTES, &size)) {
        return 0;
    }
    job->replace_target = 1;
    job->replace_size = (uint64_t)size;
    return 1;
}

static int read_crc32(const cJSON *object, skiff_job *job) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, KEY_CRC32);
    if (item == NULL || cJSON_IsNull(item)) {
        job->has_crc32 = 0;
        return 1;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) != CRC32_HEX_DIGITS ||
        strspn(item->valuestring, "0123456789abcdefABCDEF") != CRC32_HEX_DIGITS) {
        return 0;
    }
    job->has_crc32 = 1;
    job->crc32 = (uint32_t)strtoul(item->valuestring, NULL, HEX_BASE);
    return 1;
}

static int read_state(const cJSON *object, skiff_job_state *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, KEY_STATE);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < STATE_COUNT; i++) {
        if (strcmp(item->valuestring, STATE_NAMES[i]) == 0) {
            *out = (skiff_job_state)i;
            return 1;
        }
    }
    return 0;
}

static int parse_job(const cJSON *object, skiff_job *job) {
    double id = 0;
    double rom_id = 0;
    double size = 0;
    double error = 0;
    double attempts = 0;
    memset(job, 0, sizeof *job);
    if (!cJSON_IsObject(object) || !read_number(object, KEY_ID, JSON_UINT32_LIMIT, &id) ||
        id == 0 || !read_number(object, KEY_ROM_ID, JSON_ID_LIMIT, &rom_id) ||
        !read_text(object, KEY_TITLE, job->title, sizeof job->title, 1) ||
        !read_text(object, KEY_FILE_NAME, job->file_name, sizeof job->file_name, 0) ||
        !read_text(object, KEY_TARGET, job->target, sizeof job->target, 0) ||
        !target_fits(job->target) ||
        !read_number(object, KEY_SIZE, (double)SKIFF_STORAGE_MAX_FILE_BYTES, &size) || size == 0 ||
        !read_crc32(object, job) || !read_replace(object, job) ||
        !read_state(object, &job->state) ||
        !read_number(object, KEY_ERROR, JSON_ERROR_LIMIT, &error) ||
        !read_number(object, KEY_ATTEMPTS, JSON_UINT32_LIMIT, &attempts)) {
        return 0;
    }
    job->id = (uint32_t)id;
    job->rom_id = (uint64_t)rom_id;
    job->size = (uint64_t)size;
    job->error = (skiff_err)(int)error;
    job->attempts = (uint32_t)attempts;
    return 1;
}

static int finished(skiff_job_state state) {
    return state == SKIFF_JOB_DONE || state == SKIFF_JOB_CANCELLED;
}

/* Drops the oldest finished job; 0 when there is none. */
static int drop_oldest_finished(jobs_table *table) {
    for (size_t i = 0; i < table->count; i++) {
        if (finished(table->jobs[i].state)) {
            memmove(&table->jobs[i], &table->jobs[i + 1],
                    (table->count - i - 1) * sizeof table->jobs[0]);
            table->count--;
            return 1;
        }
    }
    return 0;
}

static int request_valid(const skiff_job_request *request) {
    return request->file_name != NULL && request->target != NULL && request->size > 0 &&
           (double)request->rom_id <= JSON_ID_LIMIT &&
           request->size <= SKIFF_STORAGE_MAX_FILE_BYTES &&
           text_fits(request->title != NULL ? request->title : "", SKIFF_JOBS_TITLE_MAX, 1) &&
           text_fits(request->file_name, SKIFF_JOBS_FILE_NAME_MAX, 0) &&
           text_fits(request->target, SKIFF_JOBS_TARGET_MAX, 0) && target_fits(request->target);
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static int equals_ignoring_case(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (ascii_lower(*a) != ascii_lower(*b)) {
            return 0;
        }
    }
    return *a == *b;
}

/* The paths a download owns besides its target. */
static const char *const OWNED_SUFFIXES[] = {"", SKIFF_DOWNLOAD_PART_SUFFIX,
                                             SKIFF_DOWNLOAD_STATE_SUFFIX};

enum { OWNED_SUFFIX_COUNT = sizeof OWNED_SUFFIXES / sizeof OWNED_SUFFIXES[0] };

/* Whether two downloads would touch a common file: a target, a .part or a .resume file of one is a
 * target, .part or .resume file of the other (FAT ignores case). */
static int paths_collide(const char *a, const char *b) {
    char a_path[SKIFF_DOWNLOAD_PATH_MAX];
    char b_path[SKIFF_DOWNLOAD_PATH_MAX];
    for (int i = 0; i < OWNED_SUFFIX_COUNT; i++) {
        snprintf(a_path, sizeof a_path, "%s%s", a, OWNED_SUFFIXES[i]);
        for (int j = 0; j < OWNED_SUFFIX_COUNT; j++) {
            snprintf(b_path, sizeof b_path, "%s%s", b, OWNED_SUFFIXES[j]);
            if (equals_ignoring_case(a_path, b_path)) {
                return 1;
            }
        }
    }
    return 0;
}

/* An unfinished job whose files an earlier unfinished one already owns (as skiff_jobs_add()
 * refuses): a damaged or hand-edited file must not set two downloads on one .part file. */
static int files_owned(const skiff_job *jobs, size_t count, const skiff_job *job) {
    if (finished(job->state)) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (!finished(jobs[i].state) && paths_collide(jobs[i].target, job->target)) {
            return 1;
        }
    }
    return 0;
}

static int id_taken(const skiff_job *jobs, size_t count, uint32_t id) {
    for (size_t i = 0; i < count; i++) {
        if (jobs[i].id == id) {
            return 1;
        }
    }
    return 0;
}

skiff_err skiff_jobs_parse(const char *json, size_t length, skiff_job *out, size_t *count,
                           uint32_t *next_id, size_t *dropped) {
    if (json == NULL || out == NULL || count == NULL || next_id == NULL || dropped == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *count = 0;
    *next_id = 1;
    *dropped = 0;
    if (memchr(json, '\0', length) != NULL) {
        return SKIFF_ERR_CONFIG_PARSE;
    }
    cJSON *root = cJSON_ParseWithLength(json, length);
    double version = 0;
    double next = 0;
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, KEY_JOBS);
    if (root == NULL || !read_number(root, KEY_VERSION, JSON_UINT32_LIMIT, &version) ||
        version != SKIFF_JOBS_VERSION ||
        !read_number(root, KEY_NEXT_ID, JSON_UINT32_LIMIT, &next) || !cJSON_IsArray(list)) {
        cJSON_Delete(root);
        return SKIFF_ERR_CONFIG_PARSE;
    }
    uint32_t largest = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, list) {
        skiff_job job;
        const int parsed = parse_job(item, &job);
        /* Dropped jobs' ids are not given out again either. */
        largest = parsed && job.id > largest ? job.id : largest;
        if (*count == SKIFF_JOBS_MAX || !parsed || id_taken(out, *count, job.id) ||
            files_owned(out, *count, &job)) {
            (*dropped)++;
            continue;
        }
        out[(*count)++] = job;
    }
    cJSON_Delete(root);
    /* An id is never reused, even if the file's counter fell behind its jobs. */
    *next_id = (uint32_t)next > largest ? (uint32_t)next : largest + 1;
    return SKIFF_OK;
}

static cJSON *job_object(const skiff_job *job) {
    cJSON *object = cJSON_CreateObject();
    char crc32[CRC32_HEX_DIGITS + 1];
    snprintf(crc32, sizeof crc32, "%08lx", (unsigned long)job->crc32);
    if (object == NULL || cJSON_AddNumberToObject(object, KEY_ID, (double)job->id) == NULL ||
        cJSON_AddNumberToObject(object, KEY_ROM_ID, (double)job->rom_id) == NULL ||
        cJSON_AddStringToObject(object, KEY_TITLE, job->title) == NULL ||
        cJSON_AddStringToObject(object, KEY_FILE_NAME, job->file_name) == NULL ||
        cJSON_AddStringToObject(object, KEY_TARGET, job->target) == NULL ||
        cJSON_AddNumberToObject(object, KEY_SIZE, (double)job->size) == NULL ||
        (job->has_crc32 ? cJSON_AddStringToObject(object, KEY_CRC32, crc32)
                        : cJSON_AddNullToObject(object, KEY_CRC32)) == NULL ||
        cJSON_AddStringToObject(object, KEY_STATE, skiff_job_state_name(job->state)) == NULL ||
        cJSON_AddNumberToObject(object, KEY_ERROR, (double)job->error) == NULL ||
        cJSON_AddNumberToObject(object, KEY_ATTEMPTS, (double)job->attempts) == NULL ||
        cJSON_AddBoolToObject(object, KEY_REPLACE, job->replace_target != 0) == NULL ||
        (job->replace_target &&
         cJSON_AddNumberToObject(object, KEY_REPLACE_SIZE, (double)job->replace_size) == NULL)) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

skiff_err skiff_jobs_format(const skiff_job *jobs, size_t count, uint32_t next_id, char *out,
                            size_t out_size, size_t *length) {
    if (jobs == NULL || out == NULL || length == NULL || out_size == 0 || count > SKIFF_JOBS_MAX) {
        return SKIFF_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();
    int built = root != NULL && list != NULL &&
                cJSON_AddNumberToObject(root, KEY_VERSION, SKIFF_JOBS_VERSION) != NULL &&
                cJSON_AddNumberToObject(root, KEY_NEXT_ID, (double)next_id) != NULL &&
                cJSON_AddItemToObject(root, KEY_JOBS, list);
    if (!built) {
        cJSON_Delete(list);
    }
    for (size_t i = 0; built && i < count; i++) {
        cJSON *object = job_object(&jobs[i]);
        built = object != NULL && cJSON_AddItemToArray(list, object);
        if (!built) {
            cJSON_Delete(object);
        }
    }
    char *text = built ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (text == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    const size_t text_length = strlen(text);
    const skiff_err err = text_length < out_size ? SKIFF_OK : SKIFF_ERR_BUFFER_TOO_SMALL;
    if (err == SKIFF_OK) {
        memcpy(out, text, text_length + 1);
        *length = text_length;
    }
    cJSON_free(text);
    return err;
}

/* ---- The queue object ---- */

void jobs_lock(skiff_jobs *jobs) {
    if (jobs->lock != NULL) {
        jobs->lock(jobs->lock_ctx);
    }
}

void jobs_unlock(skiff_jobs *jobs) {
    if (jobs->unlock != NULL) {
        jobs->unlock(jobs->lock_ctx);
    }
}

void jobs_commit_lock(skiff_jobs *jobs) {
    if (jobs->lock != NULL) {
        jobs->lock(jobs->save_lock_ctx);
    }
}

void jobs_commit_unlock(skiff_jobs *jobs) {
    if (jobs->unlock != NULL) {
        jobs->unlock(jobs->save_lock_ctx);
    }
}

skiff_job *jobs_table_find(jobs_table *table, uint32_t id) {
    for (size_t i = 0; i < table->count; i++) {
        if (table->jobs[i].id == id) {
            return &table->jobs[i];
        }
    }
    return NULL;
}

jobs_table *jobs_stage(skiff_jobs *jobs) {
    jobs->staged = jobs->table;
    return &jobs->staged;
}

skiff_err jobs_save_staged(skiff_jobs *jobs) {
    char *text = malloc(SKIFF_JOBS_FILE_MAX);
    if (text == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    const jobs_table *staged = &jobs->staged;
    size_t length = 0;
    skiff_err err = skiff_jobs_format(staged->jobs, staged->count, staged->next_id, text,
                                      SKIFF_JOBS_FILE_MAX, &length);
    if (err == SKIFF_OK) {
        err = skiff_storage_replace_whole(jobs->storage, jobs->path, text, length);
    }
    free(text);
    if (err != SKIFF_OK) {
        skiff_log_write(jobs->log, SKIFF_LOG_ERROR, JOBS_LOG_TAG,
                        "could not save the queue: %s (%d)", skiff_err_name(err), (int)err);
    }
    return err;
}

void jobs_publish(skiff_jobs *jobs) { jobs->table = jobs->staged; }

void jobs_push_event(skiff_jobs *jobs, const skiff_jobs_event *event) {
    if (event->kind == SKIFF_JOBS_EVENT_STATE && jobs->has_progress &&
        jobs->progress.job_id == event->job_id) {
        jobs->has_progress = 0;
    }
    if (jobs->event_count == SKIFF_JOBS_EVENTS_MAX) {
        /* The UI fell behind: the oldest goes, since newer events supersede it. */
        jobs->event_head = (jobs->event_head + 1) % SKIFF_JOBS_EVENTS_MAX;
        jobs->event_count--;
    }
    jobs->events[(jobs->event_head + jobs->event_count) % SKIFF_JOBS_EVENTS_MAX] = *event;
    jobs->event_count++;
}

void jobs_set_progress(skiff_jobs *jobs, const skiff_jobs_event *event) {
    jobs->progress = *event;
    jobs->has_progress = 1;
}

void jobs_push_state_event(skiff_jobs *jobs, const skiff_job *job) {
    if (job == NULL) {
        return;
    }
    skiff_jobs_event event;
    memset(&event, 0, sizeof event);
    event.kind = SKIFF_JOBS_EVENT_STATE;
    event.job_id = job->id;
    event.state = job->state;
    event.error = job->error;
    jobs_push_event(jobs, &event);
}

skiff_err jobs_commit_state(skiff_jobs *jobs, uint32_t id, skiff_job_state state, skiff_err error,
                            uint32_t attempts, jobs_commit_mode mode) {
    skiff_job *job = jobs_table_find(jobs_stage(jobs), id);
    if (job == NULL) {
        return SKIFF_ERR_STORAGE_NOT_FOUND;
    }
    job->state = state;
    job->error = error;
    job->attempts = attempts;
    const skiff_err err = jobs_save_staged(jobs);
    if (err == SKIFF_OK || mode == JOBS_COMMIT_ALWAYS) {
        jobs_lock(jobs);
        jobs_publish(jobs);
        jobs_push_state_event(jobs, jobs_table_find(&jobs->table, id));
        jobs_unlock(jobs);
    }
    return err;
}

/* Reads the queue file; a file that is not a queue is set aside, not fatal. */
static skiff_err load(skiff_jobs *jobs) {
    char *text = malloc(SKIFF_JOBS_FILE_MAX);
    if (text == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    jobs_table *table = &jobs->table;
    size_t length = 0;
    skiff_err err =
        skiff_storage_read_whole(jobs->storage, jobs->path, text, SKIFF_JOBS_FILE_MAX, &length);
    size_t dropped = 0;
    if (err == SKIFF_OK && length > 0) {
        err = skiff_jobs_parse(text, length, table->jobs, &table->count, &table->next_id, &dropped);
    }
    free(text);
    if (err == SKIFF_ERR_CONFIG_PARSE || err == SKIFF_ERR_BUFFER_TOO_SMALL) {
        skiff_log_write(jobs->log, SKIFF_LOG_WARN, JOBS_LOG_TAG,
                        "the queue file is not usable (%s); starting with an empty queue",
                        skiff_err_name(err));
        table->count = 0;
        table->next_id = 1;
        return SKIFF_OK;
    }
    if (dropped > 0) {
        skiff_log_write(jobs->log, SKIFF_LOG_WARN, JOBS_LOG_TAG,
                        "dropped %zu unusable job(s) from the queue file", dropped);
    }
    return err;
}

/* Both hooks with two distinct lock contexts, or no hooks. */
static int locks_valid(const skiff_jobs_config *config) {
    if (config->lock == NULL && config->unlock == NULL) {
        return 1;
    }
    return config->lock != NULL && config->unlock != NULL && config->save_lock_ctx != NULL &&
           config->save_lock_ctx != config->lock_ctx;
}

skiff_err skiff_jobs_create(const skiff_jobs_config *config, skiff_jobs **out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (config == NULL || config->storage == NULL || config->path == NULL ||
        config->path[0] == '\0' || strlen(config->path) >= SKIFF_STORAGE_PATH_MAX ||
        !locks_valid(config)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_jobs *jobs = calloc(1, sizeof *jobs);
    if (jobs == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    jobs->storage = config->storage;
    memcpy(jobs->path, config->path, strlen(config->path) + 1);
    jobs->log = config->log;
    jobs->lock = config->lock;
    jobs->unlock = config->unlock;
    jobs->lock_ctx = config->lock_ctx;
    jobs->save_lock_ctx = config->save_lock_ctx;
    jobs->table.next_id = 1;
    const skiff_err err = load(jobs);
    if (err != SKIFF_OK) {
        free(jobs);
        return err;
    }
    /* Left active by a quit or a crash: its .part file lets it resume. */
    for (size_t i = 0; i < jobs->table.count; i++) {
        if (jobs->table.jobs[i].state == SKIFF_JOB_ACTIVE) {
            jobs->table.jobs[i].state = SKIFF_JOB_QUEUED;
        }
    }
    *out = jobs;
    return SKIFF_OK;
}

void skiff_jobs_destroy(skiff_jobs *jobs) { free(jobs); }

/* Another job not yet finished owns one of target's files: two jobs sharing a .part file would
 * overwrite or delete each other's progress. The job for the same file is not another. */
static int target_taken(const jobs_table *table, uint64_t rom_id, const char *file_name,
                        const char *target) {
    for (size_t i = 0; i < table->count; i++) {
        const skiff_job *job = &table->jobs[i];
        const int same_file = job->rom_id == rom_id && strcmp(job->file_name, file_name) == 0;
        if (!same_file && !finished(job->state) && paths_collide(job->target, target)) {
            return 1;
        }
    }
    return 0;
}

static skiff_job *find_same_file(jobs_table *table, const skiff_job_request *request) {
    for (size_t i = 0; i < table->count; i++) {
        skiff_job *job = &table->jobs[i];
        if (job->rom_id == request->rom_id && strcmp(job->file_name, request->file_name) == 0) {
            return job;
        }
    }
    return NULL;
}

static void fill_job(skiff_job *job, uint32_t id, const skiff_job_request *request) {
    memset(job, 0, sizeof *job);
    job->id = id;
    job->rom_id = request->rom_id;
    if (request->title != NULL) {
        memcpy(job->title, request->title, strlen(request->title) + 1);
    }
    memcpy(job->file_name, request->file_name, strlen(request->file_name) + 1);
    memcpy(job->target, request->target, strlen(request->target) + 1);
    job->size = request->size;
    job->has_crc32 = request->has_crc32;
    job->crc32 = request->crc32;
    job->replace_target = request->replace_target != 0;
    job->replace_size = job->replace_target ? request->replace_size : 0;
    job->state = SKIFF_JOB_QUEUED;
}

/* Commit lock held: puts the request in staged; its id goes to *id. */
static skiff_err stage_request(skiff_jobs *jobs, const skiff_job_request *request,
                               const skiff_job *same, uint32_t *id) {
    jobs_table *staged = jobs_stage(jobs);
    skiff_job *job = same != NULL ? jobs_table_find(staged, same->id) : NULL;
    if (job != NULL) {
        fill_job(job, job->id, request);
    } else if (staged->count == SKIFF_JOBS_MAX && !drop_oldest_finished(staged)) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    } else {
        job = &staged->jobs[staged->count++];
        fill_job(job, staged->next_id++, request);
    }
    *id = job->id;
    return SKIFF_OK;
}

skiff_err skiff_jobs_add(skiff_jobs *jobs, const skiff_job_request *request, uint32_t *id) {
    if (jobs == NULL || request == NULL || id == NULL || !request_valid(request)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    jobs_commit_lock(jobs);
    if (target_taken(&jobs->table, request->rom_id, request->file_name, request->target)) {
        jobs_commit_unlock(jobs);
        return SKIFF_ERR_INVALID_ARG;
    }
    const skiff_job *same = find_same_file(&jobs->table, request);
    if (same != NULL && (same->state == SKIFF_JOB_QUEUED || same->state == SKIFF_JOB_ACTIVE)) {
        *id = same->id;
        jobs_commit_unlock(jobs);
        return SKIFF_OK;
    }
    /* Staged only: the queue keeps it only once the file holds it. */
    uint32_t added = 0;
    skiff_err err = stage_request(jobs, request, same, &added);
    if (err == SKIFF_OK) {
        err = jobs_save_staged(jobs);
    }
    if (err == SKIFF_OK) {
        jobs_lock(jobs);
        jobs_publish(jobs);
        const skiff_job *job = jobs_table_find(&jobs->table, added);
        jobs_push_state_event(jobs, job);
        jobs_unlock(jobs);
        *id = added;
        skiff_log_write(jobs->log, SKIFF_LOG_INFO, JOBS_LOG_TAG,
                        "queued job %u: rom %llu, %llu bytes", (unsigned)added,
                        (unsigned long long)request->rom_id, (unsigned long long)request->size);
    }
    jobs_commit_unlock(jobs);
    return err;
}

/* The active job is cancelled by a flag the runner's stop hook reads, under the state lock alone:
 * 1 when id is that job. */
static int flag_active_cancel(skiff_jobs *jobs, uint32_t id) {
    jobs_lock(jobs);
    const skiff_job *job = jobs_table_find(&jobs->table, id);
    const int active = job != NULL && job->state == SKIFF_JOB_ACTIVE && jobs->active_id == id;
    if (active) {
        /* The runner's stop hook sees it within about a second. */
        jobs->cancel_active = 1;
    }
    jobs_unlock(jobs);
    return active;
}

skiff_err skiff_jobs_cancel(skiff_jobs *jobs, uint32_t id) {
    if (jobs == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (flag_active_cancel(jobs, id)) {
        return SKIFF_OK;
    }
    jobs_commit_lock(jobs);
    /* Again under the commit lock: the runner may have taken the job meanwhile. */
    if (flag_active_cancel(jobs, id)) {
        jobs_commit_unlock(jobs);
        return SKIFF_OK;
    }
    const skiff_job *job = jobs_table_find(&jobs->table, id);
    skiff_err err = job == NULL ? SKIFF_ERR_STORAGE_NOT_FOUND : SKIFF_OK;
    if (job != NULL && (job->state == SKIFF_JOB_QUEUED || job->state == SKIFF_JOB_FAILED)) {
        /* The cancel is saved before the partial files go, so a refused save leaves the job as it
         * was with its progress. Files that then cannot be deleted fail the job with the Memory
         * Stick's error instead: a retry resumes from them, a second cancel tries again. The commit
         * lock is held until they are gone, so no other job can claim the target meanwhile. */
        const uint32_t attempts = job->attempts;
        char target[SKIFF_JOBS_TARGET_MAX];
        memcpy(target, job->target, strlen(job->target) + 1);
        err = jobs_commit_state(jobs, id, SKIFF_JOB_CANCELLED, SKIFF_ERR_CANCELLED, attempts,
                                JOBS_COMMIT_IF_SAVED);
        if (err == SKIFF_OK) {
            err = skiff_download_discard(jobs->storage, target);
            if (err != SKIFF_OK) {
                (void)jobs_commit_state(jobs, id, SKIFF_JOB_FAILED, err, attempts,
                                        JOBS_COMMIT_ALWAYS);
            }
        }
        skiff_log_write(jobs->log, SKIFF_LOG_INFO, JOBS_LOG_TAG, "cancel job %u: %s (%d)",
                        (unsigned)id, skiff_err_name(err), (int)err);
    }
    jobs_commit_unlock(jobs);
    return err;
}

skiff_err skiff_jobs_retry(skiff_jobs *jobs, uint32_t id) {
    if (jobs == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    jobs_commit_lock(jobs);
    const skiff_job *job = jobs_table_find(&jobs->table, id);
    skiff_err err = SKIFF_OK;
    if (job == NULL) {
        err = SKIFF_ERR_STORAGE_NOT_FOUND;
    } else if ((job->state != SKIFF_JOB_FAILED && job->state != SKIFF_JOB_CANCELLED) ||
               target_taken(&jobs->table, job->rom_id, job->file_name, job->target)) {
        err = SKIFF_ERR_INVALID_ARG;
    } else {
        err = jobs_commit_state(jobs, id, SKIFF_JOB_QUEUED, SKIFF_OK, 0, JOBS_COMMIT_IF_SAVED);
    }
    jobs_commit_unlock(jobs);
    return err;
}

skiff_err skiff_jobs_clear_finished(skiff_jobs *jobs) {
    if (jobs == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    jobs_commit_lock(jobs);
    jobs_table *staged = jobs_stage(jobs);
    size_t kept = 0;
    for (size_t i = 0; i < staged->count; i++) {
        if (!finished(staged->jobs[i].state)) {
            staged->jobs[kept++] = staged->jobs[i];
        }
    }
    staged->count = kept;
    const skiff_err err = jobs_save_staged(jobs);
    if (err == SKIFF_OK) {
        jobs_lock(jobs);
        jobs_publish(jobs);
        jobs_unlock(jobs);
    }
    jobs_commit_unlock(jobs);
    return err;
}

size_t skiff_jobs_list(skiff_jobs *jobs, skiff_job *out, size_t capacity) {
    if (jobs == NULL) {
        return 0;
    }
    jobs_lock(jobs);
    const size_t count = jobs->table.count;
    for (size_t i = 0; out != NULL && i < count && i < capacity; i++) {
        out[i] = jobs->table.jobs[i];
    }
    jobs_unlock(jobs);
    return count;
}

int skiff_jobs_next_event(skiff_jobs *jobs, skiff_jobs_event *out) {
    if (jobs == NULL || out == NULL) {
        return 0;
    }
    jobs_lock(jobs);
    int got = 1;
    if (jobs->event_count > 0) {
        *out = jobs->events[jobs->event_head];
        jobs->event_head = (jobs->event_head + 1) % SKIFF_JOBS_EVENTS_MAX;
        jobs->event_count--;
    } else if (jobs->has_progress) {
        *out = jobs->progress;
        jobs->has_progress = 0;
    } else {
        got = 0;
    }
    jobs_unlock(jobs);
    return got;
}

void skiff_jobs_request_stop(skiff_jobs *jobs) {
    if (jobs == NULL) {
        return;
    }
    jobs_lock(jobs);
    jobs->stop_requested = 1;
    jobs_unlock(jobs);
}
