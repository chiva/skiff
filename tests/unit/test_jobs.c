/*
 * The download queue and its runner (skiff/jobs.h) against the fake transport, a fake Memory Stick
 * over a temporary directory and a scripted platform: a clock that only moves when the runner
 * sleeps, a Wi-Fi switch and access point the test turns off and on, suspends, and cancels or a
 * quit arriving mid-transfer. Every branch of the retry policy, the queue file surviving restarts
 * and damage, and the events the UI sees.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "skiff/jobs.h"

#include "fake_storage.h"
#include "fake_transport.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define SERVER "https://romm.test"
#define TOKEN "rmm_test_token_0123456789"
#define ROM_ID 7U
#define FILE_NAME "Skiff Test.iso"
#define CONTENT_PATH "/api/roms/7/content/Skiff%20Test.iso"
#define ETAG "\"v1\""
#define MIB ((uint64_t)1024 * 1024)
/* Two full write buffers and a tail. */
#define BODY_BYTES (2 * MIB + 777)
#define HEADERS_MAX 256
#define SCRIPT_MAX 16
#define EVENTS_MAX 256
#define TEXT_MAX 4096

/* ---- The scripted platform ---- */

typedef struct attempt_script {
    skiff_err fail_before_response;
    skiff_err fail_mid_body;
    uint64_t fail_after_bytes;
} attempt_script;

/* Hands the runner the fake transport without letting it free the fake's routes. */
typedef struct proxy_transport {
    skiff_transport base;
    fake_transport *fake;
    int destroys;
} proxy_transport;

typedef struct fake_env {
    int64_t now;
    int switch_on;
    int online;
    uint32_t suspends;
    int opens;
    skiff_err open_error;
    int rejoins;
    skiff_err rejoin_result;
    int reloads;
    skiff_err reload_result;
    int keep_awakes;
    int sleeps;
    uint64_t slept_ms;
    /* Stop-hook checks so far, and what happens at a given one (0 for never). */
    int checks;
    int suspend_at;
    int switch_off_at;
    int cancel_at;
    int stop_at;
    /* At this check the test reads the events, as the UI would during the transfer. */
    int peek_at;
    uint32_t cancel_id;
    /* The PSP suspends during the write that runs out of the storage's write budget: the attempt
     * sees a Memory Stick error, and the suspend count changes only after it. */
    int suspend_on_storage_error;
    /* From the cancel on, the queue file's saves fail. */
    int refuse_saves_at_cancel;
    /* The cancel arrives after this many sleeps (0 for never). */
    int cancel_after_sleeps;
    /* The switch comes back after this many sleeps (0 for never). */
    int switch_back_after_sleeps;
    /* What each attempt meets, by the order of transport openings. */
    attempt_script script[SCRIPT_MAX];
} fake_env;

/* ---- Locks that record how they are used ---- */

typedef struct tracked_lock {
    int held;
    int takes;
    /* Taken while already held: a deadlock on a real mutex. */
    int nested;
} tracked_lock;

typedef struct lock_tracker {
    tracked_lock state;
    tracked_lock commit;
    /* The commit lock taken while the state lock was held (the wrong order). */
    int commit_under_state;
    /* While set, taking the commit lock is a fault: the call must not wait for a save. */
    int forbid_commit;
    int forbidden_commits;
    /* Memory Stick calls (opens and removes) made while the state lock was held. */
    int io_under_state;
    /* Opens of the queue file (saves), and those made without the commit lock. */
    int queue_opens;
    int queue_opens_without_commit;
    /* Runs once, just before the commit lock is taken for the commit_hook_at-th time. */
    void (*commit_hook)(void);
    int commit_hook_at;
} lock_tracker;

static lock_tracker locks;

static void tracked_lock_take(void *ctx) {
    tracked_lock *lock = ctx;
    if (lock == &locks.commit && locks.commit_hook != NULL &&
        locks.commit.takes + 1 == locks.commit_hook_at) {
        void (*hook)(void) = locks.commit_hook;
        locks.commit_hook = NULL;
        hook();
    }
    if (lock == &locks.commit) {
        locks.commit_under_state += locks.state.held;
        locks.forbidden_commits += locks.forbid_commit;
    }
    lock->nested += lock->held;
    lock->held = 1;
    lock->takes++;
}

static void tracked_lock_give(void *ctx) { ((tracked_lock *)ctx)->held = 0; }

static char dir[TEMP_DIR_PATH_MAX];
static char queue_path[TEMP_DIR_PATH_MAX];
static char log_path[TEMP_DIR_PATH_MAX];
static char target[TEMP_DIR_PATH_MAX];
static char part[TEMP_DIR_PATH_MAX + sizeof SKIFF_DOWNLOAD_PART_SUFFIX];
static char state_file[TEMP_DIR_PATH_MAX + sizeof SKIFF_DOWNLOAD_STATE_SUFFIX];
static skiff_storage *posix;
static fake_storage storage;
static fake_transport transport;
static proxy_transport proxy;
static fake_route *route;
static skiff_romm_client romm;
static skiff_jobs *jobs;
static skiff_log *logger;
static fake_env env_state;
static skiff_jobs_env env;
static void drain_events(void);
static unsigned char *body;
static uint32_t body_crc;
static skiff_jobs_event events[EVENTS_MAX];
static size_t event_count;

static skiff_err proxy_perform(skiff_transport *base, const skiff_http_request *request,
                               skiff_http_response *response) {
    proxy_transport *p = (proxy_transport *)base;
    return p->fake->base.ops->perform(&p->fake->base, request, response);
}

static void proxy_destroy(skiff_transport *base) { ((proxy_transport *)base)->destroys++; }

static const skiff_transport_ops PROXY_OPS = {proxy_perform, proxy_destroy};

static skiff_err env_open(void *ctx, skiff_transport **out) {
    fake_env *e = ctx;
    if (e->open_error != SKIFF_OK) {
        return e->open_error;
    }
    const attempt_script *step = &e->script[e->opens < SCRIPT_MAX ? e->opens : SCRIPT_MAX - 1];
    route->fail_before_response = step->fail_before_response;
    route->fail_mid_body = step->fail_mid_body;
    route->fail_after_bytes = step->fail_after_bytes;
    e->opens++;
    *out = &proxy.base;
    return SKIFF_OK;
}

static int env_switch_on(void *ctx) { return ((fake_env *)ctx)->switch_on; }

static int env_online(void *ctx) { return ((fake_env *)ctx)->online; }

static skiff_err env_rejoin(void *ctx) {
    fake_env *e = ctx;
    e->rejoins++;
    e->online = e->rejoin_result == SKIFF_OK && e->switch_on;
    return e->rejoin_result;
}

static skiff_err env_reload(void *ctx) {
    fake_env *e = ctx;
    e->reloads++;
    e->online = e->reload_result == SKIFF_OK && e->switch_on;
    return e->reload_result;
}

/* Asked once per attempt and at every stop-hook check: the test's events fire here. */
static uint32_t env_suspends(void *ctx) {
    fake_env *e = ctx;
    e->checks++;
    if (e->checks == e->suspend_at) {
        e->suspends++;
        e->online = 0;
    }
    if (e->checks == e->switch_off_at) {
        e->switch_on = 0;
        e->online = 0;
    }
    if (e->checks == e->cancel_at) {
        /* The active job's cancel is a flag: it never waits for a save. */
        locks.forbid_commit = 1;
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_cancel(jobs, e->cancel_id));
        locks.forbid_commit = 0;
        if (e->refuse_saves_at_cancel) {
            storage.sync_error = SKIFF_ERR_STORAGE_IO;
        }
    }
    if (e->checks == e->stop_at) {
        skiff_jobs_request_stop(jobs);
    }
    if (e->checks == e->peek_at) {
        drain_events();
    }
    if (e->suspend_on_storage_error && storage.write_error != SKIFF_OK &&
        storage.bytes_written >= storage.write_budget) {
        e->suspends++;
        e->online = 0;
        storage.write_error = SKIFF_OK;
    }
    return e->suspends;
}

static void env_keep_awake(void *ctx) { ((fake_env *)ctx)->keep_awakes++; }

/* Time moves a little with every look at the clock, and by the whole amount on a sleep. */
static int64_t env_now(void *ctx) {
    fake_env *e = ctx;
    e->now += 5;
    return e->now;
}

static void env_sleep(void *ctx, uint32_t ms) {
    fake_env *e = ctx;
    e->sleeps++;
    e->slept_ms += ms;
    e->now += ms;
    if (e->switch_back_after_sleeps != 0 && e->sleeps >= e->switch_back_after_sleeps) {
        e->switch_on = 1;
    }
    if (e->sleeps == e->cancel_after_sleeps) {
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_cancel(jobs, e->cancel_id));
    }
}

/* ---- Fixture ---- */

static void serve(void) {
    char headers[HEADERS_MAX];
    const int used = snprintf(headers, sizeof headers,
                              "HTTP/1.1 200 OK\r\nETag: %s\r\nContent-Length: %llu\r\n\r\n", ETAG,
                              (unsigned long long)BODY_BYTES);
    const size_t raw_size = (size_t)used + BODY_BYTES;
    char *raw = malloc(raw_size);
    TEST_ASSERT_NOT_NULL(raw);
    memcpy(raw, headers, (size_t)used);
    memcpy(raw + used, body, BODY_BYTES);
    route = fake_transport_add_raw(&transport, CONTENT_PATH, raw, raw_size);
    free(raw);
    TEST_ASSERT_NOT_NULL(route);
}

static void create_jobs(void) {
    const skiff_jobs_config config = {&storage.base, queue_path, logger, NULL, NULL, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_create(&config, &jobs));
}

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    TEST_ASSERT_TRUE(temp_dir_path(dir, SKIFF_JOBS_FILE_NAME, queue_path, sizeof queue_path));
    TEST_ASSERT_TRUE(temp_dir_path(dir, "skiff.log", log_path, sizeof log_path));
    /* The ISO folder does not exist yet: the runner creates it. */
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO/" FILE_NAME, target, sizeof target));
    snprintf(part, sizeof part, "%s" SKIFF_DOWNLOAD_PART_SUFFIX, target);
    snprintf(state_file, sizeof state_file, "%s" SKIFF_DOWNLOAD_STATE_SUFFIX, target);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&posix));
    fake_storage_init(&storage, posix);
    fake_transport_init(&transport);
    memset(&proxy, 0, sizeof proxy);
    proxy.base.ops = &PROXY_OPS;
    proxy.fake = &transport;
    body = malloc(BODY_BYTES);
    TEST_ASSERT_NOT_NULL(body);
    uint32_t seed = 0x5EED1234U;
    for (uint64_t i = 0; i < BODY_BYTES; i++) {
        seed = seed * 1103515245U + 12345U;
        body[i] = (unsigned char)(seed >> 16);
    }
    body_crc = (uint32_t)crc32(0L, body, (uInt)BODY_BYTES);
    serve();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_client_init(&romm, &proxy.base, SERVER, TOKEN));
    const skiff_log_config log_config = {&storage.base, log_path, 0,    0,    SKIFF_LOG_INFO,
                                         NULL,          NULL,     NULL, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_log_create(&log_config, &logger));
    memset(&env_state, 0, sizeof env_state);
    memset(&locks, 0, sizeof locks);
    env_state.switch_on = 1;
    env_state.online = 1;
    const skiff_jobs_env base_env = {env_open,   env_switch_on, env_online,     env_rejoin,
                                     env_reload, env_suspends,  env_keep_awake, env_now,
                                     env_sleep,  &env_state,    &romm};
    env = base_env;
    event_count = 0;
    create_jobs();
}

void tearDown(void) {
    skiff_jobs_destroy(jobs);
    jobs = NULL;
    skiff_log_destroy(logger);
    logger = NULL;
    skiff_romm_client_clear(&romm);
    skiff_transport_destroy(&transport.base);
    skiff_storage_destroy(&storage.base);
    skiff_storage_destroy(posix);
    free(body);
    temp_dir_remove(dir);
}

static skiff_job_request request_for(const char *file_name, const char *job_target) {
    skiff_job_request request;
    memset(&request, 0, sizeof request);
    request.rom_id = ROM_ID;
    request.title = "Skiff Test";
    request.file_name = file_name;
    request.target = job_target;
    request.size = BODY_BYTES;
    request.has_crc32 = 1;
    request.crc32 = body_crc;
    return request;
}

/* A request for another file of the ROM, downloading to its own path in the ISO folder. */
static skiff_job_request request_named(const char *file_name, char *path, size_t path_size) {
    char relative[TEMP_DIR_PATH_MAX];
    snprintf(relative, sizeof relative, "ISO/%s", file_name);
    TEST_ASSERT_TRUE(temp_dir_path(dir, relative, path, path_size));
    return request_for(file_name, path);
}

static uint32_t add_job(void) {
    const skiff_job_request request = request_for(FILE_NAME, target);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &request, &id));
    return id;
}

static skiff_job job_with(uint32_t id) {
    skiff_job list[SKIFF_JOBS_MAX];
    const size_t count = skiff_jobs_list(jobs, list, SKIFF_JOBS_MAX);
    for (size_t i = 0; i < count; i++) {
        if (list[i].id == id) {
            return list[i];
        }
    }
    TEST_FAIL_MESSAGE("no job with that id");
    return list[0];
}

static void drain_events(void) {
    while (event_count < EVENTS_MAX && skiff_jobs_next_event(jobs, &events[event_count])) {
        event_count++;
    }
}

static int count_events(skiff_jobs_event_kind kind, int step) {
    int count = 0;
    for (size_t i = 0; i < event_count; i++) {
        if (events[i].kind == kind && (step < 0 || (int)events[i].step == step)) {
            count++;
        }
    }
    return count;
}

static void run(void) {
    int ran = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_run_one(jobs, &env, &ran));
    TEST_ASSERT_EQUAL_INT(1, ran);
    drain_events();
}

static void report(uint32_t id) {
    const skiff_job job = job_with(id);
    TEST_PRINTF("job %u: %s, %s, %u attempt(s); opens %d, rejoins %d, reloads %d, slept %llu ms, "
                "%zu event(s)",
                (unsigned)job.id, skiff_job_state_name(job.state), skiff_err_name(job.error),
                (unsigned)job.attempts, env_state.opens, env_state.rejoins, env_state.reloads,
                (unsigned long long)env_state.slept_ms, event_count);
}

static int exists(const char *path) {
    uint64_t size = 0;
    return skiff_storage_size(posix, path, &size) == SKIFF_OK;
}

static void assert_complete(void) {
    uint64_t size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_size(posix, target, &size));
    TEST_ASSERT_EQUAL_UINT64(BODY_BYTES, size);
    TEST_ASSERT_FALSE(exists(part));
    TEST_ASSERT_FALSE(exists(state_file));
}

static void read_text(const char *path, char *out, size_t size) {
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_read_whole(posix, path, out, size - 1, &length));
    out[length] = '\0';
}

static void write_text(const char *path, const char *text) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_replace_whole(posix, path, text, strlen(text)));
}

/* Puts a file at the job's target, in the ISO folder the runner would create. */
static void place_at_target(const char *text) {
    char folder[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO", folder, sizeof folder));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, folder));
    write_text(target, text);
}

/* ---- The queue ---- */

static void test_jobs_are_saved_and_survive_a_restart(void) {
    const uint32_t first = add_job();
    char other_path[TEMP_DIR_PATH_MAX];
    const skiff_job_request other = request_named("Other.iso", other_path, sizeof other_path);
    uint32_t second = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &other, &second));
    TEST_ASSERT_EQUAL_UINT32(first + 1, second);
    skiff_jobs_destroy(jobs);
    create_jobs();
    skiff_job list[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(2, skiff_jobs_list(jobs, list, SKIFF_JOBS_MAX));
    TEST_ASSERT_EQUAL_STRING("Skiff Test", list[0].title);
    TEST_ASSERT_EQUAL_STRING(target, list[0].target);
    TEST_ASSERT_EQUAL_UINT64(BODY_BYTES, list[0].size);
    TEST_ASSERT_EQUAL_HEX32(body_crc, list[0].crc32);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, list[1].state);
    TEST_PRINTF("ids keep counting after a restart");
    char third_path[TEMP_DIR_PATH_MAX];
    const skiff_job_request third = request_named("Third.iso", third_path, sizeof third_path);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &third, &id));
    TEST_ASSERT_EQUAL_UINT32(second + 1, id);
}

static void test_the_same_file_is_not_queued_twice(void) {
    const uint32_t id = add_job();
    TEST_ASSERT_EQUAL_UINT32(id, add_job());
    TEST_ASSERT_EQUAL_size_t(1, skiff_jobs_list(jobs, NULL, 0));
    run();
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    TEST_PRINTF("a finished one is queued again, under its id");
    TEST_ASSERT_EQUAL_UINT32(id, add_job());
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, job_with(id).state);
    TEST_ASSERT_EQUAL_size_t(1, skiff_jobs_list(jobs, NULL, 0));
}

static void test_a_full_queue_makes_room_from_finished_jobs_only(void) {
    char name[32];
    uint32_t first = 0;
    for (int i = 0; i < SKIFF_JOBS_MAX; i++) {
        snprintf(name, sizeof name, "Game %02d.iso", i);
        char path[TEMP_DIR_PATH_MAX];
        const skiff_job_request request = request_named(name, path, sizeof path);
        uint32_t id = 0;
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &request, &id));
        first = i == 0 ? id : first;
    }
    char extra_path[TEMP_DIR_PATH_MAX];
    const skiff_job_request extra = request_named("Extra.iso", extra_path, sizeof extra_path);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL, skiff_jobs_add(jobs, &extra, &id));
    TEST_PRINTF("a cancelled job is finished, so it makes room");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_cancel(jobs, first));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &extra, &id));
    TEST_ASSERT_EQUAL_size_t(SKIFF_JOBS_MAX, skiff_jobs_list(jobs, NULL, 0));
    skiff_job list[1];
    skiff_jobs_list(jobs, list, 1);
    TEST_ASSERT_NOT_EQUAL_UINT32(first, list[0].id);
}

static void test_unusable_requests_are_refused(void) {
    uint32_t id = 0;
    skiff_job_request request = request_for("Bad\x01name.iso", target);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_add(jobs, &request, &id));
    request = request_for("", target);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_add(jobs, &request, &id));
    request = request_for(FILE_NAME, "");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_add(jobs, &request, &id));
    request = request_for(FILE_NAME, target);
    request.size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_add(jobs, &request, &id));
    request.size = SKIFF_STORAGE_MAX_FILE_BYTES + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_add(jobs, &request, &id));
    request.size = BODY_BYTES;
    request.rom_id = 1000000000000000ULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_add(jobs, &request, &id));
    request = request_for(FILE_NAME, target);
    request.title = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &request, &id));
    TEST_ASSERT_EQUAL_STRING("", job_with(id).title);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_add(NULL, &request, &id));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_add(jobs, NULL, &id));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_add(jobs, &request, NULL));
}

static void test_a_job_the_file_cannot_hold_is_not_added(void) {
    add_job();
    storage.fail_suffix = SKIFF_STORAGE_DRAFT_SUFFIX;
    storage.write_budget = storage.bytes_written + 3;
    storage.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    char other_path[TEMP_DIR_PATH_MAX];
    const skiff_job_request other = request_named("Other.iso", other_path, sizeof other_path);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, skiff_jobs_add(jobs, &other, &id));
    TEST_ASSERT_EQUAL_size_t(1, skiff_jobs_list(jobs, NULL, 0));
    storage.write_error = SKIFF_OK;
    TEST_PRINTF("and the id it would have had is still free");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &other, &id));
    TEST_ASSERT_EQUAL_UINT32(2, id);
}

static void test_a_damaged_queue_file_starts_an_empty_queue(void) {
    skiff_jobs_destroy(jobs);
    write_text(queue_path, "{\"version\":1,\"next_id\":3,\"jobs\":[{\"id\":");
    create_jobs();
    TEST_ASSERT_EQUAL_size_t(0, skiff_jobs_list(jobs, NULL, 0));
    skiff_jobs_destroy(jobs);
    write_text(queue_path, "{\"version\":2,\"next_id\":1,\"jobs\":[]}");
    create_jobs();
    TEST_ASSERT_EQUAL_size_t(0, skiff_jobs_list(jobs, NULL, 0));
    skiff_log_flush(logger);
    char log_text[TEXT_MAX];
    read_text(log_path, log_text, sizeof log_text);
    TEST_PRINTF("log:\n%s", log_text);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "the queue file is not usable"));
}

static void test_an_unusable_job_is_dropped_and_the_rest_kept(void) {
    skiff_jobs_destroy(jobs);
    write_text(queue_path,
               "{\"version\":1,\"next_id\":2,\"jobs\":["
               "{\"id\":5,\"rom_id\":7,\"title\":\"A\",\"file_name\":\"a.iso\",\"target\":"
               "\"ms0:/ISO/a.iso\",\"size\":10,\"crc32\":\"0000000a\",\"state\":\"active\","
               "\"error\":111,\"attempts\":2},"
               "{\"id\":6,\"rom_id\":7,\"title\":\"B\",\"file_name\":\"\",\"target\":\"x\","
               "\"size\":10,\"crc32\":null,\"state\":\"queued\",\"error\":0,\"attempts\":0},"
               "{\"id\":5,\"rom_id\":8,\"title\":\"dup\",\"file_name\":\"d.iso\",\"target\":\"x\","
               "\"size\":10,\"state\":\"queued\",\"error\":0,\"attempts\":0},"
               "{\"id\":7,\"rom_id\":9,\"title\":\"same\",\"file_name\":\"s.iso\",\"target\":"
               "\"MS0:/ISO/A.ISO.part\",\"size\":10,\"state\":\"queued\",\"error\":0,"
               "\"attempts\":0}]}");
    create_jobs();
    skiff_job list[SKIFF_JOBS_MAX];
    TEST_ASSERT_EQUAL_size_t(1, skiff_jobs_list(jobs, list, SKIFF_JOBS_MAX));
    TEST_PRINTF("left active by a quit: queued again; the error and attempts stay");
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, list[0].state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECTION_LOST, list[0].error);
    TEST_ASSERT_EQUAL_UINT32(2, list[0].attempts);
    TEST_PRINTF("the counter fell behind its jobs: ids are never reused");
    const skiff_job_request request = request_for(FILE_NAME, target);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &request, &id));
    TEST_ASSERT_EQUAL_UINT32(8, id);
}

static void test_the_queue_file_round_trips(void) {
    skiff_job written[2];
    memset(written, 0, sizeof written);
    written[0].id = 1;
    written[0].rom_id = 999999999999999ULL;
    snprintf(written[0].title, sizeof written[0].title, "Caf\xC3\xA9 \"quoted\" \\ game");
    snprintf(written[0].file_name, sizeof written[0].file_name, "%s", FILE_NAME);
    snprintf(written[0].target, sizeof written[0].target, "ms0:/ISO/%s", FILE_NAME);
    written[0].size = SKIFF_STORAGE_MAX_FILE_BYTES;
    written[0].has_crc32 = 1;
    written[0].crc32 = 0xCBF43926U;
    written[0].state = SKIFF_JOB_FAILED;
    written[0].error = SKIFF_ERR_ROMM_CHECKSUM;
    written[0].attempts = 6;
    written[0].replace_target = 1;
    written[0].replace_size = 123456789;
    written[1] = written[0];
    written[1].replace_target = 0;
    written[1].replace_size = 0;
    written[1].id = 4;
    written[1].has_crc32 = 0;
    written[1].crc32 = 0;
    written[1].state = SKIFF_JOB_CANCELLED;
    char text[TEXT_MAX];
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_format(written, 2, 5, text, sizeof text, &length));
    TEST_PRINTF("%s", text);
    skiff_job read[SKIFF_JOBS_MAX];
    size_t count = 0;
    uint32_t next_id = 0;
    size_t dropped = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_jobs_parse(text, length, read, &count, &next_id, &dropped));
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_EQUAL_size_t(0, dropped);
    TEST_ASSERT_EQUAL_UINT32(5, next_id);
    TEST_ASSERT_EQUAL_MEMORY(&written[0], &read[0], sizeof read[0]);
    TEST_ASSERT_EQUAL_MEMORY(&written[1], &read[1], sizeof read[1]);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_jobs_format(written, 2, 5, text, length, &length));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_PARSE,
                          skiff_jobs_parse("[]", 2, read, &count, &next_id, &dropped));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_format(written, SKIFF_JOBS_MAX + 1, 5,
                                                                   text, sizeof text, &length));
}

static void test_a_queue_file_without_the_replace_flag_never_replaces(void) {
    static const char OLDER[] =
        "{\"version\":1,\"next_id\":2,\"jobs\":[{\"id\":1,\"rom_id\":7,\"title\":\"\","
        "\"file_name\":\"Game.iso\",\"target\":\"ms0:/ISO/Game.iso\",\"size\":10,"
        "\"crc32\":null,\"state\":\"queued\",\"error\":0,\"attempts\":0}]}";
    skiff_job read[SKIFF_JOBS_MAX];
    size_t count = 0;
    uint32_t next_id = 0;
    size_t dropped = 0;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_jobs_parse(OLDER, sizeof OLDER - 1, read, &count, &next_id, &dropped));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_INT(0, read[0].replace_target);
    TEST_PRINTF("a mistyped flag drops the job, as any other mistyped field");
    static const char MISTYPED[] =
        "{\"version\":1,\"next_id\":2,\"jobs\":[{\"id\":1,\"rom_id\":7,\"title\":\"\","
        "\"file_name\":\"Game.iso\",\"target\":\"ms0:/ISO/Game.iso\",\"size\":10,"
        "\"crc32\":null,\"state\":\"queued\",\"error\":0,\"attempts\":0,\"replace\":1}]}";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_parse(MISTYPED, sizeof MISTYPED - 1, read, &count,
                                                     &next_id, &dropped));
    TEST_ASSERT_EQUAL_size_t(0, count);
    TEST_ASSERT_EQUAL_size_t(1, dropped);
    TEST_PRINTF("replacing needs the size of the copy it may replace");
    static const char NO_SIZE[] =
        "{\"version\":1,\"next_id\":2,\"jobs\":[{\"id\":1,\"rom_id\":7,\"title\":\"\","
        "\"file_name\":\"Game.iso\",\"target\":\"ms0:/ISO/Game.iso\",\"size\":10,"
        "\"crc32\":null,\"state\":\"queued\",\"error\":0,\"attempts\":0,\"replace\":true}]}";
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_jobs_parse(NO_SIZE, sizeof NO_SIZE - 1, read, &count, &next_id, &dropped));
    TEST_ASSERT_EQUAL_size_t(0, count);
}

static void test_retry_and_clear_finished(void) {
    const uint32_t id = add_job();
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_retry(jobs, id));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_jobs_retry(jobs, id + 99));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_jobs_cancel(jobs, id + 99));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_cancel(jobs, id));
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_CANCELLED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_retry(jobs, id));
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, job_with(id).state);
    run();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_clear_finished(jobs));
    TEST_ASSERT_EQUAL_size_t(0, skiff_jobs_list(jobs, NULL, 0));
}

static void test_events_coalesce_progress_and_drop_the_oldest(void) {
    add_job();
    run();
    TEST_PRINTF("a finished job's last progress is superseded by its state");
    TEST_ASSERT_EQUAL_INT(0, count_events(SKIFF_JOBS_EVENT_PROGRESS, -1));
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, events[event_count - 1].state);
    event_count = 0;
    enum { ADDED = SKIFF_JOBS_EVENTS_MAX + 4 };
    uint32_t ids[ADDED];
    char name[32];
    for (int i = 0; i < ADDED; i++) {
        snprintf(name, sizeof name, "More %02d.iso", i);
        char path[TEMP_DIR_PATH_MAX];
        const skiff_job_request request = request_named(name, path, sizeof path);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &request, &ids[i]));
    }
    drain_events();
    TEST_PRINTF("a UI that fell behind finds the newest %d events", SKIFF_JOBS_EVENTS_MAX);
    TEST_ASSERT_EQUAL_size_t(SKIFF_JOBS_EVENTS_MAX, event_count);
    TEST_ASSERT_EQUAL_UINT32(ids[ADDED - SKIFF_JOBS_EVENTS_MAX], events[0].job_id);
    TEST_ASSERT_EQUAL_UINT32(ids[ADDED - 1], events[event_count - 1].job_id);
    skiff_jobs_event none;
    TEST_ASSERT_EQUAL_INT(0, skiff_jobs_next_event(jobs, &none));
}

static void test_player_changes_the_file_cannot_hold_are_undone(void) {
    const uint32_t id = add_job();
    storage.fail_suffix = SKIFF_STORAGE_DRAFT_SUFFIX;
    storage.sync_error = SKIFF_ERR_STORAGE_IO;
    TEST_PRINTF("a cancel the queue file refuses leaves the job queued");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_jobs_cancel(jobs, id));
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, job_with(id).state);
    storage.sync_error = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_cancel(jobs, id));
    storage.sync_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_jobs_retry(jobs, id));
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_CANCELLED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_jobs_clear_finished(jobs));
    TEST_ASSERT_EQUAL_size_t(1, skiff_jobs_list(jobs, NULL, 0));
    storage.sync_error = SKIFF_OK;
    TEST_PRINTF("and the file still says what the queue shows");
    skiff_jobs_destroy(jobs);
    create_jobs();
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_CANCELLED, job_with(id).state);
}

static void test_a_cancel_whose_files_cannot_be_deleted_is_refused(void) {
    const uint32_t id = add_job();
    char folder[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO", folder, sizeof folder));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdir(posix, folder));
    write_text(part, "partial");
    storage.fail_suffix = SKIFF_DOWNLOAD_PART_SUFFIX;
    storage.remove_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_jobs_cancel(jobs, id));
    TEST_PRINTF("not runnable, and a retry would resume from what is left");
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_FAILED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, job_with(id).error);
}

static void test_a_refused_cancel_keeps_the_progress(void) {
    const uint32_t id = add_job();
    char folder[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO", folder, sizeof folder));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdir(posix, folder));
    write_text(part, "partial");
    storage.fail_suffix = SKIFF_STORAGE_DRAFT_SUFFIX;
    storage.sync_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_jobs_cancel(jobs, id));
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, job_with(id).state);
    TEST_ASSERT_TRUE(exists(part));
}

static void test_two_jobs_never_write_to_one_target(void) {
    const uint32_t id = add_job();
    char other_case[TEMP_DIR_PATH_MAX];
    snprintf(other_case, sizeof other_case, "%s", target);
    char *name = strrchr(other_case, '/') + 1;
    name[0] = (char)(name[0] == 'S' ? 's' : 'S');
    skiff_job_request other = request_for("Another file.iso", other_case);
    uint32_t other_id = 0;
    TEST_PRINTF("another ROM's file on the same path, even in another case, is refused");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_add(jobs, &other, &other_id));
    TEST_PRINTF("nor one whose target is the first job's .part or .resume file");
    char sidecar[TEMP_DIR_PATH_MAX + sizeof SKIFF_DOWNLOAD_STATE_SUFFIX];
    snprintf(sidecar, sizeof sidecar, "%s" SKIFF_DOWNLOAD_STATE_SUFFIX, target);
    const skiff_job_request on_sidecar = request_for("Sidecar.iso", sidecar);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_add(jobs, &on_sidecar, &other_id));
    run();
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    TEST_PRINTF("once the first job is done the path is the installer's to decide");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &other, &other_id));
}

static void test_a_target_too_long_for_its_partial_files_is_refused(void) {
    char long_target[SKIFF_DOWNLOAD_PATH_MAX];
    const size_t room = SKIFF_DOWNLOAD_PATH_MAX - sizeof SKIFF_DOWNLOAD_STATE_SUFFIX;
    memset(long_target, 'a', room + 1);
    long_target[room + 1] = '\0';
    long_target[0] = '/';
    skiff_job_request request = request_for(FILE_NAME, long_target);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_add(jobs, &request, &id));
    long_target[room] = '\0';
    request = request_for(FILE_NAME, long_target);
    TEST_PRINTF("%zu bytes leave room for \".resume\"", strlen(long_target));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &request, &id));
}

/* ---- Running ---- */

static void test_a_job_downloads_into_a_folder_it_creates(void) {
    const uint32_t id = add_job();
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, job_with(id).error);
    TEST_ASSERT_EQUAL_UINT32(1, job_with(id).attempts);
    assert_complete();
    TEST_ASSERT_EQUAL_INT(1, env_state.opens);
    TEST_ASSERT_EQUAL_INT(1, proxy.destroys);
    TEST_ASSERT_EQUAL_UINT64(0, env_state.slept_ms);
    TEST_PRINTF("the token goes in the request, the PSP is kept awake");
    TEST_ASSERT_NOT_NULL(strstr(transport.log[0].headers, "Authorization: Bearer " TOKEN));
    TEST_ASSERT_GREATER_THAN_INT(0, env_state.keep_awakes);
    TEST_PRINTF("the UI saw queued, active, done");
    TEST_ASSERT_EQUAL_INT(3, count_events(SKIFF_JOBS_EVENT_STATE, -1));
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, events[0].state);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_ACTIVE, events[1].state);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, events[event_count - 1].state);
    int ran = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_run_one(jobs, &env, &ran));
    TEST_ASSERT_EQUAL_INT(0, ran);
}

static void test_the_log_is_written_right_after_each_block(void) {
    add_job();
    run();
    TEST_PRINTF("info lines wait in memory; the engine's writes let them out while the job runs");
    char log_text[TEXT_MAX];
    read_text(log_path, log_text, sizeof log_text);
    TEST_PRINTF("log:\n%s", log_text);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "job 1: start"));
    TEST_ASSERT_NULL(strstr(log_text, TOKEN));
}

static void test_progress_reports_bytes_and_speed(void) {
    add_job();
    env_state.peek_at = 1500;
    run();
    int progress = 0;
    for (size_t i = 0; i < event_count; i++) {
        if (events[i].kind == SKIFF_JOBS_EVENT_PROGRESS) {
            progress++;
            TEST_PRINTF("progress %llu of %llu at %llu B/s", (unsigned long long)events[i].done,
                        (unsigned long long)events[i].total,
                        (unsigned long long)events[i].bytes_per_s);
            TEST_ASSERT_GREATER_THAN_UINT64(MIB, events[i].done);
            TEST_ASSERT_EQUAL_UINT64(BODY_BYTES, events[i].total);
            TEST_ASSERT_GREATER_THAN_UINT64(0, events[i].bytes_per_s);
        }
    }
    TEST_PRINTF("only the latest progress waited for the UI");
    TEST_ASSERT_EQUAL_INT(1, progress);
}

static void test_an_error_a_retry_cannot_fix_fails_the_job(void) {
    char missing_path[TEMP_DIR_PATH_MAX];
    skiff_job_request request = request_named("Missing.iso", missing_path, sizeof missing_path);
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &request, &id));
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_FAILED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_NOT_FOUND, job_with(id).error);
    TEST_ASSERT_EQUAL_UINT32(1, job_with(id).attempts);
    TEST_ASSERT_EQUAL_UINT64(0, env_state.slept_ms);
}

static void test_a_file_found_at_the_target_fails_the_job_and_keeps_it(void) {
    serve();
    place_at_target("mine");
    const uint32_t id = add_job();
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_FAILED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NAME_TAKEN, job_with(id).error);
    char kept[8];
    read_text(target, kept, sizeof kept);
    TEST_ASSERT_EQUAL_STRING("mine", kept);
    TEST_PRINTF("moved away by the player, a retry finishes from the kept download");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(posix, target));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_retry(jobs, id));
    run();
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    assert_complete();
}

static void test_skiffs_own_copy_is_replaced_when_the_job_says_so(void) {
    serve();
    static const char OLDER_COPY[] = "an older copy Skiff installed";
    place_at_target(OLDER_COPY);
    skiff_job_request request = request_for(FILE_NAME, target);
    request.replace_target = 1;
    request.replace_size = sizeof OLDER_COPY - 1;
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &request, &id));
    TEST_ASSERT_EQUAL_INT(1, job_with(id).replace_target);
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    assert_complete();
}

static void test_a_connection_lost_with_progress_is_retried_at_once(void) {
    const uint32_t id = add_job();
    env_state.script[0].fail_mid_body = SKIFF_ERR_NET_CONNECTION_LOST;
    env_state.script[0].fail_after_bytes = MIB + 100;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    TEST_ASSERT_EQUAL_UINT32(2, job_with(id).attempts);
    TEST_ASSERT_EQUAL_UINT64(0, env_state.slept_ms);
    TEST_ASSERT_EQUAL_INT(0, env_state.rejoins);
    TEST_PRINTF("the second attempt resumed with a range on a new connection");
    TEST_ASSERT_TRUE(transport.log[1].has_range);
    TEST_ASSERT_EQUAL_INT(2, proxy.destroys);
    assert_complete();
}

static void test_attempts_that_get_nothing_back_off_then_fail(void) {
    const uint32_t id = add_job();
    for (int i = 0; i < SCRIPT_MAX; i++) {
        env_state.script[i].fail_before_response = SKIFF_ERR_NET_CONNECT;
    }
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_FAILED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECT, job_with(id).error);
    TEST_ASSERT_EQUAL_UINT32(SKIFF_JOBS_IDLE_ATTEMPTS_MAX, job_with(id).attempts);
    TEST_PRINTF("waits of 1, 2, 4, 8 and 16 s between the six attempts");
    TEST_ASSERT_EQUAL_UINT64(31000, env_state.slept_ms);
    TEST_ASSERT_EQUAL_INT(5, count_events(SKIFF_JOBS_EVENT_RECOVERY, SKIFF_JOBS_RETRYING));
    uint32_t expected = SKIFF_JOBS_BACKOFF_FIRST_MS;
    for (size_t i = 0; i < event_count; i++) {
        if (events[i].kind == SKIFF_JOBS_EVENT_RECOVERY && events[i].step == SKIFF_JOBS_RETRYING) {
            TEST_ASSERT_EQUAL_UINT32(expected, events[i].retry_in_ms);
            expected *= 2;
        }
    }
}

static void test_bytes_received_reset_the_count_of_idle_attempts(void) {
    const uint32_t id = add_job();
    for (int i = 0; i < SCRIPT_MAX; i++) {
        env_state.script[i].fail_before_response = SKIFF_ERR_NET_TIMEOUT;
    }
    /* Five idle attempts, then one that gets a block, then five idle again, then success. */
    env_state.script[5].fail_before_response = SKIFF_OK;
    env_state.script[5].fail_mid_body = SKIFF_ERR_NET_TIMEOUT;
    env_state.script[5].fail_after_bytes = MIB + 1;
    env_state.script[11].fail_before_response = SKIFF_OK;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    TEST_ASSERT_EQUAL_UINT32(12, job_with(id).attempts);
    assert_complete();
}

static void test_with_the_switch_off_the_runner_waits_without_failing(void) {
    const uint32_t id = add_job();
    env_state.switch_off_at = 300;
    env_state.switch_back_after_sleeps = 400;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    TEST_ASSERT_EQUAL_UINT32(2, job_with(id).attempts);
    TEST_PRINTF("100 s with the switch off cost no failed attempt; then the profile is rejoined");
    TEST_ASSERT_EQUAL_UINT64(400 * SKIFF_JOBS_POLL_MS, env_state.slept_ms);
    TEST_ASSERT_EQUAL_INT(1, count_events(SKIFF_JOBS_EVENT_RECOVERY, SKIFF_JOBS_WAITING_FOR_WIFI));
    TEST_ASSERT_EQUAL_INT(1, env_state.rejoins);
    TEST_ASSERT_EQUAL_INT(0, env_state.reloads);
    assert_complete();
}

static void test_after_a_suspend_the_profile_is_rejoined(void) {
    const uint32_t id = add_job();
    env_state.suspend_at = 600;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(1, env_state.rejoins);
    TEST_ASSERT_EQUAL_INT(1, count_events(SKIFF_JOBS_EVENT_RECOVERY, SKIFF_JOBS_REJOINING));
    assert_complete();
}

static void test_a_memory_stick_error_across_a_suspend_costs_only_the_attempt(void) {
    const uint32_t id = add_job();
    storage.fail_suffix = SKIFF_DOWNLOAD_PART_SUFFIX;
    storage.write_budget = MIB + 10;
    storage.write_error = SKIFF_ERR_STORAGE_IO;
    env_state.suspend_on_storage_error = 1;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    TEST_ASSERT_EQUAL_UINT32(2, job_with(id).attempts);
    TEST_ASSERT_EQUAL_INT(1, env_state.rejoins);
    assert_complete();
}

static void test_a_memory_stick_error_without_a_suspend_fails_the_job(void) {
    const uint32_t id = add_job();
    storage.fail_suffix = SKIFF_DOWNLOAD_PART_SUFFIX;
    storage.write_budget = MIB + 10;
    storage.write_error = SKIFF_ERR_STORAGE_IO;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_FAILED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, job_with(id).error);
}

static void test_when_rejoining_fails_the_modules_are_reloaded(void) {
    const uint32_t id = add_job();
    env_state.suspend_at = 600;
    env_state.rejoin_result = SKIFF_ERR_NET_WIFI_JOIN;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(1, env_state.rejoins);
    TEST_ASSERT_EQUAL_INT(1, env_state.reloads);
    TEST_ASSERT_EQUAL_INT(1, count_events(SKIFF_JOBS_EVENT_RECOVERY, SKIFF_JOBS_RELOADING));
}

static void test_a_network_that_never_comes_back_fails_the_job(void) {
    const uint32_t id = add_job();
    env_state.suspend_at = 600;
    env_state.rejoin_result = SKIFF_ERR_NET_WIFI_JOIN;
    env_state.reload_result = SKIFF_ERR_NET_WIFI_JOIN;
    for (int i = 1; i < SCRIPT_MAX; i++) {
        env_state.script[i].fail_before_response = SKIFF_ERR_NET_CONNECT;
    }
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_FAILED, job_with(id).state);
    TEST_ASSERT_TRUE(exists(part));
    TEST_PRINTF("the progress stays for a retry");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_retry(jobs, id));
    env_state.rejoin_result = SKIFF_OK;
    env_state.online = 1;
    memset(env_state.script, 0, sizeof env_state.script);
    env_state.opens = 0;
    run();
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    TEST_ASSERT_TRUE(transport.log[transport.log_count - 1].has_range);
}

static void test_a_cancel_mid_transfer_stops_and_deletes_the_progress(void) {
    const uint32_t id = add_job();
    env_state.cancel_at = 900;
    env_state.cancel_id = id;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_CANCELLED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CANCELLED, job_with(id).error);
    TEST_ASSERT_FALSE(exists(part));
    TEST_ASSERT_FALSE(exists(state_file));
    TEST_ASSERT_FALSE(exists(target));
}

static void test_a_cancel_during_a_wait_ends_the_wait(void) {
    const uint32_t id = add_job();
    for (int i = 0; i < SCRIPT_MAX; i++) {
        env_state.script[i].fail_before_response = SKIFF_ERR_NET_CONNECT;
    }
    env_state.cancel_after_sleeps = 1;
    env_state.cancel_id = id;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_CANCELLED, job_with(id).state);
    TEST_PRINTF("the 1 s wait ended at its first step");
    TEST_ASSERT_EQUAL_UINT64(SKIFF_JOBS_POLL_MS, env_state.slept_ms);
}

static void test_an_active_cancel_whose_files_stay_fails_the_job(void) {
    const uint32_t id = add_job();
    env_state.cancel_at = 900;
    env_state.cancel_id = id;
    storage.fail_suffix = SKIFF_DOWNLOAD_STATE_SUFFIX;
    storage.remove_error = SKIFF_ERR_STORAGE_IO;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_FAILED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, job_with(id).error);
}

static void test_an_active_cancel_the_file_refuses_keeps_the_progress(void) {
    const uint32_t id = add_job();
    env_state.cancel_at = 900;
    env_state.cancel_id = id;
    storage.fail_suffix = SKIFF_STORAGE_DRAFT_SUFFIX;
    /* The active state saves; from the cancel on, the queue file refuses. */
    env_state.refuse_saves_at_cancel = 1;
    run();
    report(id);
    TEST_ASSERT_TRUE(exists(part));
    TEST_ASSERT_TRUE(exists(state_file));
    storage.sync_error = SKIFF_OK;
    TEST_PRINTF("after a restart the job is queued again and resumes");
    skiff_jobs_destroy(jobs);
    create_jobs();
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, job_with(id).state);
}

static void test_a_retry_cannot_take_another_jobs_target(void) {
    const uint32_t first = add_job();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_cancel(jobs, first));
    skiff_job_request other = request_for("Another file.iso", target);
    uint32_t second = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &other, &second));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_retry(jobs, first));
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_CANCELLED, job_with(first).state);
}

static void test_a_queued_job_cancels_at_once(void) {
    const uint32_t id = add_job();
    char folder[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "ISO", folder, sizeof folder));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdir(posix, folder));
    write_text(part, "partial");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_cancel(jobs, id));
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_CANCELLED, job_with(id).state);
    TEST_ASSERT_FALSE(exists(part));
    int ran = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_run_one(jobs, &env, &ran));
    TEST_ASSERT_EQUAL_INT(0, ran);
}

static void test_a_quit_leaves_the_job_queued_and_it_resumes(void) {
    const uint32_t id = add_job();
    env_state.stop_at = 1200;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, job_with(id).state);
    TEST_ASSERT_TRUE(exists(part));
    TEST_ASSERT_TRUE(exists(state_file));
    TEST_PRINTF("as after a relaunch: the queue file is read again");
    skiff_jobs_destroy(jobs);
    create_jobs();
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    TEST_ASSERT_TRUE(transport.log[transport.log_count - 1].has_range);
    assert_complete();
}

static void test_a_stop_asked_for_between_jobs_starts_no_job(void) {
    const uint32_t id = add_job();
    skiff_jobs_request_stop(jobs);
    int ran = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_run_one(jobs, &env, &ran));
    TEST_ASSERT_EQUAL_INT(0, ran);
    TEST_PRINTF("still asked on the next call: the request stays until Skiff quits");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_run_one(jobs, &env, &ran));
    TEST_ASSERT_EQUAL_INT(0, ran);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(0, env_state.opens);
}

static void test_a_transport_that_cannot_open_fails_the_job(void) {
    const uint32_t id = add_job();
    env_state.open_error = SKIFF_ERR_NET_NEEDS_ARK;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_FAILED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_NEEDS_ARK, job_with(id).error);
}

static void test_a_folder_that_cannot_be_made_fails_the_job(void) {
    const uint32_t id = add_job();
    storage.mkdir_error = SKIFF_ERR_STORAGE_NO_SPACE;
    run();
    report(id);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_FAILED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, job_with(id).error);
}

/* ---- Arguments and locking ---- */

static int is_queue_file(const char *path) {
    return strncmp(path, queue_path, strlen(queue_path)) == 0;
}

/* Every Memory Stick open and remove: the state lock must not be held, and a save of the queue
 * file must hold the commit lock. */
static void watch_io(void *ctx, const char *call, const char *path) {
    (void)ctx;
    locks.io_under_state += locks.state.held;
    if (strcmp(call, "open") == 0 && is_queue_file(path)) {
        locks.queue_opens++;
        locks.queue_opens_without_commit += !locks.commit.held;
    }
}

static void create_tracked_jobs(void) {
    skiff_jobs_destroy(jobs);
    memset(&locks, 0, sizeof locks);
    const skiff_jobs_config config = {&storage.base,     queue_path,        logger,
                                      tracked_lock_take, tracked_lock_give, &locks.state,
                                      &locks.commit};
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_create(&config, &jobs));
    storage.on_call = watch_io;
}

static void assert_locks_used_well(void) {
    TEST_PRINTF("state lock taken %d times, commit lock %d, queue file opened %d times",
                locks.state.takes, locks.commit.takes, locks.queue_opens);
    TEST_ASSERT_EQUAL_INT(0, locks.state.held);
    TEST_ASSERT_EQUAL_INT(0, locks.commit.held);
    TEST_ASSERT_EQUAL_INT(0, locks.state.nested);
    TEST_ASSERT_EQUAL_INT(0, locks.commit.nested);
    TEST_ASSERT_EQUAL_INT(0, locks.commit_under_state);
    TEST_ASSERT_EQUAL_INT(0, locks.forbidden_commits);
    TEST_ASSERT_EQUAL_INT(0, locks.io_under_state);
    TEST_ASSERT_EQUAL_INT(0, locks.queue_opens_without_commit);
}

static void test_saves_hold_the_commit_lock_and_never_the_state_lock(void) {
    create_tracked_jobs();
    const uint32_t id = add_job();
    env_state.script[0].fail_mid_body = SKIFF_ERR_NET_CONNECTION_LOST;
    env_state.script[0].fail_after_bytes = MIB;
    run();
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_clear_finished(jobs));
    const uint32_t again = add_job();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_cancel(jobs, again));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_retry(jobs, again));
    skiff_jobs_request_stop(jobs);
    int ran = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_run_one(jobs, &env, &ran));
    TEST_ASSERT_EQUAL_INT(0, ran);
    TEST_ASSERT_GREATER_THAN_INT(6, locks.queue_opens);
    TEST_ASSERT_GREATER_THAN_INT(10, locks.state.takes);
    assert_locks_used_well();
}

/* What the UI reads while a save is on the Memory Stick. */
static size_t jobs_seen_during_save;
static int events_seen_during_save;

static void read_during_save(void *ctx, const char *call, const char *path) {
    watch_io(ctx, call, path);
    if (strcmp(call, "open") == 0 && is_queue_file(path) && jobs_seen_during_save == 0) {
        jobs_seen_during_save = skiff_jobs_list(jobs, NULL, 0);
        skiff_jobs_event event;
        while (skiff_jobs_next_event(jobs, &event)) {
            events_seen_during_save++;
        }
    }
}

static void test_a_reader_during_a_save_sees_the_queue_as_it_was(void) {
    create_tracked_jobs();
    add_job();
    drain_events();
    event_count = 0;
    char other_target[TEMP_DIR_PATH_MAX];
    const skiff_job_request request = request_named("Other.iso", other_target, sizeof other_target);
    jobs_seen_during_save = 0;
    events_seen_during_save = 0;
    storage.on_call = read_during_save;
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &request, &id));
    TEST_PRINTF("during the save the UI saw %zu job(s) and %d event(s); after it, %zu",
                jobs_seen_during_save, events_seen_during_save, skiff_jobs_list(jobs, NULL, 0));
    TEST_ASSERT_EQUAL_size_t(1, jobs_seen_during_save);
    TEST_ASSERT_EQUAL_INT(0, events_seen_during_save);
    TEST_ASSERT_EQUAL_size_t(2, skiff_jobs_list(jobs, NULL, 0));
    drain_events();
    TEST_ASSERT_EQUAL_size_t(1, event_count);
    TEST_ASSERT_EQUAL_UINT32(id, events[0].job_id);
    assert_locks_used_well();
}

static void test_a_save_that_fails_leaves_no_trace_for_the_reader(void) {
    create_tracked_jobs();
    add_job();
    drain_events();
    event_count = 0;
    char other_target[TEMP_DIR_PATH_MAX];
    const skiff_job_request request = request_named("Other.iso", other_target, sizeof other_target);
    jobs_seen_during_save = 0;
    storage.on_call = read_during_save;
    storage.sync_error = SKIFF_ERR_STORAGE_IO;
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, skiff_jobs_add(jobs, &request, &id));
    storage.sync_error = SKIFF_OK;
    TEST_ASSERT_EQUAL_size_t(1, jobs_seen_during_save);
    TEST_ASSERT_EQUAL_size_t(1, skiff_jobs_list(jobs, NULL, 0));
    drain_events();
    TEST_ASSERT_EQUAL_size_t(0, event_count);
    /* Nor did the failed add use up an id. */
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_add(jobs, &request, &id));
    TEST_ASSERT_EQUAL_UINT32(2, id);
    assert_locks_used_well();
}

/* At the removal of a cancelled job's partial files: the locks held, and the job as the UI sees
 * it. */
static int discard_seen;
static int discard_without_commit;
static skiff_job_state discard_state_seen;
static uint32_t discard_job_id;

static void watch_discard(void *ctx, const char *call, const char *path) {
    watch_io(ctx, call, path);
    /* After the cancel: the engine's own removals at the start of a download do not count. */
    if (strcmp(call, "remove") == 0 && strcmp(path, part) == 0 &&
        env_state.checks >= env_state.cancel_at) {
        discard_seen++;
        discard_without_commit += !locks.commit.held;
        discard_state_seen = job_with(discard_job_id).state;
    }
}

static void test_a_cancelled_jobs_files_go_under_the_commit_lock_alone(void) {
    create_tracked_jobs();
    discard_job_id = add_job();
    discard_seen = 0;
    discard_without_commit = 0;
    storage.on_call = watch_discard;
    env_state.cancel_at = 900;
    env_state.cancel_id = discard_job_id;
    run();
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_CANCELLED, job_with(discard_job_id).state);
    TEST_ASSERT_FALSE(exists(part));
    TEST_PRINTF("the .part file was removed %d time(s), %d without the commit lock; the UI saw "
                "the job %s",
                discard_seen, discard_without_commit, skiff_job_state_name(discard_state_seen));
    TEST_ASSERT_GREATER_THAN_INT(0, discard_seen);
    /* Held, so no other job can claim the target until the files are gone; the UI still reads. */
    TEST_ASSERT_EQUAL_INT(0, discard_without_commit);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_CANCELLED, discard_state_seen);
    assert_locks_used_well();
}

static void stop_during_save(void *ctx, const char *call, const char *path) {
    watch_io(ctx, call, path);
    if (strcmp(call, "open") == 0 && is_queue_file(path)) {
        skiff_jobs_request_stop(jobs);
    }
}

static void test_a_stop_asked_for_during_the_start_save_starts_nothing(void) {
    create_tracked_jobs();
    const uint32_t id = add_job();
    storage.on_call = stop_during_save;
    int ran = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_run_one(jobs, &env, &ran));
    TEST_PRINTF("ran %d, job %s, %d transport(s) opened", ran,
                skiff_job_state_name(job_with(id).state), env_state.opens);
    TEST_ASSERT_EQUAL_INT(0, ran);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, job_with(id).state);
    TEST_ASSERT_EQUAL_INT(0, env_state.opens);
    assert_locks_used_well();
    /* The file may say active; a restart reads that as queued, and the job runs. */
    storage.on_call = NULL;
    skiff_jobs_destroy(jobs);
    create_jobs();
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_QUEUED, job_with(id).state);
    run();
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_DONE, job_with(id).state);
}

static uint32_t late_cancel_id;

static void cancel_as_the_job_ends(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_jobs_cancel(jobs, late_cancel_id));
}

/* A cancel acknowledged after the attempt ended, just before the runner records how: it is not
 * lost, whatever the attempt's own outcome (here a quit, which leaves the job queued). */
static void test_a_cancel_as_the_job_ends_is_not_lost(void) {
    create_tracked_jobs();
    late_cancel_id = add_job();
    /* The run's commits: the start, then the end. */
    locks.commit_hook = cancel_as_the_job_ends;
    locks.commit_hook_at = locks.commit.takes + 2;
    env_state.stop_at = 900;
    run();
    TEST_PRINTF("job %s, .part %s", skiff_job_state_name(job_with(late_cancel_id).state),
                exists(part) ? "kept" : "deleted");
    TEST_ASSERT_NULL(locks.commit_hook);
    TEST_ASSERT_EQUAL_INT(SKIFF_JOB_CANCELLED, job_with(late_cancel_id).state);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CANCELLED, job_with(late_cancel_id).error);
    TEST_ASSERT_FALSE(exists(part));
    TEST_ASSERT_FALSE(exists(state_file));
    assert_locks_used_well();
}

static void test_bad_arguments_are_refused(void) {
    skiff_jobs *other = NULL;
    skiff_jobs_config config = {&storage.base, queue_path, NULL, tracked_lock_take,
                                NULL,          NULL,       NULL};
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_create(&config, &other));
    TEST_ASSERT_NULL(other);
    /* Lock hooks need a commit lock, and one apart from the state lock. */
    config.unlock = tracked_lock_give;
    config.lock_ctx = &locks.state;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_create(&config, &other));
    config.save_lock_ctx = &locks.state;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_create(&config, &other));
    TEST_ASSERT_NULL(other);
    config.lock = NULL;
    config.unlock = NULL;
    config.path = "";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_create(&config, &other));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_create(NULL, &other));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_create(&config, NULL));
    int ran = 1;
    skiff_jobs_env incomplete = env;
    incomplete.rejoin = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_run_one(jobs, &incomplete, &ran));
    TEST_ASSERT_EQUAL_INT(0, ran);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_run_one(jobs, &env, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_cancel(NULL, 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_retry(NULL, 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_jobs_clear_finished(NULL));
    TEST_ASSERT_EQUAL_size_t(0, skiff_jobs_list(NULL, NULL, 0));
    skiff_jobs_event event;
    TEST_ASSERT_EQUAL_INT(0, skiff_jobs_next_event(NULL, &event));
    skiff_jobs_request_stop(NULL);
    skiff_jobs_destroy(NULL);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    TEST_ASSERT_EQUAL_STRING("unknown", skiff_job_state_name((skiff_job_state)99));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_jobs_are_saved_and_survive_a_restart);
    RUN_TEST(test_the_same_file_is_not_queued_twice);
    RUN_TEST(test_a_full_queue_makes_room_from_finished_jobs_only);
    RUN_TEST(test_unusable_requests_are_refused);
    RUN_TEST(test_a_job_the_file_cannot_hold_is_not_added);
    RUN_TEST(test_a_damaged_queue_file_starts_an_empty_queue);
    RUN_TEST(test_an_unusable_job_is_dropped_and_the_rest_kept);
    RUN_TEST(test_the_queue_file_round_trips);
    RUN_TEST(test_a_queue_file_without_the_replace_flag_never_replaces);
    RUN_TEST(test_retry_and_clear_finished);
    RUN_TEST(test_events_coalesce_progress_and_drop_the_oldest);
    RUN_TEST(test_player_changes_the_file_cannot_hold_are_undone);
    RUN_TEST(test_a_cancel_whose_files_cannot_be_deleted_is_refused);
    RUN_TEST(test_a_target_too_long_for_its_partial_files_is_refused);
    RUN_TEST(test_a_refused_cancel_keeps_the_progress);
    RUN_TEST(test_two_jobs_never_write_to_one_target);
    RUN_TEST(test_a_job_downloads_into_a_folder_it_creates);
    RUN_TEST(test_the_log_is_written_right_after_each_block);
    RUN_TEST(test_progress_reports_bytes_and_speed);
    RUN_TEST(test_an_error_a_retry_cannot_fix_fails_the_job);
    RUN_TEST(test_a_file_found_at_the_target_fails_the_job_and_keeps_it);
    RUN_TEST(test_skiffs_own_copy_is_replaced_when_the_job_says_so);
    RUN_TEST(test_a_connection_lost_with_progress_is_retried_at_once);
    RUN_TEST(test_attempts_that_get_nothing_back_off_then_fail);
    RUN_TEST(test_bytes_received_reset_the_count_of_idle_attempts);
    RUN_TEST(test_with_the_switch_off_the_runner_waits_without_failing);
    RUN_TEST(test_after_a_suspend_the_profile_is_rejoined);
    RUN_TEST(test_a_memory_stick_error_across_a_suspend_costs_only_the_attempt);
    RUN_TEST(test_a_memory_stick_error_without_a_suspend_fails_the_job);
    RUN_TEST(test_when_rejoining_fails_the_modules_are_reloaded);
    RUN_TEST(test_a_network_that_never_comes_back_fails_the_job);
    RUN_TEST(test_a_cancel_mid_transfer_stops_and_deletes_the_progress);
    RUN_TEST(test_a_cancel_during_a_wait_ends_the_wait);
    RUN_TEST(test_an_active_cancel_whose_files_stay_fails_the_job);
    RUN_TEST(test_an_active_cancel_the_file_refuses_keeps_the_progress);
    RUN_TEST(test_a_retry_cannot_take_another_jobs_target);
    RUN_TEST(test_a_queued_job_cancels_at_once);
    RUN_TEST(test_a_quit_leaves_the_job_queued_and_it_resumes);
    RUN_TEST(test_a_stop_asked_for_between_jobs_starts_no_job);
    RUN_TEST(test_a_transport_that_cannot_open_fails_the_job);
    RUN_TEST(test_a_folder_that_cannot_be_made_fails_the_job);
    RUN_TEST(test_saves_hold_the_commit_lock_and_never_the_state_lock);
    RUN_TEST(test_a_reader_during_a_save_sees_the_queue_as_it_was);
    RUN_TEST(test_a_save_that_fails_leaves_no_trace_for_the_reader);
    RUN_TEST(test_a_cancelled_jobs_files_go_under_the_commit_lock_alone);
    RUN_TEST(test_a_stop_asked_for_during_the_start_save_starts_nothing);
    RUN_TEST(test_a_cancel_as_the_job_ends_is_not_lost);
    RUN_TEST(test_bad_arguments_are_refused);
    return UNITY_END();
}
