/*
 * Resume probe (Phase 1 hardware spike, W7): Skiff's resumable downloads (skiff/download.h) on a
 * PSP, interrupted the ways a player interrupts them, against the test RomM from tests/integration
 * (`scripts/dev.sh romm-lan`, best with a large seeded file: SKIFF_PAYLOAD_BYTES=67108864). Each
 * scenario downloads the seeded file next to the EBOOT through the PSP's own file calls and the
 * app's transport, and ends with the CRC-32 RomM recorded:
 *
 *   - restart: the probe stops the download at 40% and resumes it on a new connection (206);
 *   - wifi: the player turns the Wi-Fi switch off and on again when asked; the probe logs how soon
 *     the download noticed, rejoins the access point and resumes;
 *   - suspend: the player puts the PSP to sleep and wakes it; the probe logs the power events, what
 *     the download returned, the CPU clock afterwards and how the network came back;
 *   - home: the player opens the HOME menu for a while; the probe logs the longest pause between
 *     chunks;
 *   - sleep: back-to-back downloads with no input for awake_s seconds while Auto Sleep is set to
 *     its shortest: keep-awake (scePowerTick) must stop the PSP from sleeping;
 *   - speed (only when named, about 15 minutes, no player needed): where a download's time goes.
 *     The Memory Stick alone (the file's size in 128 KB writes, from a 64-byte-aligned buffer and
 *     from one 8 bytes off, as malloc() returns), the network alone (no writes, without and with
 *     the stop and progress hooks), then whole downloads as the engine writes them and with every
 *     write copied to an aligned buffer first. Each line carries the signal and channel.
 *
 * A download left unfinished by an earlier run (HOME > Quit, a power-off) is resumed first, which
 * tests the .resume file across launches. Recovery is the same for every interruption: wait for the
 * Wi-Fi switch, rejoin the profile (or reload the network modules if rejoining fails), make a new
 * transport, and attempt again; jobs/ will take its policy from what this logs.
 *
 * scripts/memstick.sh install writes resume-probe.ini (server, profile, seeded file and token, and
 * optional scenarios=, wait_s= and awake_s=) and copies the test CA next to the EBOOT. Each
 * scenario appends one line to resume-log.txt.
 *
 * Without ARK (PPSSPP in CI) TLS cannot start. The probe then checks the PSP storage the downloads
 * write through (sceIo: offsets, sync, rename, remove) on the emulated Memory Stick, checks that
 * libcurl refuses to start, and ends with SKIFF RESUME PROBE NO ARK OK.
 */
#include <curl/curl.h>
#include <malloc.h>
#include <pspkernel.h>
#include <psppower.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "skiff/curl_transport.h"
#include "skiff/download.h"
#include "skiff/selftest.h"
#include "skiff/storage.h"

#include "kirk_entropy.h"
#include "lifecycle.h"
#include "net_psp.h"
#include "probe_psp.h"
#include "probe_support.h"
#include "report.h"
#include "storage_psp.h"

#define PROBE_OK_MARKER "SKIFF RESUME PROBE OK"
#define PROBE_FAIL_MARKER "SKIFF RESUME PROBE FAIL"
#define PROBE_NO_ARK_OK_MARKER "SKIFF RESUME PROBE NO ARK OK"
#define PROBE_NO_ARK_FAIL_MARKER "SKIFF RESUME PROBE NO ARK FAIL"
#define PROBE_CONFIG_FILE "resume-probe.ini"
#define PROBE_LOG_FILE "resume-log.txt"
#define CA_FILE "ca.crt"
/* The download's target, next to the EBOOT; its .part and .resume files sit beside it. */
#define TARGET_FILE "resume-test.bin"
#define STORAGE_CHECK_FILE "storage-check.bin"
#define STORAGE_CHECK_RENAMED "storage-check.renamed"
#define BEARER_PREFIX "Bearer "
#define PATH_RECONNECT "rejoined the profile"
#define PATH_RELOAD "reloaded the network modules"

enum {
    PATH_MAX_LEN = SKIFF_PROBE_PATH_MAX,
    LONG_LINE_MAX = SKIFF_PROBE_LONG_LINE_MAX,
    TOKEN_MAX = SKIFF_PROBE_TOKEN_MAX,
    /* https://<host>:8443/api/roms/<id>/content/<URL-encoded file name> */
    CONTENT_URL_MAX = 320,
    HTTPS_PORT = 8443,
    HTTP_STATUS_OK = 200,
    HTTP_STATUS_PARTIAL = 206,
    US_PER_MS = 1000,
    BYTES_PER_KB = 1024,
    PERCENT = 100,
    /* The player is asked to act once this share of the file has arrived. */
    PROMPT_AT_PERCENT = 20,
    /* The restart scenario stops the download itself here. */
    RESTART_AT_PERCENT = 40,
    /* A scenario gives up after this many attempts. */
    MAX_ATTEMPTS = 8,
    /* The storage check's file: three blocks, then a few bytes overwritten in the middle. */
    STORAGE_BLOCK_BYTES = 64 * 1024,
    STORAGE_BLOCKS = 3,
    STORAGE_PATCH_OFFSET = 100000,
    /* speed: the download's write size, the alignment the Memory Stick's DMA works in, what
     * newlib's malloc() guarantees, and how many parts of the file writes are timed in. */
    SPEED_BLOCK_BYTES = SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES,
    MS_ALIGNMENT = 64,
    MALLOC_ALIGNMENT = 8,
    SPEED_PARTS = 4,
};

#define US_PER_S (1000LL * 1000)
#define WIFI_JOIN_TIMEOUT_US (30LL * 1000 * 1000)
#define DISCONNECT_TIMEOUT_US (10LL * 1000 * 1000)
/* The stop hook asks the firmware about the network at most this often. */
#define NETWORK_POLL_US (100LL * 1000)
#define STATUS_INTERVAL_US (500LL * 1000)
#define WAIT_POLL_US (200LL * 1000)
#define STORAGE_PATCH "resume"

/* Why the stop hook ended an attempt. */
typedef enum stop_reason {
    STOP_NONE,
    STOP_BY_PROBE,
    STOP_QUIT,
    STOP_SUSPEND,
    STOP_SWITCH_OFF,
    STOP_ACCESS_POINT_LOST,
} stop_reason;

static const char *const STOP_REASON_NAMES[] = {
    "-", "the probe stopped it", "HOME > Quit", "suspend", "Wi-Fi switch off", "access point lost",
};

typedef struct probe {
    skiff_psp_report report;
    const char *program_path;
    skiff_probe_config config;
    skiff_psp_net net;
    /* The PSP storage, and the timing wrapper around it the downloads write through. */
    skiff_storage *psp_storage;
    skiff_storage *storage;
    skiff_transport *transport;
    char authorization[TOKEN_MAX + sizeof BEARER_PREFIX];
    char url[CONTENT_URL_MAX];
    char target[PATH_MAX_LEN];
    char ca[PATH_MAX_LEN];
} probe;

/* What one download saw, shared with its stop and progress hooks. */
typedef struct watch {
    probe *p;
    /* Shown once when the download passes prompt_at bytes. */
    const char *prompt;
    uint64_t prompt_at;
    int prompted;
    /* The probe stops the attempt itself at this many bytes; 0 for never. */
    uint64_t stop_at;
    skiff_psp_power_events power_at_attempt;
    long long last_poll_us;
    /* When the attempt started, its last body byte (0 before the first), the longest wait for a
     * first byte (request, TLS and the server's answer) and the longest pause between bytes. */
    long long attempt_start_us;
    long long last_byte_us;
    long long first_byte_us;
    long long longest_gap_us;
    long long last_status_us;
    uint64_t done;
    /* Why the stop hook ended the attempt, and how long after the last byte. */
    stop_reason stop_reason;
    long long detected_after_us;
} watch;

/* How a whole download went, over all its attempts. */
typedef struct outcome {
    int attempts;
    int interruptions;
    skiff_err first_interruption;
    stop_reason first_reason;
    /* The first interruption came from the transport, not the stop hook. */
    int first_from_transport;
    long long detected_after_us;
    uint64_t resumed_from;
    long last_status;
    int restarted;
    int complete;
    long long elapsed_us;
    /* Bytes of the files downloaded, for the speed. */
    uint64_t bytes;
    const char *recovery;
    long long recovery_us;
    int cpu_mhz_after;
} outcome;

/* ---- Where the Memory Stick's time goes: every storage call, timed ---- */

typedef enum storage_op {
    OP_OPEN,
    OP_READ,
    OP_WRITE,
    OP_SYNC,
    OP_CLOSE,
    OP_SIZE,
    OP_RENAME,
    OP_REMOVE,
    STORAGE_OPS,
} storage_op;

static const char *const STORAGE_OP_NAMES[STORAGE_OPS] = {
    "open", "read", "write", "sync", "close", "size", "rename", "remove",
};

typedef struct op_timing {
    int count;
    long long total_us;
    long long max_us;
} op_timing;

typedef struct timed_storage {
    skiff_storage base;
    skiff_storage *inner;
    op_timing ops[STORAGE_OPS];
    /* When set (SPEED_BLOCK_BYTES, 64-byte aligned), writes are copied here before reaching the
     * Memory Stick: the speed scenario's aligned download. */
    unsigned char *bounce;
} timed_storage;

typedef struct timed_file {
    skiff_file base;
    skiff_file *inner;
} timed_file;

/* One per probe, so the wrapper reaches it directly. */
static timed_storage timed;

static skiff_err timed_call(storage_op op, long long start_us, skiff_err err) {
    const long long elapsed_us = sceKernelGetSystemTimeWide() - start_us;
    op_timing *timing = &timed.ops[op];
    timing->count++;
    timing->total_us += elapsed_us;
    if (elapsed_us > timing->max_us) {
        timing->max_us = elapsed_us;
    }
    return err;
}

static skiff_err timed_open(skiff_storage *base, const char *path, skiff_file_mode mode,
                            uint64_t offset, skiff_file **out) {
    timed_file *file = calloc(1, sizeof *file);
    if (file == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    const long long start = sceKernelGetSystemTimeWide();
    const skiff_err err = timed_call(
        OP_OPEN, start, skiff_storage_open(timed.inner, path, mode, offset, &file->inner));
    if (err != SKIFF_OK) {
        free(file);
        return err;
    }
    file->base.storage = base;
    *out = &file->base;
    return SKIFF_OK;
}

static skiff_err timed_read(skiff_file *base, void *buffer, size_t size, size_t *got) {
    const long long start = sceKernelGetSystemTimeWide();
    return timed_call(OP_READ, start,
                      skiff_file_read(((timed_file *)base)->inner, buffer, size, got));
}

static skiff_err timed_write(skiff_file *base, const void *data, size_t size) {
    skiff_file *inner = ((timed_file *)base)->inner;
    const long long start = sceKernelGetSystemTimeWide();
    if (timed.bounce == NULL) {
        return timed_call(OP_WRITE, start, skiff_file_write(inner, data, size));
    }
    const unsigned char *next = data;
    skiff_err err = SKIFF_OK;
    while (err == SKIFF_OK && size > 0) {
        const size_t take = size < SPEED_BLOCK_BYTES ? size : SPEED_BLOCK_BYTES;
        memcpy(timed.bounce, next, take);
        err = skiff_file_write(inner, timed.bounce, take);
        next += take;
        size -= take;
    }
    return timed_call(OP_WRITE, start, err);
}

static skiff_err timed_sync(skiff_file *base) {
    const long long start = sceKernelGetSystemTimeWide();
    return timed_call(OP_SYNC, start, skiff_file_sync(((timed_file *)base)->inner));
}

static skiff_err timed_close(skiff_file *base) {
    timed_file *file = (timed_file *)base;
    const long long start = sceKernelGetSystemTimeWide();
    const skiff_err err = timed_call(OP_CLOSE, start, skiff_file_close(file->inner));
    free(file);
    return err;
}

static skiff_err timed_size(skiff_storage *base, const char *path, uint64_t *out) {
    (void)base;
    const long long start = sceKernelGetSystemTimeWide();
    return timed_call(OP_SIZE, start, skiff_storage_size(timed.inner, path, out));
}

static skiff_err timed_rename(skiff_storage *base, const char *from, const char *to) {
    (void)base;
    const long long start = sceKernelGetSystemTimeWide();
    return timed_call(OP_RENAME, start, skiff_storage_rename(timed.inner, from, to));
}

static skiff_err timed_remove(skiff_storage *base, const char *path) {
    (void)base;
    const long long start = sceKernelGetSystemTimeWide();
    return timed_call(OP_REMOVE, start, skiff_storage_remove(timed.inner, path));
}

static void timed_destroy(skiff_storage *base) { (void)base; }

static const skiff_storage_ops TIMED_STORAGE_OPS = {
    timed_open, timed_read,   timed_write,  timed_sync,    timed_close,
    timed_size, timed_rename, timed_remove, timed_destroy,
};

static void reset_storage_timing(void) { memset(timed.ops, 0, sizeof timed.ops); }

static void report_check(probe *p, int ok, const char *text) {
    skiff_probe_report_check(&p->report, ok, text);
}

static void report_line(probe *p, const char *text) { skiff_psp_report_line(&p->report, text); }

static long long now_us(void) { return sceKernelGetSystemTimeWide(); }

/* ---- Without ARK: the PSP storage on the emulated Memory Stick ---- */

static int expect(probe *p, const char *label, skiff_err got, skiff_err want) {
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "storage: %s: %s (want %s)", label, skiff_err_name(got),
             skiff_err_name(want));
    report_check(p, got == want, text);
    if (got != want) {
        const skiff_psp_storage_failure failure = skiff_psp_storage_last_failure(p->psp_storage);
        snprintf(text, sizeof text, "storage: last failed call %s = 0x%08X",
                 failure.call != NULL ? failure.call : "-", (unsigned)failure.sce_result);
        report_line(p, text);
    }
    return got == want;
}

static skiff_err write_blocks(probe *p, const char *path, const unsigned char *block) {
    skiff_file *file = NULL;
    skiff_err err = skiff_storage_open(p->storage, path, SKIFF_FILE_REPLACE, 0, &file);
    for (int i = 0; err == SKIFF_OK && i < STORAGE_BLOCKS; i++) {
        err = skiff_file_write(file, block, STORAGE_BLOCK_BYTES);
    }
    if (err == SKIFF_OK) {
        err = skiff_file_sync(file);
    }
    const skiff_err closed = skiff_file_close(file);
    return err != SKIFF_OK ? err : closed;
}

static skiff_err patch_middle(probe *p, const char *path) {
    skiff_file *file = NULL;
    skiff_err err =
        skiff_storage_open(p->storage, path, SKIFF_FILE_WRITE_AT, STORAGE_PATCH_OFFSET, &file);
    if (err == SKIFF_OK) {
        err = skiff_file_write(file, STORAGE_PATCH, sizeof STORAGE_PATCH - 1);
    }
    const skiff_err closed = skiff_file_close(file);
    return err != SKIFF_OK ? err : closed;
}

/* Reads the file back: the blocks, with the patch where it was written. */
static int contents_match(probe *p, const char *path, const unsigned char *block) {
    static unsigned char read_back[STORAGE_BLOCK_BYTES];
    skiff_file *file = NULL;
    if (skiff_storage_open(p->storage, path, SKIFF_FILE_READ, 0, &file) != SKIFF_OK) {
        return 0;
    }
    int same = 1;
    uint64_t offset = 0;
    size_t got = 0;
    do {
        if (skiff_file_read(file, read_back, sizeof read_back, &got) != SKIFF_OK) {
            same = 0;
            break;
        }
        for (size_t i = 0; i < got; i++, offset++) {
            const int patched = offset >= STORAGE_PATCH_OFFSET &&
                                offset < STORAGE_PATCH_OFFSET + sizeof STORAGE_PATCH - 1;
            const unsigned char want =
                patched ? (unsigned char)STORAGE_PATCH[offset - STORAGE_PATCH_OFFSET]
                        : block[offset % STORAGE_BLOCK_BYTES];
            same &= read_back[i] == want;
        }
    } while (got > 0);
    skiff_file_close(file);
    return same && offset == (uint64_t)STORAGE_BLOCKS * STORAGE_BLOCK_BYTES;
}

static int check_storage(probe *p) {
    static unsigned char block[STORAGE_BLOCK_BYTES];
    char path[PATH_MAX_LEN];
    char renamed[PATH_MAX_LEN];
    if (!skiff_probe_sibling(p->program_path, STORAGE_CHECK_FILE, path) ||
        !skiff_probe_sibling(p->program_path, STORAGE_CHECK_RENAMED, renamed)) {
        report_check(p, 0, "storage: no path next to the EBOOT");
        return 0;
    }
    for (size_t i = 0; i < sizeof block; i++) {
        block[i] = (unsigned char)(i * 7U + 3U);
    }
    skiff_file *file = NULL;
    uint64_t size = 0;
    int ok = expect(p, "write three blocks and sync", write_blocks(p, path, block), SKIFF_OK);
    ok &= expect(p, "size", skiff_storage_size(p->storage, path, &size), SKIFF_OK) &&
          size == (uint64_t)STORAGE_BLOCKS * STORAGE_BLOCK_BYTES;
    ok &= expect(p, "overwrite in the middle", patch_middle(p, path), SKIFF_OK);
    const int same = contents_match(p, path, block);
    report_check(p, same, "storage: read back, the rest of the file unchanged");
    ok &= same;
    ok &= expect(p, "write past the end",
                 skiff_storage_open(p->storage, path, SKIFF_FILE_WRITE_AT, size + 1, &file),
                 SKIFF_ERR_INVALID_ARG);
    ok &= expect(p, "path without a device",
                 skiff_storage_open(p->storage, STORAGE_CHECK_FILE, SKIFF_FILE_READ, 0, &file),
                 SKIFF_ERR_INVALID_ARG);
    ok &= expect(p, "create the rename target", write_blocks(p, renamed, block), SKIFF_OK);
    ok &= expect(p, "rename over an existing file", skiff_storage_rename(p->storage, path, renamed),
                 SKIFF_ERR_STORAGE_IO);
    ok &= expect(p, "remove", skiff_storage_remove(p->storage, renamed), SKIFF_OK);
    ok &= expect(p, "rename", skiff_storage_rename(p->storage, path, renamed), SKIFF_OK);
    ok &= expect(p, "old name gone", skiff_storage_size(p->storage, path, &size),
                 SKIFF_ERR_STORAGE_NOT_FOUND);
    ok &= expect(p, "remove the renamed file", skiff_storage_remove(p->storage, renamed), SKIFF_OK);
    ok &= expect(p, "remove a missing file", skiff_storage_remove(p->storage, renamed),
                 SKIFF_ERR_STORAGE_NOT_FOUND);
    return ok;
}

static int run_without_ark(probe *p) {
    report_line(p, "no ARK: TLS cannot start; checking the PSP storage, then that TLS refuses");
    int ok = check_storage(p);
    const CURLcode curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "curl_global_init() refuses without entropy: %d", (int)curl_status);
    report_check(p, curl_status != CURLE_OK, text);
    if (curl_status == CURLE_OK) {
        curl_global_cleanup();
    }
    return ok && curl_status != CURLE_OK;
}

/* ---- With ARK: downloads against the test RomM ---- */

static skiff_err stopped(watch *w, stop_reason reason, skiff_err err, long long now) {
    w->stop_reason = reason;
    w->detected_after_us = now - (w->last_byte_us != 0 ? w->last_byte_us : w->attempt_start_us);
    return err;
}

/* Ends the attempt as soon as the network is gone, the PSP suspended or the player quit. */
static skiff_err should_stop(void *ctx) {
    watch *w = ctx;
    const long long now = now_us();
    if (skiff_psp_exit_requested()) {
        return stopped(w, STOP_QUIT, SKIFF_ERR_NET_CONNECTION_LOST, now);
    }
    if (w->stop_at > 0 && w->done >= w->stop_at) {
        return stopped(w, STOP_BY_PROBE, SKIFF_ERR_NET_CONNECTION_LOST, now);
    }
    if (now - w->last_poll_us < NETWORK_POLL_US) {
        return SKIFF_OK;
    }
    w->last_poll_us = now;
    if (skiff_psp_power_events_now().suspends != w->power_at_attempt.suspends) {
        return stopped(w, STOP_SUSPEND, SKIFF_ERR_NET_CONNECTION_LOST, now);
    }
    const skiff_err online = skiff_psp_net_online(&w->p->net);
    if (online != SKIFF_OK) {
        return stopped(
            w, online == SKIFF_ERR_NET_UNAVAILABLE ? STOP_SWITCH_OFF : STOP_ACCESS_POINT_LOST,
            online, now);
    }
    return SKIFF_OK;
}

static void on_progress(void *ctx, uint64_t done, uint64_t total) {
    watch *w = ctx;
    const long long now = now_us();
    if (w->last_byte_us == 0) {
        if (now - w->attempt_start_us > w->first_byte_us) {
            w->first_byte_us = now - w->attempt_start_us;
        }
    } else if (now - w->last_byte_us > w->longest_gap_us) {
        w->longest_gap_us = now - w->last_byte_us;
    }
    w->last_byte_us = now;
    w->done = done;
    skiff_psp_keep_awake();
    if (w->prompt != NULL && !w->prompted && done >= w->prompt_at) {
        w->prompted = 1;
        report_line(w->p, w->prompt);
    }
    if (now - w->last_status_us >= STATUS_INTERVAL_US) {
        w->last_status_us = now;
        char status[SKIFF_SELFTEST_LINE_MAX];
        snprintf(status, sizeof status, "  %llu / %llu KB (%llu%%)",
                 (unsigned long long)(done / BYTES_PER_KB),
                 (unsigned long long)(total / BYTES_PER_KB),
                 (unsigned long long)(total > 0 ? done * PERCENT / total : 0));
        skiff_psp_report_status(&w->p->report, status);
    }
}

/* A fresh transport trusting the test CA, with the token: a new connection and TLS session. */
static int new_transport(probe *p) {
    skiff_transport_destroy(p->transport);
    p->transport = NULL;
    const skiff_http_header headers[] = {{"Authorization", p->authorization}};
    const skiff_curl_config config = {
        .ca_file = p->ca, .default_headers = headers, .default_header_count = 1};
    const skiff_err err = skiff_curl_transport_create(&config, &p->transport);
    if (err != SKIFF_OK) {
        char text[SKIFF_SELFTEST_LINE_MAX];
        snprintf(text, sizeof text, "transport: creating it failed: %s", skiff_err_name(err));
        report_check(p, 0, text);
    }
    return err == SKIFF_OK;
}

static int join(probe *p) {
    const long long start = now_us();
    const skiff_err err = skiff_psp_net_connect(&p->net, p->config.profile, WIFI_JOIN_TIMEOUT_US);
    char text[SKIFF_SELFTEST_LINE_MAX];
    if (err != SKIFF_OK) {
        skiff_probe_report_net_failure(&p->report, &p->net, err);
        return 0;
    }
    char ip[SKIFF_PSP_NET_IP_MAX] = "";
    skiff_psp_net_ip(&p->net, ip, sizeof ip);
    snprintf(text, sizeof text, "Wi-Fi: profile %d joined in %lld ms, IP %s", p->config.profile,
             (now_us() - start) / US_PER_MS, ip);
    report_check(p, 1, text);
    return 1;
}

/* Waits for the player to turn the Wi-Fi switch back on; 0 when wait_s passes first. */
static int wait_for_switch(probe *p) {
    if (skiff_psp_net_online(&p->net) != SKIFF_ERR_NET_UNAVAILABLE) {
        return 1;
    }
    report_line(p, "ACTION: turn the Wi-Fi switch back ON");
    const long long deadline = now_us() + p->config.wait_s * US_PER_S;
    while (now_us() < deadline && !skiff_psp_exit_requested()) {
        if (skiff_psp_net_online(&p->net) != SKIFF_ERR_NET_UNAVAILABLE) {
            return 1;
        }
        sceKernelDelayThread((SceUInt)WAIT_POLL_US);
    }
    report_check(p, 0, "Wi-Fi: the switch stayed off");
    return 0;
}

/*
 * After an interruption: the switch back on, then rejoin the profile; if that fails, reload the
 * network modules and join again. Always a new transport: the old connection is dead.
 */
static int recover(probe *p, outcome *o) {
    const long long start = now_us();
    if (!wait_for_switch(p)) {
        return 0;
    }
    const skiff_psp_power_events power = skiff_psp_power_events_now();
    o->cpu_mhz_after = scePowerGetCpuClockFrequency();
    char text[LONG_LINE_MAX];
    snprintf(text, sizeof text,
             "after the interruption: clock %d/%d MHz, access point state %d, suspends %d, "
             "resumes %d, last power event 0x%08X",
             o->cpu_mhz_after, scePowerGetBusClockFrequency(), p->net.apctl_state, power.suspends,
             power.resumes, (unsigned)power.last_info);
    report_line(p, text);
    o->recovery = PATH_RECONNECT;
    if (skiff_psp_net_disconnect(&p->net, DISCONNECT_TIMEOUT_US) != SKIFF_OK || !join(p)) {
        report_line(p, "rejoining failed: reloading the network modules");
        o->recovery = PATH_RELOAD;
        if (!skiff_probe_tear_down(&p->report, &p->net, DISCONNECT_TIMEOUT_US) ||
            !skiff_probe_load_network(&p->report, &p->net, SKIFF_PSP_NET_CPU_MHZ) || !join(p)) {
            return 0;
        }
    }
    o->recovery_us = now_us() - start;
    snprintf(text, sizeof text, "recovered in %lld ms: %s", o->recovery_us / US_PER_MS,
             o->recovery);
    report_check(p, 1, text);
    return new_transport(p);
}

static int is_storage_error(skiff_err err) {
    return err >= SKIFF_ERR_STORAGE_NO_MEDIA && err <= SKIFF_ERR_STORAGE_FILE_TOO_LARGE;
}

static void log_attempt(probe *p, const watch *w, const outcome *o, skiff_err err,
                        const skiff_download_result *result) {
    char text[LONG_LINE_MAX];
    const skiff_psp_storage_failure failure = skiff_psp_storage_last_failure(p->psp_storage);
    int used = snprintf(
        text, sizeof text, "attempt %d: %s, HTTP %ld, from %llu, +%llu bytes%s, %s %s", o->attempts,
        skiff_err_name(err), result->response.status, (unsigned long long)result->resumed_from,
        (unsigned long long)result->bytes_received, result->restarted ? " (restarted)" : "",
        result->response.tls_version, result->response.tls_cipher);
    if (w->stop_reason != STOP_NONE && used > 0 && (size_t)used < sizeof text) {
        used += snprintf(text + used, sizeof text - (size_t)used,
                         "; stopped: %s, %lld ms after the last byte",
                         STOP_REASON_NAMES[w->stop_reason], w->detected_after_us / US_PER_MS);
    }
    if (is_storage_error(err) && used > 0 && (size_t)used < sizeof text) {
        snprintf(text + used, sizeof text - (size_t)used, "; %s = 0x%08X",
                 failure.call != NULL ? failure.call : "-", (unsigned)failure.sce_result);
    }
    report_check(p, err == SKIFF_OK, text);
}

/* Runs attempts until the file is complete, recovering after each interruption. */
static skiff_err download(probe *p, watch *w, outcome *o) {
    skiff_download_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.url = p->url;
    spec.target_path = p->target;
    spec.expected_size = p->config.size;
    spec.has_expected_crc32 = 1;
    spec.expected_crc32 = p->config.crc32;
    spec.should_stop = should_stop;
    spec.stop_ctx = w;
    spec.on_progress = on_progress;
    spec.progress_ctx = w;
    const long long start = now_us();
    skiff_err err = SKIFF_ERR_NET_CONNECTION_LOST;
    while (o->attempts < MAX_ATTEMPTS && !o->complete) {
        o->attempts++;
        w->stop_reason = STOP_NONE;
        w->power_at_attempt = skiff_psp_power_events_now();
        w->attempt_start_us = now_us();
        w->last_byte_us = 0;
        skiff_download_result result;
        err = skiff_download_attempt(p->transport, p->storage, &spec, &result);
        log_attempt(p, w, o, err, &result);
        o->resumed_from = result.resumed_from;
        o->last_status = result.response.status;
        o->restarted |= result.restarted;
        o->complete = result.complete;
        if (err == SKIFF_OK || w->stop_reason == STOP_QUIT) {
            break;
        }
        const int suspended = skiff_psp_power_events_now().suspends != w->power_at_attempt.suspends;
        if (w->stop_reason == STOP_BY_PROBE) {
            w->stop_at = 0;
            if (!new_transport(p)) {
                break;
            }
            continue;
        }
        if (!skiff_download_retryable(err) && !(suspended && is_storage_error(err))) {
            break;
        }
        if (o->interruptions++ == 0) {
            o->first_interruption = err;
            o->first_reason = w->stop_reason;
            o->first_from_transport = w->stop_reason == STOP_NONE;
            o->detected_after_us = w->detected_after_us;
        }
        if (!recover(p, o)) {
            break;
        }
    }
    o->elapsed_us = now_us() - start;
    o->bytes = p->config.size;
    return err;
}

static void watch_init(watch *w, probe *p, const char *prompt) {
    memset(w, 0, sizeof *w);
    w->p = p;
    w->prompt = prompt;
    w->prompt_at = p->config.size * PROMPT_AT_PERCENT / PERCENT;
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

/* One line on screen, in result.txt and in resume-log.txt. */
static int report_scenario(probe *p, const char *name, int ok, const watch *w, const outcome *o,
                           const char *note) {
    char text[LONG_LINE_MAX];
    snprintf(text, sizeof text,
             "scenario=%s ok=%d attempts=%d interruptions=%d first=%s reason=%s detected_ms=%lld "
             "recovery=%s recovery_ms=%lld clock_after=%d resumed_from=%llu status=%ld "
             "restarted=%d complete=%d first_byte_ms=%lld longest_gap_ms=%lld total_ms=%lld "
             "kb_s=%llu %s",
             name, ok, o->attempts, o->interruptions,
             o->interruptions > 0 ? skiff_err_name(o->first_interruption) : "-",
             o->first_from_transport ? "transport" : STOP_REASON_NAMES[o->first_reason],
             o->detected_after_us / US_PER_MS, o->recovery != NULL ? o->recovery : "-",
             o->recovery_us / US_PER_MS, o->cpu_mhz_after, (unsigned long long)o->resumed_from,
             o->last_status, o->restarted, o->complete, w->first_byte_us / US_PER_MS,
             w->longest_gap_us / US_PER_MS, o->elapsed_us / US_PER_MS,
             skiff_probe_kb_per_s(o->bytes, o->elapsed_us), note);
    report_check(p, ok, text);
    const int logged = append_log(p, text);
    /* Where the Memory Stick's time went in this scenario, then start counting afresh. */
    int used = snprintf(text, sizeof text, "storage %s:", name);
    for (int op = 0; op < STORAGE_OPS && used > 0 && (size_t)used < sizeof text; op++) {
        const op_timing *timing = &timed.ops[op];
        if (timing->count > 0) {
            used += snprintf(text + used, sizeof text - (size_t)used, " %s=%dx/%lldms/max%lldms",
                             STORAGE_OP_NAMES[op], timing->count, timing->total_us / US_PER_MS,
                             timing->max_us / US_PER_MS);
        }
    }
    report_line(p, text);
    reset_storage_timing();
    return append_log(p, text) && logged && ok;
}

static int resumed_with_206(const outcome *o) {
    return o->complete && o->resumed_from > 0 && o->last_status == HTTP_STATUS_PARTIAL &&
           !o->restarted;
}

/* A .resume file left by an earlier run: finish that download first. */
static int run_leftover(probe *p) {
    char state[PATH_MAX_LEN + sizeof SKIFF_DOWNLOAD_STATE_SUFFIX];
    uint64_t size = 0;
    snprintf(state, sizeof state, "%s" SKIFF_DOWNLOAD_STATE_SUFFIX, p->target);
    if (skiff_storage_size(p->storage, state, &size) != SKIFF_OK) {
        return 1;
    }
    report_line(p, "relaunch: a download from an earlier run was left unfinished; resuming it");
    watch w;
    outcome o;
    watch_init(&w, p, NULL);
    memset(&o, 0, sizeof o);
    download(p, &w, &o);
    /* An earlier run that got every byte but quit before the rename is finished without a request.
     */
    const int finished_locally =
        o.complete && o.resumed_from == p->config.size && o.last_status == 0;
    return report_scenario(p, "relaunch", resumed_with_206(&o) || finished_locally, &w, &o,
                           "(resumed across launches)");
}

static int run_interrupted(probe *p, const char *name, const char *prompt, uint64_t stop_at) {
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "-- scenario %s", name);
    report_line(p, text);
    skiff_download_discard(p->storage, p->target);
    watch w;
    outcome o;
    watch_init(&w, p, prompt);
    w.stop_at = stop_at;
    memset(&o, 0, sizeof o);
    const skiff_psp_power_events before = skiff_psp_power_events_now();
    download(p, &w, &o);
    const int suspended = skiff_psp_power_events_now().suspends != before.suspends;
    int ok = resumed_with_206(&o);
    const char *note = "";
    if (stop_at > 0) {
        ok &= o.attempts == 2 && o.resumed_from >= stop_at;
    } else if (strcmp(name, "suspend") == 0) {
        ok &= suspended && o.interruptions > 0;
        note = suspended ? "" : "(no suspend seen: was the PSP put to sleep in time?)";
    } else {
        ok &= o.interruptions > 0;
        note = o.interruptions > 0 ? "" : "(no interruption seen: was the switch turned off?)";
    }
    return report_scenario(p, name, ok, &w, &o, note);
}

/* The HOME menu is not an interruption: the download must simply finish. */
static int run_home(probe *p) {
    report_line(p, "-- scenario home");
    skiff_download_discard(p->storage, p->target);
    watch w;
    outcome o;
    watch_init(&w, p, "ACTION: press HOME, wait 10 s on the menu, then go back (do not quit)");
    memset(&o, 0, sizeof o);
    download(p, &w, &o);
    return report_scenario(p, "home", o.complete, &w, &o, "(longest_gap shows any pause)");
}

/* Adds one download's figures to the scenario's: counts and times summed, the worst waits kept. */
static void add_download(watch *total_w, outcome *total, const watch *w, const outcome *o) {
    total->attempts += o->attempts;
    total->interruptions += o->interruptions;
    total->complete = o->complete;
    total->last_status = o->last_status;
    total->elapsed_us += o->elapsed_us;
    total->bytes += o->bytes;
    if (w->first_byte_us > total_w->first_byte_us) {
        total_w->first_byte_us = w->first_byte_us;
    }
    if (w->longest_gap_us > total_w->longest_gap_us) {
        total_w->longest_gap_us = w->longest_gap_us;
    }
}

/* Downloads back to back for awake_s with no input: keep-awake must hold off Auto Sleep. */
static int run_sleep(probe *p) {
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text,
             "-- scenario sleep: do not touch the PSP for %d s (Auto Sleep at its shortest)",
             p->config.awake_s);
    report_line(p, text);
    const skiff_psp_power_events before = skiff_psp_power_events_now();
    const long long deadline = now_us() + p->config.awake_s * US_PER_S;
    watch total_w;
    outcome total;
    watch_init(&total_w, p, NULL);
    memset(&total, 0, sizeof total);
    int downloads = 0;
    int all_complete = 1;
    while (now_us() < deadline && all_complete && !skiff_psp_exit_requested()) {
        skiff_download_discard(p->storage, p->target);
        watch w;
        outcome o;
        watch_init(&w, p, NULL);
        memset(&o, 0, sizeof o);
        download(p, &w, &o);
        add_download(&total_w, &total, &w, &o);
        all_complete &= o.complete;
        downloads++;
    }
    const int suspends = skiff_psp_power_events_now().suspends - before.suspends;
    snprintf(text, sizeof text, "(%d downloads, %d suspend(s) during the scenario)", downloads,
             suspends);
    return report_scenario(p, "sleep", all_complete && suspends == 0, &total_w, &total, text);
}

/* ---- speed: where a download's time goes ---- */

/*
 * The Memory Stick alone: the seeded file's size written in the download's blocks from data,
 * synced at every checkpoint as the download does, with the write time of each quarter of the file.
 */
static int speed_memory_stick(probe *p, const char *name, const unsigned char *data) {
    const uint64_t size = p->config.size;
    long long part_us[SPEED_PARTS] = {0};
    long long sync_us = 0;
    skiff_file *file = NULL;
    const long long start = now_us();
    skiff_err err = skiff_storage_open(p->psp_storage, p->target, SKIFF_FILE_REPLACE, 0, &file);
    uint64_t written = 0;
    while (err == SKIFF_OK && written < size) {
        const size_t take =
            size - written < SPEED_BLOCK_BYTES ? (size_t)(size - written) : SPEED_BLOCK_BYTES;
        const long long write_start = now_us();
        err = skiff_file_write(file, data, take);
        part_us[written * SPEED_PARTS / size] += now_us() - write_start;
        written += take;
        if (err == SKIFF_OK && written % SKIFF_DOWNLOAD_CHECKPOINT_BYTES == 0) {
            const long long sync_start = now_us();
            err = skiff_file_sync(file);
            sync_us += now_us() - sync_start;
        }
    }
    if (file != NULL) {
        const skiff_err closed = skiff_file_close(file);
        err = err != SKIFF_OK ? err : closed;
    }
    const long long elapsed_us = now_us() - start;
    skiff_storage_remove(p->psp_storage, p->target);
    char environment[SKIFF_PROBE_ENVIRONMENT_MAX];
    skiff_probe_describe_environment(1, environment, sizeof environment);
    char text[LONG_LINE_MAX];
    snprintf(text, sizeof text,
             "speed=%s ok=%d err=%s bytes=%llu total_ms=%lld kb_s=%llu sync_ms=%lld "
             "quarters_write_ms=%lld/%lld/%lld/%lld %s",
             name, err == SKIFF_OK, skiff_err_name(err), (unsigned long long)written,
             elapsed_us / US_PER_MS, skiff_probe_kb_per_s(written, elapsed_us), sync_us / US_PER_MS,
             part_us[0] / US_PER_MS, part_us[1] / US_PER_MS, part_us[2] / US_PER_MS,
             part_us[3] / US_PER_MS, environment);
    report_check(p, err == SKIFF_OK, text);
    return append_log(p, text) && err == SKIFF_OK;
}

/* The network alone: the body's CRC-32 is computed and nothing is written. */
typedef struct speed_sink {
    /* The download's hooks run on every chunk when set, as they do for a real download. */
    watch *w;
    long long start_us;
    long long first_byte_us;
    uint64_t bytes;
    uLong crc32;
} speed_sink;

static skiff_err speed_body(void *ctx, const unsigned char *data, size_t size) {
    speed_sink *sink = ctx;
    if (sink->bytes == 0) {
        sink->first_byte_us = now_us() - sink->start_us;
    }
    sink->crc32 = crc32(sink->crc32, data, (uInt)size);
    sink->bytes += size;
    if (sink->w != NULL) {
        on_progress(sink->w, sink->bytes, sink->w->p->config.size);
    }
    return SKIFF_OK;
}

static int speed_network(probe *p, const char *name, int with_hooks) {
    watch w;
    outcome o;
    watch_init(&w, p, NULL);
    memset(&o, 0, sizeof o);
    speed_sink sink = {.w = with_hooks ? &w : NULL, .crc32 = crc32(0L, Z_NULL, 0)};
    skiff_http_request request = {.url = p->url, .on_body = speed_body, .body_ctx = &sink};
    if (with_hooks) {
        request.should_stop = should_stop;
        request.stop_ctx = &w;
    }
    skiff_http_response response;
    memset(&response, 0, sizeof response);
    sink.start_us = now_us();
    w.attempt_start_us = sink.start_us;
    const skiff_err err = skiff_transport_perform(p->transport, &request, &response);
    o.elapsed_us = now_us() - sink.start_us;
    o.attempts = 1;
    o.bytes = sink.bytes;
    o.last_status = response.status;
    o.complete = err == SKIFF_OK && response.status == HTTP_STATUS_OK &&
                 sink.bytes == p->config.size && sink.crc32 == p->config.crc32;
    w.first_byte_us = sink.first_byte_us;
    char environment[SKIFF_PROBE_ENVIRONMENT_MAX];
    skiff_probe_describe_environment(1, environment, sizeof environment);
    return report_scenario(p, name, o.complete, &w, &o, environment);
}

/* A whole download through the engine; with bounce, every write is first copied there. */
static int speed_download(probe *p, const char *name, unsigned char *bounce) {
    skiff_download_discard(p->storage, p->target);
    watch w;
    outcome o;
    watch_init(&w, p, NULL);
    memset(&o, 0, sizeof o);
    timed.bounce = bounce;
    download(p, &w, &o);
    timed.bounce = NULL;
    skiff_storage_remove(p->storage, p->target);
    char environment[SKIFF_PROBE_ENVIRONMENT_MAX];
    skiff_probe_describe_environment(1, environment, sizeof environment);
    return report_scenario(p, name, o.complete && o.attempts == 1, &w, &o, environment);
}

static int run_speed(probe *p) {
    report_line(p, "-- scenario speed: where a download's time goes (about 15 min, no action)");
    unsigned char *area = memalign(MS_ALIGNMENT, SPEED_BLOCK_BYTES + MS_ALIGNMENT);
    if (area == NULL) {
        report_check(p, 0, "speed: no memory for the test block");
        return 0;
    }
    for (size_t i = 0; i < SPEED_BLOCK_BYTES + MS_ALIGNMENT; i++) {
        area[i] = (unsigned char)(i * 7U + 3U);
    }
    /* The engine's write buffer comes from malloc(): show where such a block lands. */
    void *like_the_engine = malloc(SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES);
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text,
             "speed: malloc(%d KB) returned a block %u bytes past a %d-byte boundary",
             SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES / BYTES_PER_KB,
             (unsigned)((uintptr_t)like_the_engine % MS_ALIGNMENT), MS_ALIGNMENT);
    report_line(p, text);
    free(like_the_engine);
    int ok = speed_memory_stick(p, "ms-aligned", area);
    ok &= speed_memory_stick(p, "ms-unaligned", area + MALLOC_ALIGNMENT);
    ok &= speed_network(p, "network", 0);
    ok &= speed_network(p, "network-hooks", 1);
    ok &= speed_download(p, "download", NULL);
    ok &= speed_download(p, "download-aligned", area);
    free(area);
    return ok;
}

static int run_scenarios(probe *p) {
    const unsigned chosen = p->config.scenarios;
    reset_storage_timing();
    int ok = run_leftover(p);
    if ((chosen & SKIFF_PROBE_SCENARIO_RESTART) != 0) {
        ok &= run_interrupted(p, "restart", NULL, p->config.size * RESTART_AT_PERCENT / PERCENT);
    }
    if ((chosen & SKIFF_PROBE_SCENARIO_WIFI) != 0) {
        ok &= run_interrupted(p, "wifi",
                              "ACTION: turn the Wi-Fi switch OFF now (you will be asked to turn "
                              "it back on)",
                              0);
    }
    if ((chosen & SKIFF_PROBE_SCENARIO_SUSPEND) != 0) {
        ok &= run_interrupted(p, "suspend",
                              "ACTION: slide POWER down to sleep, wait 5 s, then slide it again "
                              "to wake the PSP",
                              0);
    }
    if ((chosen & SKIFF_PROBE_SCENARIO_HOME) != 0) {
        ok &= run_home(p);
    }
    if ((chosen & SKIFF_PROBE_SCENARIO_SLEEP) != 0) {
        ok &= run_sleep(p);
    }
    if ((chosen & SKIFF_PROBE_SCENARIO_SPEED) != 0) {
        ok &= run_speed(p);
    }
    skiff_download_discard(p->storage, p->target);
    skiff_storage_remove(p->storage, p->target);
    return ok;
}

/* resume-probe.ini: the server and the seeded file are required. */
static int read_config(probe *p) {
    if (!skiff_probe_load_config(&p->report, p->program_path, PROBE_CONFIG_FILE, &p->config)) {
        return 0;
    }
    const skiff_probe_config *config = &p->config;
    snprintf(p->authorization, sizeof p->authorization, BEARER_PREFIX "%s", config->token);
    snprintf(p->url, sizeof p->url, "https://%s:%d/api/roms/%s/content/%s", config->host,
             HTTPS_PORT, config->rom_id, config->file_name);
    const int ok = skiff_probe_config_complete(config) &&
                   skiff_probe_sibling(p->program_path, TARGET_FILE, p->target) &&
                   skiff_probe_existing_sibling(&p->report, p->program_path, CA_FILE, p->ca);
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text,
             "config: server %s, profile %d, ROM %s (%llu bytes), scenarios 0x%x, wait %d s, "
             "awake %d s",
             config->host[0] != '\0' ? config->host : "(missing)", config->profile,
             config->rom_id[0] != '\0' ? config->rom_id : "(missing)", config->size,
             config->scenarios, config->wait_s, config->awake_s);
    report_check(p, ok, text);
    return ok;
}

static int run_with_ark(probe *p) {
    if (!read_config(p)) {
        return 0;
    }
    const skiff_psp_power_events power = skiff_psp_power_events_now();
    char text[SKIFF_SELFTEST_LINE_MAX];
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
    const CURLcode curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    snprintf(text, sizeof text, "curl_global_init() = %d", (int)curl_status);
    report_check(p, curl_status == CURLE_OK, text);
    int ok = curl_status == CURLE_OK && new_transport(p) && run_scenarios(p);
    skiff_transport_destroy(p->transport);
    p->transport = NULL;
    if (curl_status == CURLE_OK) {
        curl_global_cleanup();
    }
    ok &= skiff_probe_tear_down(&p->report, &p->net, DISCONNECT_TIMEOUT_US);
    return ok;
}

int main(int argc, char *argv[]) {
    static probe p;
    memset(&p, 0, sizeof p);
    p.program_path = argc > 0 ? argv[0] : NULL;
    skiff_psp_install_callbacks();
    skiff_psp_report_open(&p.report, p.program_path);
    skiff_psp_report_line(&p.report, "Skiff resume probe");

    const int has_ark = skiff_psp_entropy_status() != SKIFF_ERR_NET_NEEDS_ARK;
    int passed = skiff_psp_storage_create(&p.psp_storage) == SKIFF_OK;
    timed.base.ops = &TIMED_STORAGE_OPS;
    timed.inner = p.psp_storage;
    p.storage = &timed.base;
    if (passed) {
        passed = has_ark ? run_with_ark(&p) : run_without_ark(&p);
    }
    skiff_storage_destroy(p.psp_storage);
    const char *marker = has_ark ? (passed ? PROBE_OK_MARKER : PROBE_FAIL_MARKER)
                                 : (passed ? PROBE_NO_ARK_OK_MARKER : PROBE_NO_ARK_FAIL_MARKER);
    skiff_psp_report_line(&p.report, marker);
    skiff_psp_report_close(&p.report);
    sceKernelExitGame();
    return passed ? 0 : 1;
}
