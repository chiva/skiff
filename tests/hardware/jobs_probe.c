/*
 * Jobs probe (Phase 2, hardware row J1): the download queue (skiff/jobs.h) on its real worker
 * thread (src/platform/psp/jobs_psp.h) on a PSP, while the main thread draws a frame every vertical
 * blank with the UI renderer, as the app will. Against the test RomM from tests/integration
 * (`scripts/dev.sh romm-lan`, with a large seeded file: SKIFF_PAYLOAD_BYTES=67108864), it queues
 * the seeded file and asks the player to interrupt it:
 *
 *   - at 20%: turn the Wi-Fi switch off, then on again when asked; the probe logs how soon the
 *     runner noticed and how long it took to download again;
 *   - at 50%: put the PSP to sleep and wake it; the probe logs how long until bytes arrived again.
 *
 * It measures what jobs/ was sized by guesswork: the worker's lowest free stack (of
 * SKIFF_PSP_WORKER_STACK_BYTES), the system memory left once joined and while downloading, the
 * download speed with the UI drawing, and the longest gap between two frames (the UI waits for the
 * queue's lock while the worker saves the queue file). The finished file is read back and checked
 * against RomM's CRC-32.
 *
 * A job left unfinished by an earlier run (HOME > Quit, a crash) is still in the queue file and
 * resumes from its .part file, which tests the queue across launches.
 *
 * scripts/memstick.sh install writes jobs-probe.ini (server, profile, seeded file and token, and an
 * optional wait_s=, how long a prompt waits for the player) and copies the test CA next to the
 * EBOOT. Each run appends one line to jobs-log.txt; Skiff's own log goes to skiff.log.
 *
 * Without ARK (PPSSPP in CI) TLS cannot start: the probe queues a job that fails before any network
 * I/O (SKIFF_ERR_NET_NEEDS_ARK), and checks that the worker thread ran it, that the queue file on
 * the Memory Stick says so, that the log got lines from both threads, and that the thread stops.
 * It ends with SKIFF JOBS PROBE NO ARK OK.
 */
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspkernel.h>
#include <pspwlan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "skiff/curl_transport.h"
#include "skiff/jobs.h"
#include "skiff/log.h"
#include "skiff/romm.h"
#include "skiff/selftest.h"
#include "skiff/storage.h"
#include "skiff/ui.h"

#include "jobs_psp.h"
#include "kirk_entropy.h"
#include "lifecycle.h"
#include "net_psp.h"
#include "probe_psp.h"
#include "probe_support.h"
#include "report.h"
#include "storage_psp.h"
#include "ui_psp.h"

#define PROBE_TITLE "Skiff jobs probe"
#define PROBE_OK_MARKER "SKIFF JOBS PROBE OK"
#define PROBE_FAIL_MARKER "SKIFF JOBS PROBE FAIL"
#define PROBE_NO_ARK_OK_MARKER "SKIFF JOBS PROBE NO ARK OK"
#define PROBE_NO_ARK_FAIL_MARKER "SKIFF JOBS PROBE NO ARK FAIL"
#define PROBE_CONFIG_FILE "jobs-probe.ini"
#define PROBE_LOG_FILE "jobs-log.txt"
#define CA_FILE "ca.crt"
#define QUEUE_FILE "jobs-queue.json"
#define SKIFF_LOG_FILE "skiff.log"
/* The download's target, next to the EBOOT; its .part and .resume files sit beside it. */
#define TARGET_FILE "jobs-test.bin"
/* Without ARK nothing is downloaded: a job that never reaches the network. */
#define NO_ARK_SERVER "https://romm.invalid"
#define NO_ARK_FILE_NAME "never-downloaded.iso"
#define NO_ARK_ROM_ID 1U
#define NO_ARK_SIZE 1U
#define DECIMAL_SEPARATOR "."
#define ACTION_WIFI_OFF "ACTION: turn the Wi-Fi switch OFF now"
#define ACTION_WIFI_ON "ACTION: turn the Wi-Fi switch back ON"
#define ACTION_SUSPEND "ACTION: slide POWER down to sleep, wait 5 s, slide it again to wake"

enum {
    PATH_MAX_LEN = SKIFF_PROBE_PATH_MAX,
    LONG_LINE_MAX = SKIFF_PROBE_LONG_LINE_MAX,
    TEXT_LINE_MAX = SKIFF_SELFTEST_LINE_MAX,
    DECIMAL_BASE = 10,
    HTTPS_PORT = 8443,
    US_PER_MS = 1000,
    BYTES_PER_KB = 1024,
    PERCENT = 100,
    /* The player is asked to turn the Wi-Fi switch off here, and to suspend here. */
    WIFI_AT_PERCENT = 20,
    SUSPEND_AT_PERCENT = 50,
    /* The speed the download should keep with the UI drawing (J1's pass criterion). */
    MIN_KB_PER_S = 400,
    /* The worker should keep this much of its stack unused. */
    MIN_STACK_FREE_BYTES = 8 * 1024,
    /* The longest the UI may go between two frames (a suspend excepted): six frames. */
    MAX_FRAME_GAP_MS = 100,
    /* Memory and the stack are sampled this often, in frames. */
    SAMPLE_EVERY_FRAMES = 60,
    /* CRC-32 read-back of the finished file. */
    READ_BLOCK_BYTES = 64 * 1024,
    /* Screen layout below the header. */
    TEXT_LEFT = SKIFF_PSP_UI_MARGIN,
    LINE_FILE = 52,
    LINE_BYTES = 74,
    LINE_STATUS = 96,
    BAR_TOP = 110,
    BAR_HEIGHT = 12,
    BAR_WIDTH = SKIFF_PSP_UI_SCREEN_WIDTH - 2 * SKIFF_PSP_UI_MARGIN,
    LINE_ACTION = 160,
    LINE_NOTE = 190,
    BYTES_TEXT_MAX = 32,
};

#define US_PER_S (1000LL * 1000)
#define WIFI_JOIN_TIMEOUT_US (30LL * 1000 * 1000)
#define DISCONNECT_TIMEOUT_US (10LL * 1000 * 1000)
/* Without ARK the job fails at once; this much is plenty, inside run_eboot.sh's 30 s. */
#define NO_ARK_WAIT_US (8LL * 1000 * 1000)
/* With ARK the whole run gives up after this long. */
#define RUN_LIMIT_US (40LL * 60 * 1000 * 1000)

/* What the player is asked to do, in order. */
typedef enum phase {
    PHASE_DOWNLOAD,
    PHASE_WIFI_OFF,
    PHASE_WIFI_ON,
    PHASE_BEFORE_SUSPEND,
    PHASE_SUSPEND,
    PHASE_FINISH,
} phase;

typedef struct probe {
    skiff_psp_report report;
    const char *program_path;
    int has_ark;
    skiff_probe_config config;
    skiff_psp_net net;
    skiff_storage *storage;
    skiff_psp_mutex jobs_lock;
    skiff_psp_mutex log_lock;
    skiff_log *log;
    skiff_jobs *jobs;
    skiff_romm_client romm;
    skiff_psp_worker worker;
    /* The worker did not stop in time and may still use the queue, the log, TLS and the network:
     * nothing is torn down, the process exit releases it all. */
    int worker_alive;
    skiff_psp_ui ui;
    intraFont *font;
    char queue_path[PATH_MAX_LEN];
    char log_path[PATH_MAX_LEN];
    char target[PATH_MAX_LEN];
    char ca[PATH_MAX_LEN];
    char file_name[SKIFF_PROBE_FILE_NAME_MAX];
    char base_url[SKIFF_CONFIG_URL_MAX];
} probe;

/* What the main thread saw of the run. */
typedef struct watch {
    uint32_t job_id;
    int ended;
    skiff_job_state state;
    skiff_err error;
    int saw_active;
    skiff_ui_progress progress;
    int has_progress;
    long long last_progress_us;
    uint64_t rate_before_wifi;
    uint64_t rate_last;
    long long first_progress_us;
    uint64_t first_done;
    /* The player's part. */
    phase phase;
    const char *action;
    long long action_since_us;
    long long switch_off_us;
    long long switch_on_us;
    long long wifi_noticed_ms;
    long long wifi_recovered_ms;
    int wifi_done;
    int suspends_before;
    int resumes_before;
    long long resumed_us;
    long long suspend_recovered_ms;
    int suspend_done;
    /* Recovery steps the runner reported. */
    int waits_for_wifi;
    int rejoins;
    int reloads;
    int retries;
    char status[TEXT_LINE_MAX];
    /* Frames: the longest gap between two (a suspend's excepted), and how many. */
    long long last_frame_us;
    long long frame_gap_max_us;
    long frames;
    /* The lowest system memory seen while the worker ran. */
    SceSize system_free_min;
    SceSize system_largest_min;
} watch;

/* The RomM client needs a transport for its own requests; the probe makes none, and the jobs open
 * their own. */
static skiff_err unused_perform(skiff_transport *transport, const skiff_http_request *request,
                                skiff_http_response *response) {
    (void)transport;
    (void)request;
    (void)response;
    return SKIFF_ERR_NOT_IMPLEMENTED;
}

static void unused_destroy(skiff_transport *transport) { (void)transport; }

static const skiff_transport_ops UNUSED_TRANSPORT_OPS = {unused_perform, unused_destroy};
static skiff_transport unused_transport = {&UNUSED_TRANSPORT_OPS};

static long long now_us(void) { return sceKernelGetSystemTimeWide(); }

static void report_check(probe *p, int ok, const char *text) {
    skiff_probe_report_check(&p->report, ok, text);
}

/* While GU draws, lines go to stdout and result.txt only. */
static void report_offscreen(probe *p, const char *text) {
    skiff_psp_report_line_offscreen(&p->report, text);
}

static int append_log(probe *p, const char *line) {
    char path[PATH_MAX_LEN];
    FILE *file =
        skiff_probe_sibling(p->program_path, PROBE_LOG_FILE, path) ? fopen(path, "a") : NULL;
    if (file == NULL) {
        report_check(p, 0, "run log: could not open " PROBE_LOG_FILE);
        return 0;
    }
    const int written = fprintf(file, "%s\n", line) > 0;
    return fclose(file) == 0 && written;
}

/* ---- Set-up shared by both paths ---- */

static int open_queue_and_log(probe *p) {
    char text[TEXT_LINE_MAX];
    if (!skiff_probe_sibling(p->program_path, QUEUE_FILE, p->queue_path) ||
        !skiff_probe_sibling(p->program_path, SKIFF_LOG_FILE, p->log_path) ||
        !skiff_probe_sibling(p->program_path, TARGET_FILE, p->target)) {
        report_check(p, 0, "paths next to the EBOOT do not fit");
        return 0;
    }
    if (skiff_psp_mutex_create(&p->jobs_lock, "skiff_jobs_lock") != SKIFF_OK ||
        skiff_psp_mutex_create(&p->log_lock, "skiff_log_lock") != SKIFF_OK) {
        report_check(p, 0, "mutexes: the kernel refused a semaphore");
        return 0;
    }
    const skiff_log_config log_config = {
        .storage = p->storage,
        .path = p->log_path,
        .level = SKIFF_LOG_DEBUG,
        .clock = skiff_psp_utc_ms,
        .lock = skiff_psp_mutex_lock,
        .unlock = skiff_psp_mutex_unlock,
        .lock_ctx = &p->log_lock,
    };
    skiff_err err = skiff_log_create(&log_config, &p->log);
    if (err == SKIFF_OK) {
        skiff_log_write(p->log, SKIFF_LOG_INFO, "probe", "%s, ARK %s", PROBE_TITLE,
                        p->has_ark ? "present" : "missing");
        const skiff_jobs_config jobs_config = {
            .storage = p->storage,
            .path = p->queue_path,
            .log = p->log,
            .lock = skiff_psp_mutex_lock,
            .unlock = skiff_psp_mutex_unlock,
            .lock_ctx = &p->jobs_lock,
        };
        err = skiff_jobs_create(&jobs_config, &p->jobs);
    }
    snprintf(text, sizeof text, "queue %s and log: %s", QUEUE_FILE, skiff_err_name(err));
    report_check(p, err == SKIFF_OK, text);
    return err == SKIFF_OK;
}

static void close_queue_and_log(probe *p) {
    skiff_jobs_destroy(p->jobs);
    p->jobs = NULL;
    skiff_log_destroy(p->log);
    p->log = NULL;
    skiff_psp_mutex_destroy(&p->jobs_lock);
    skiff_psp_mutex_destroy(&p->log_lock);
}

/* Unfinished jobs a quit or a crash left in the queue file: they run first. */
static void report_leftovers(probe *p) {
    static skiff_job jobs[SKIFF_JOBS_MAX];
    const size_t count = skiff_jobs_list(p->jobs, jobs, SKIFF_JOBS_MAX);
    int unfinished = 0;
    for (size_t i = 0; i < count && i < SKIFF_JOBS_MAX; i++) {
        unfinished += jobs[i].state == SKIFF_JOB_QUEUED;
    }
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "queue file: %zu job(s), %d unfinished from an earlier run", count,
             unfinished);
    skiff_psp_report_line(&p->report, text);
}

static int start_ui(probe *p) {
    p->font = skiff_psp_ui_load_font();
    if (p->font == NULL) {
        report_check(p, 0, "UI: the Latin firmware font (ltn0.pgf) did not load");
        return 0;
    }
    skiff_psp_ui_start(&p->ui, p->font);
    return 1;
}

static void stop_ui(probe *p) {
    if (p->font == NULL) {
        return;
    }
    skiff_psp_ui_stop(&p->ui);
    skiff_psp_ui_unload_font(p->font);
    p->font = NULL;
    /* GU drew over the debug screen; give the report lines their screen back. */
    pspDebugScreenInit();
}

static int start_worker(probe *p, skiff_err transport_status) {
    const skiff_psp_worker_config config = {
        .jobs = p->jobs,
        .romm = &p->romm,
        .net = &p->net,
        .profile = p->config.profile,
        .curl = {.ca_file = p->ca[0] != '\0' ? p->ca : NULL},
        .transport_status = transport_status,
        .log = p->log,
    };
    const skiff_err err = skiff_psp_worker_start(&p->worker, &config);
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "worker thread: %s, priority 0x%X, stack %d bytes",
             skiff_err_name(err), SKIFF_PSP_WORKER_PRIORITY, SKIFF_PSP_WORKER_STACK_BYTES);
    report_check(p, err == SKIFF_OK, text);
    return err == SKIFF_OK;
}

/* Stops the worker and reports its stack: 0 when it did not stop or used too much of it. */
static int stop_worker(probe *p) {
    const long long start = now_us();
    const skiff_err err = skiff_psp_worker_stop(&p->worker, SKIFF_PSP_WORKER_STOP_TIMEOUT_US);
    p->worker_alive = err != SKIFF_OK;
    const int stack_free = skiff_psp_worker_stack_free(&p->worker);
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "worker stopped: %s in %lld ms%s", skiff_err_name(err),
             (now_us() - start) / US_PER_MS,
             p->worker_alive ? "; still busy, so nothing is torn down before the exit" : "");
    report_check(p, err == SKIFF_OK, text);
    snprintf(text, sizeof text, "worker stack: lowest free %d of %d bytes (%d used)", stack_free,
             SKIFF_PSP_WORKER_STACK_BYTES, SKIFF_PSP_WORKER_STACK_BYTES - stack_free);
    report_check(p, stack_free >= MIN_STACK_FREE_BYTES, text);
    return err == SKIFF_OK && stack_free >= MIN_STACK_FREE_BYTES;
}

/* ---- The main thread: events, the player's part, and a frame per vertical blank ---- */

static void watch_init(watch *w, uint32_t job_id) {
    memset(w, 0, sizeof *w);
    w->job_id = job_id;
    w->phase = PHASE_DOWNLOAD;
    const skiff_probe_memory memory = skiff_probe_memory_now();
    w->system_free_min = memory.system_free;
    w->system_largest_min = memory.system_largest;
}

static void set_action(probe *p, watch *w, phase next, const char *action) {
    w->phase = next;
    w->action = action;
    w->action_since_us = now_us();
    if (action != NULL) {
        report_offscreen(p, action);
    }
}

static const char *recovery_name(skiff_jobs_recovery step) {
    switch (step) {
    case SKIFF_JOBS_WAITING_FOR_WIFI:
        return "waiting for Wi-Fi";
    case SKIFF_JOBS_REJOINING:
        return "rejoining the access point";
    case SKIFF_JOBS_RELOADING:
        return "reloading the network modules";
    case SKIFF_JOBS_RETRYING:
        return "retrying";
    }
    return "?";
}

static void on_recovery(probe *p, watch *w, const skiff_jobs_event *event) {
    const long long now = now_us();
    snprintf(w->status, sizeof w->status, "%s", recovery_name(event->step));
    switch (event->step) {
    case SKIFF_JOBS_WAITING_FOR_WIFI:
        w->waits_for_wifi++;
        if (w->phase == PHASE_WIFI_OFF && w->switch_off_us != 0) {
            w->wifi_noticed_ms = (now - w->switch_off_us) / US_PER_MS;
            set_action(p, w, PHASE_WIFI_ON, ACTION_WIFI_ON);
        }
        break;
    case SKIFF_JOBS_REJOINING:
        w->rejoins++;
        break;
    case SKIFF_JOBS_RELOADING:
        w->reloads++;
        break;
    case SKIFF_JOBS_RETRYING:
        w->retries++;
        break;
    }
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "job %u: %s (retry in %u ms) at %llu bytes", (unsigned)w->job_id,
             recovery_name(event->step), (unsigned)event->retry_in_ms,
             (unsigned long long)w->progress.done);
    report_offscreen(p, text);
}

static void on_progress(probe *p, watch *w, const skiff_jobs_event *event) {
    const long long now = now_us();
    const uint64_t now_ms = (uint64_t)(now / US_PER_MS);
    if (!w->has_progress) {
        w->has_progress = 1;
        w->first_progress_us = now;
        w->first_done = event->done;
        skiff_ui_progress_start(&w->progress, event->done, event->total, now_ms);
    } else {
        skiff_ui_progress_update(&w->progress, event->done, now_ms);
    }
    w->last_progress_us = now;
    if (event->bytes_per_s > 0) {
        w->rate_last = event->bytes_per_s;
    }
    w->status[0] = '\0';
    const unsigned percent = skiff_ui_progress_percent(&w->progress);
    if (w->phase == PHASE_DOWNLOAD && percent >= WIFI_AT_PERCENT) {
        w->rate_before_wifi = w->rate_last;
        set_action(p, w, PHASE_WIFI_OFF, ACTION_WIFI_OFF);
    } else if (w->phase == PHASE_WIFI_ON && w->switch_on_us != 0) {
        /* Bytes again after the switch came back on. */
        w->wifi_recovered_ms = (now - w->switch_on_us) / US_PER_MS;
        w->wifi_done = 1;
        set_action(p, w, PHASE_BEFORE_SUSPEND, NULL);
    } else if (w->phase == PHASE_BEFORE_SUSPEND && percent >= SUSPEND_AT_PERCENT) {
        const skiff_psp_power_events power = skiff_psp_power_events_now();
        w->suspends_before = power.suspends;
        w->resumes_before = power.resumes;
        set_action(p, w, PHASE_SUSPEND, ACTION_SUSPEND);
    } else if (w->phase == PHASE_SUSPEND && w->resumed_us != 0) {
        w->suspend_recovered_ms = (now - w->resumed_us) / US_PER_MS;
        w->suspend_done = 1;
        set_action(p, w, PHASE_FINISH, NULL);
    }
}

static void on_state(probe *p, watch *w, const skiff_jobs_event *event) {
    if (event->state == SKIFF_JOB_ACTIVE) {
        w->saw_active = 1;
    } else if (event->state != SKIFF_JOB_QUEUED) {
        w->ended = 1;
        w->state = event->state;
        w->error = event->error;
    }
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "job %u: %s %s (%d)", (unsigned)event->job_id,
             skiff_job_state_name(event->state), skiff_err_name(event->error), (int)event->error);
    report_offscreen(p, text);
}

static void take_events(probe *p, watch *w) {
    skiff_jobs_event event;
    while (skiff_jobs_next_event(p->jobs, &event)) {
        if (event.job_id != w->job_id) {
            continue;
        }
        if (event.kind == SKIFF_JOBS_EVENT_STATE) {
            on_state(p, w, &event);
        } else if (event.kind == SKIFF_JOBS_EVENT_RECOVERY) {
            on_recovery(p, w, &event);
        } else {
            on_progress(p, w, &event);
        }
    }
}

/* The Wi-Fi switch and the power events, as the player works them. */
static void watch_player(probe *p, watch *w) {
    const long long now = now_us();
    const int switch_on = sceWlanGetSwitchState() != 0;
    if (w->phase == PHASE_WIFI_OFF && !switch_on && w->switch_off_us == 0) {
        w->switch_off_us = now;
        report_offscreen(p, "Wi-Fi switch: off");
    }
    if (w->phase == PHASE_WIFI_ON && switch_on && w->switch_on_us == 0) {
        w->switch_on_us = now;
        report_offscreen(p, "Wi-Fi switch: on");
    }
    if (w->phase == PHASE_SUSPEND && w->resumed_us == 0) {
        const skiff_psp_power_events power = skiff_psp_power_events_now();
        if (power.suspends != w->suspends_before && power.resumes != w->resumes_before) {
            w->resumed_us = now;
            report_offscreen(p, "power: suspended and resumed");
        }
    }
    /* A prompt the player did not act on in time is dropped; the run goes on without it. */
    if (w->action != NULL && now - w->action_since_us > p->config.wait_s * US_PER_S) {
        char text[TEXT_LINE_MAX];
        snprintf(text, sizeof text, "no action within %d s: %s", p->config.wait_s, w->action);
        report_offscreen(p, text);
        if (w->phase == PHASE_WIFI_OFF || w->phase == PHASE_WIFI_ON) {
            set_action(p, w, PHASE_BEFORE_SUSPEND, NULL);
        } else {
            set_action(p, w, PHASE_FINISH, NULL);
        }
    }
}

static void sample(probe *p, watch *w) {
    if (w->frames % SAMPLE_EVERY_FRAMES != 0) {
        return;
    }
    const skiff_probe_memory memory = skiff_probe_memory_now();
    if (memory.system_free < w->system_free_min) {
        w->system_free_min = memory.system_free;
    }
    if (memory.system_largest < w->system_largest_min) {
        w->system_largest_min = memory.system_largest;
    }
    (void)skiff_psp_worker_stack_free(&p->worker);
}

static void draw(probe *p, const watch *w) {
    char done_text[BYTES_TEXT_MAX] = "-";
    char total_text[BYTES_TEXT_MAX] = "-";
    char rate_text[BYTES_TEXT_MAX + sizeof "/s"] = "";
    char line[TEXT_LINE_MAX];
    skiff_ui_format_bytes(w->progress.done, DECIMAL_SEPARATOR, done_text, sizeof done_text);
    skiff_ui_format_bytes(w->progress.total, DECIMAL_SEPARATOR, total_text, sizeof total_text);
    if (w->rate_last > 0) {
        char per_s[BYTES_TEXT_MAX] = "";
        skiff_ui_format_bytes(w->rate_last, DECIMAL_SEPARATOR, per_s, sizeof per_s);
        snprintf(rate_text, sizeof rate_text, "%s/s", per_s);
    }
    const unsigned percent = skiff_ui_progress_percent(&w->progress);
    skiff_psp_ui_begin_frame(&p->ui);
    skiff_psp_ui_header(&p->ui, PROBE_TITLE, rate_text);
    skiff_psp_ui_text(&p->ui, TEXT_LEFT, LINE_FILE, SKIFF_PSP_UI_TEXT_SIZE,
                      SKIFF_PSP_UI_COLOUR_TEXT, p->file_name);
    snprintf(line, sizeof line, "%s of %s (%u%%)", done_text, total_text, percent);
    skiff_psp_ui_text(&p->ui, TEXT_LEFT, LINE_BYTES, SKIFF_PSP_UI_TEXT_SIZE,
                      SKIFF_PSP_UI_COLOUR_TEXT, line);
    skiff_psp_ui_text(&p->ui, TEXT_LEFT, LINE_STATUS, SKIFF_PSP_UI_TEXT_SIZE,
                      SKIFF_PSP_UI_COLOUR_DIM_TEXT, w->status);
    skiff_psp_ui_progress_bar(&p->ui, TEXT_LEFT, BAR_TOP, BAR_WIDTH, BAR_HEIGHT, percent);
    if (w->action != NULL) {
        skiff_psp_ui_text(&p->ui, TEXT_LEFT, LINE_ACTION, SKIFF_PSP_UI_TEXT_SIZE,
                          SKIFF_PSP_UI_COLOUR_TEXT, w->action);
    }
    skiff_psp_ui_text(&p->ui, TEXT_LEFT, LINE_NOTE, SKIFF_PSP_UI_HINT_SIZE,
                      SKIFF_PSP_UI_COLOUR_DIM_TEXT,
                      "HOME > Quit stops the probe; run it again to resume");
    skiff_psp_ui_end_frame(&p->ui);
    skiff_psp_ui_present(&p->ui);
}

/*
 * Draws frames and follows the job until it ends, the player quits, or limit_us passes. Returns 1
 * when the job ended. A frame per vertical blank (present() waits for it) leaves the worker the
 * rest of the CPU.
 */
static int follow(probe *p, watch *w, long long limit_us) {
    const long long start = now_us();
    int suspends_seen = skiff_psp_power_events_now().suspends;
    while (!w->ended && !skiff_psp_exit_requested() && now_us() - start < limit_us) {
        /* The pad is read every frame, as the UI does; the probe needs no button. */
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        take_events(p, w);
        if (p->has_ark) {
            watch_player(p, w);
        }
        draw(p, w);
        const long long now = now_us();
        const int suspends = skiff_psp_power_events_now().suspends;
        if (w->last_frame_us != 0 && suspends == suspends_seen &&
            now - w->last_frame_us > w->frame_gap_max_us) {
            w->frame_gap_max_us = now - w->last_frame_us;
        }
        suspends_seen = suspends;
        w->last_frame_us = now;
        w->frames++;
        sample(p, w);
    }
    take_events(p, w);
    return w->ended;
}

/* ---- Without ARK: the worker, the queue file and the log, with no network ---- */

/* The queue file on the Memory Stick, as the worker left it: the job failed with expected. */
static int queue_file_says_failed(probe *p, uint32_t job_id, skiff_err expected) {
    static char json[SKIFF_JOBS_FILE_MAX];
    static skiff_job jobs[SKIFF_JOBS_MAX];
    size_t length = 0;
    size_t count = 0;
    size_t dropped = 0;
    uint32_t next_id = 0;
    skiff_err err = skiff_storage_read_whole(p->storage, p->queue_path, json, sizeof json, &length);
    if (err == SKIFF_OK) {
        err = skiff_jobs_parse(json, length, jobs, &count, &next_id, &dropped);
    }
    int found = 0;
    for (size_t i = 0; err == SKIFF_OK && i < count; i++) {
        found |=
            jobs[i].id == job_id && jobs[i].state == SKIFF_JOB_FAILED && jobs[i].error == expected;
    }
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "queue file: %s, %zu job(s), job %u failed with %s: %s",
             skiff_err_name(err), count, (unsigned)job_id, skiff_err_name(expected),
             found ? "yes" : "no");
    report_check(p, found, text);
    return found;
}

static int log_has_lines(probe *p) {
    skiff_log_flush(p->log);
    uint64_t size = 0;
    const skiff_err err = skiff_storage_size(p->storage, p->log_path, &size);
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "log %s: %s, %llu bytes", SKIFF_LOG_FILE, skiff_err_name(err),
             (unsigned long long)size);
    report_check(p, err == SKIFF_OK && size > 0, text);
    return err == SKIFF_OK && size > 0;
}

static int run_without_ark(probe *p) {
    skiff_psp_report_line(&p->report,
                          "no ARK: TLS cannot start; a job must fail on the worker thread");
    const skiff_err init = skiff_net_global_init();
    const skiff_err status = skiff_psp_entropy_status();
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "TLS refuses: %s, entropy %s", skiff_err_name(init),
             skiff_err_name(status));
    report_check(p, init != SKIFF_OK && status == SKIFF_ERR_NET_NEEDS_ARK, text);
    if (init == SKIFF_OK) {
        skiff_net_global_cleanup();
        return 0;
    }
    snprintf(p->file_name, sizeof p->file_name, "%s", NO_ARK_FILE_NAME);
    p->config.profile = SKIFF_PROBE_DEFAULT_PROFILE;
    if (!open_queue_and_log(p) ||
        skiff_romm_client_init(&p->romm, &unused_transport, NO_ARK_SERVER, NULL) != SKIFF_OK) {
        return 0;
    }
    report_leftovers(p);
    const skiff_job_request request = {.rom_id = NO_ARK_ROM_ID,
                                       .title = NO_ARK_FILE_NAME,
                                       .file_name = NO_ARK_FILE_NAME,
                                       .target = p->target,
                                       .size = NO_ARK_SIZE};
    uint32_t job_id = 0;
    skiff_err err = skiff_jobs_add(p->jobs, &request, &job_id);
    snprintf(text, sizeof text, "job queued: %s, id %u", skiff_err_name(err), (unsigned)job_id);
    report_check(p, err == SKIFF_OK, text);
    int ok = err == SKIFF_OK && start_ui(p);
    if (ok && start_worker(p, status)) {
        static watch w;
        watch_init(&w, job_id);
        const int ended = follow(p, &w, NO_ARK_WAIT_US);
        ok = stop_worker(p) && ended;
        stop_ui(p);
        snprintf(text, sizeof text,
                 "job ended on the worker thread: %s, %s (%d), active first %d, %ld frames, "
                 "longest frame gap %lld ms",
                 ended ? skiff_job_state_name(w.state) : "no", skiff_err_name(w.error),
                 (int)w.error, w.saw_active, w.frames, w.frame_gap_max_us / US_PER_MS);
        const int failed_as_expected =
            ended && w.saw_active && w.state == SKIFF_JOB_FAILED && w.error == status;
        report_check(p, failed_as_expected, text);
        ok = ok && failed_as_expected && queue_file_says_failed(p, job_id, status);
    } else {
        stop_ui(p);
        ok = 0;
    }
    if (p->worker_alive) {
        return 0;
    }
    ok = log_has_lines(p) && ok;
    close_queue_and_log(p);
    /* The next run starts with an empty queue. */
    skiff_storage_remove(p->storage, p->queue_path);
    return ok;
}

/* ---- With ARK: the seeded file from the test RomM, interrupted by the player ---- */

/* jobs-probe.ini: the server and the seeded file are required. */
static int read_config(probe *p) {
    if (!skiff_probe_load_config(&p->report, p->program_path, PROBE_CONFIG_FILE, &p->config)) {
        return 0;
    }
    const skiff_probe_config *config = &p->config;
    snprintf(p->base_url, sizeof p->base_url, "https://%s:%d", config->host, HTTPS_PORT);
    const int ok = skiff_probe_config_complete(config) && config->has_crc32 &&
                   skiff_probe_url_decode(config->file_name, p->file_name, sizeof p->file_name) &&
                   skiff_probe_existing_sibling(&p->report, p->program_path, CA_FILE, p->ca);
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "config: server %s, profile %d, ROM %s (%llu bytes), wait %d s",
             config->host[0] != '\0' ? config->host : "(missing)", config->profile,
             config->rom_id[0] != '\0' ? config->rom_id : "(missing)", config->size,
             config->wait_s);
    report_check(p, ok, text);
    return ok;
}

static int join(probe *p) {
    const long long start = now_us();
    const skiff_err err = skiff_psp_net_connect(&p->net, p->config.profile, WIFI_JOIN_TIMEOUT_US);
    if (err != SKIFF_OK) {
        skiff_probe_report_net_failure(&p->report, &p->net, err);
        return 0;
    }
    char ip[SKIFF_PSP_NET_IP_MAX] = "";
    skiff_psp_net_ip(&p->net, ip, sizeof ip);
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "Wi-Fi: profile %d joined in %lld ms, IP %s", p->config.profile,
             (now_us() - start) / US_PER_MS, ip);
    report_check(p, 1, text);
    return 1;
}

/* Reads the finished file back from the Memory Stick and checks it against RomM's CRC-32. */
static int file_matches(probe *p) {
    static unsigned char block[READ_BLOCK_BYTES];
    skiff_file *file = NULL;
    skiff_err err = skiff_storage_open(p->storage, p->target, SKIFF_FILE_READ, 0, &file);
    uLong crc = crc32(0L, Z_NULL, 0);
    uint64_t bytes = 0;
    size_t got = 1;
    while (err == SKIFF_OK && got > 0) {
        err = skiff_file_read(file, block, sizeof block, &got);
        if (err == SKIFF_OK && got > 0) {
            crc = crc32(crc, block, (uInt)got);
            bytes += got;
        }
    }
    const skiff_err close_err = skiff_file_close(file);
    if (err == SKIFF_OK) {
        err = close_err;
    }
    const int ok = err == SKIFF_OK && bytes == p->config.size && (uint32_t)crc == p->config.crc32;
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "file read back: %s, %llu bytes, CRC-32 %08lx (RomM %08lx)",
             skiff_err_name(err), (unsigned long long)bytes, (unsigned long)crc,
             (unsigned long)p->config.crc32);
    report_check(p, ok, text);
    return ok;
}

static void report_memory(probe *p, const char *label) {
    const skiff_probe_memory memory = skiff_probe_memory_now();
    skiff_probe_report_memory(&p->report, label, &memory);
}

/* The run's summary line, on screen, in result.txt and in jobs-log.txt. */
static int report_run(probe *p, const watch *w, int ok, int crc_ok) {
    const long long elapsed_us = w->last_progress_us - w->first_progress_us;
    char text[LONG_LINE_MAX];
    snprintf(text, sizeof text,
             "jobs ok=%d state=%s error=%s kb_s_first=%llu kb_s_last=%llu kb_s_overall=%llu "
             "wifi=%d wifi_noticed_ms=%lld wifi_recovered_ms=%lld suspend=%d "
             "suspend_recovered_ms=%lld waits=%d rejoins=%d reloads=%d retries=%d "
             "stack_free_min=%d/%d system_free_min=%u largest_min=%u frame_gap_max_ms=%lld "
             "frames=%ld crc=%d",
             ok, w->ended ? skiff_job_state_name(w->state) : "running", skiff_err_name(w->error),
             (unsigned long long)(w->rate_before_wifi / BYTES_PER_KB),
             (unsigned long long)(w->rate_last / BYTES_PER_KB),
             skiff_probe_kb_per_s(w->progress.done - w->first_done, elapsed_us), w->wifi_done,
             w->wifi_noticed_ms, w->wifi_recovered_ms, w->suspend_done, w->suspend_recovered_ms,
             w->waits_for_wifi, w->rejoins, w->reloads, w->retries, p->worker.stack_free_min,
             SKIFF_PSP_WORKER_STACK_BYTES, (unsigned)w->system_free_min,
             (unsigned)w->system_largest_min, w->frame_gap_max_us / US_PER_MS, w->frames, crc_ok);
    report_check(p, ok, text);
    return append_log(p, text) && ok;
}

static int run_download(probe *p) {
    report_leftovers(p);
    const skiff_job_request request = {.rom_id = strtoull(p->config.rom_id, NULL, DECIMAL_BASE),
                                       .title = p->file_name,
                                       .file_name = p->file_name,
                                       .target = p->target,
                                       .size = p->config.size,
                                       .has_crc32 = 1,
                                       .crc32 = p->config.crc32};
    uint32_t job_id = 0;
    skiff_err err = skiff_jobs_add(p->jobs, &request, &job_id);
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "job queued: %s, id %u", skiff_err_name(err), (unsigned)job_id);
    report_check(p, err == SKIFF_OK, text);
    if (err != SKIFF_OK || !start_ui(p)) {
        return 0;
    }
    if (!start_worker(p, SKIFF_OK)) {
        stop_ui(p);
        return 0;
    }
    report_memory(p, "worker started");
    static watch w;
    watch_init(&w, job_id);
    const int ended = follow(p, &w, RUN_LIMIT_US);
    const int quit = skiff_psp_exit_requested();
    int ok = stop_worker(p);
    stop_ui(p);
    if (quit) {
        report_check(p, 0, "HOME > Quit before the end: run the probe again to resume the job");
    }
    if (p->worker_alive) {
        return 0;
    }
    const int done = ended && w.state == SKIFF_JOB_DONE;
    const int crc_ok = done && file_matches(p);
    snprintf(text, sizeof text,
             "speed with the UI drawing: %llu KB/s before the Wi-Fi test, %llu "
             "KB/s at the end (at least %d)",
             (unsigned long long)(w.rate_before_wifi / BYTES_PER_KB),
             (unsigned long long)(w.rate_last / BYTES_PER_KB), MIN_KB_PER_S);
    /* Both: the first attempt's speed, and the last one's after the interruptions. */
    const int fast_enough = w.rate_before_wifi / BYTES_PER_KB >= MIN_KB_PER_S &&
                            w.rate_last / BYTES_PER_KB >= MIN_KB_PER_S;
    report_check(p, fast_enough, text);
    snprintf(text, sizeof text,
             "UI: longest gap between frames %lld ms over %ld frames (at most %d)",
             w.frame_gap_max_us / US_PER_MS, w.frames, MAX_FRAME_GAP_MS);
    const int smooth = w.frame_gap_max_us / US_PER_MS <= MAX_FRAME_GAP_MS;
    report_check(p, smooth, text);
    snprintf(text, sizeof text, "interruptions: Wi-Fi switch %s, suspend %s",
             w.wifi_done ? "recovered" : "not done", w.suspend_done ? "recovered" : "not done");
    report_check(p, w.wifi_done && w.suspend_done, text);
    ok = ok && done && crc_ok && fast_enough && smooth && w.wifi_done && w.suspend_done;
    ok = report_run(p, &w, ok, crc_ok);
    if (done) {
        skiff_storage_remove(p->storage, p->target);
        skiff_jobs_clear_finished(p->jobs);
    }
    return ok;
}

static int run_with_ark(probe *p) {
    if (!read_config(p)) {
        return 0;
    }
    const skiff_psp_power_events power = skiff_psp_power_events_now();
    char text[TEXT_LINE_MAX];
    snprintf(text, sizeof text, "power callback registered: %d (a slot, or a firmware error)",
             power.registration);
    report_check(p, power.registration >= 0, text);
    if (!skiff_probe_load_network(&p->report, &p->net, SKIFF_PSP_NET_CPU_MHZ)) {
        skiff_psp_net_unload(&p->net);
        return 0;
    }
    if (!join(p)) {
        skiff_probe_tear_down(&p->report, &p->net, DISCONNECT_TIMEOUT_US);
        return 0;
    }
    report_memory(p, "joined");
    const skiff_err init = skiff_net_global_init();
    snprintf(text, sizeof text, "TLS: skiff_net_global_init() %s", skiff_err_name(init));
    report_check(p, init == SKIFF_OK, text);
    int ok = init == SKIFF_OK &&
             skiff_romm_client_init(&p->romm, &unused_transport, p->base_url, p->config.token) ==
                 SKIFF_OK &&
             open_queue_and_log(p);
    if (ok) {
        ok = run_download(p);
    }
    if (p->worker_alive) {
        return 0;
    }
    close_queue_and_log(p);
    skiff_romm_client_clear(&p->romm);
    if (init == SKIFF_OK) {
        skiff_net_global_cleanup();
    }
    ok &= skiff_probe_tear_down(&p->report, &p->net, DISCONNECT_TIMEOUT_US);
    return ok;
}

int main(int argc, char *argv[]) {
    static probe p;
    memset(&p, 0, sizeof p);
    p.program_path = argc > 0 ? argv[0] : NULL;
    p.jobs_lock.sema = -1;
    p.log_lock.sema = -1;
    skiff_probe_config_defaults(&p.config);
    skiff_psp_install_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    skiff_psp_report_open(&p.report, p.program_path);
    skiff_psp_report_line(&p.report, PROBE_TITLE);

    p.has_ark = skiff_psp_entropy_status() != SKIFF_ERR_NET_NEEDS_ARK;
    int passed = skiff_psp_storage_create(&p.storage) == SKIFF_OK;
    if (passed) {
        passed = p.has_ark ? run_with_ark(&p) : run_without_ark(&p);
    }
    if (!p.worker_alive) {
        skiff_storage_destroy(p.storage);
    }
    const char *marker = p.has_ark ? (passed ? PROBE_OK_MARKER : PROBE_FAIL_MARKER)
                                   : (passed ? PROBE_NO_ARK_OK_MARKER : PROBE_NO_ARK_FAIL_MARKER);
    skiff_psp_report_line(&p.report, marker);
    skiff_psp_report_close(&p.report);
    sceKernelExitGame();
    return passed ? 0 : 1;
}
