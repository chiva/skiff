/*
 * Benchmark (Phase 1 hardware spike, W6): where a download's time goes on a PSP. Skiff's transport
 * fetched the test RomM's file at 193 KB/s (T1), so a 700 MB game takes an hour. This EBOOT
 * separates what the Wi-Fi, the CPU (TLS) and the Memory Stick each cost, and measures the settings
 * the app can change:
 *
 *   - latency: TCP connects right after joining and later, and requests on a kept connection after
 *     idle gaps, to tell lost SYNs (retried after 3 s) apart from Wi-Fi power save;
 *   - cpu: CRC-32, MD5 and SHA-1 (the hashes RomM records) and ChaCha20-Poly1305 and AES-128-GCM
 *     (the TLS ciphers) on the PSP's CPU, for the integrity check and the cipher preference;
 *   - net: the seeded file by curl buffer size, socket receive buffer, TLS version and cipher, over
 *     plain HTTP, and at 333 MHz. A lowest-priority thread counts idle time, so every download also
 *     reports how busy the CPU was: near 100% means TLS is the limit, low means the Wi-Fi is;
 *   - ms: Memory Stick write and read speed by block size, and a download written to it as it
 *     arrives;
 *   - app: the download through Skiff's own transport, as T1 measured it.
 *
 * scripts/memstick.sh install writes bench.ini (the test server, the seeded file, and optional
 * runs= and sections=, see tests/hardware/probe_support.h) and copies the test CA next to the
 * EBOOT. Results go to result.txt; each measurement also appends one line to bench-log.txt, tagged
 * with the clock, power source, Wi-Fi power save and signal, so runs can be compared later.
 *
 * Without ARK (PPSSPP in CI) TLS cannot start. The network modules load and unload, libcurl must
 * refuse to start, and the CRC-32 and Memory Stick sections run on small sizes to check the code
 * (the emulator's numbers mean nothing); the probe then ends with SKIFF BENCH NO ARK OK.
 */
#include <curl/curl.h>
#include <malloc.h>
#include <psa/crypto.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_devctl.h>
#include <pspkernel.h>
#include <pspnet_apctl.h>
#include <psppower.h>
#include <pspthreadman.h>
#include <psputility.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <zlib.h>

#include "skiff/curl_transport.h"
#include "skiff/selftest.h"

#include "kirk_entropy.h"
#include "lifecycle.h"
#include "net_psp.h"
#include "probe_psp.h"
#include "probe_support.h"
#include "report.h"

#define BENCH_OK_MARKER "SKIFF BENCH OK"
#define BENCH_FAIL_MARKER "SKIFF BENCH FAIL"
#define BENCH_NO_ARK_OK_MARKER "SKIFF BENCH NO ARK OK"
#define BENCH_NO_ARK_FAIL_MARKER "SKIFF BENCH NO ARK FAIL"
#define BENCH_CONFIG_FILE "bench.ini"
#define BENCH_LOG_FILE "bench-log.txt"
/* Written and deleted again by the Memory Stick section, next to the EBOOT. */
#define MS_TEST_FILE "bench-ms.bin"
#define MS_DEVICE "ms0:"
/* Copied next to the EBOOT by scripts/memstick.sh, from build/integration/certs/. */
#define CA_FILE "ca.crt"
#define HEARTBEAT_PATH "/api/heartbeat"
#define AUTHORIZATION_HEADER "Authorization"
#define BEARER_PREFIX "Bearer "
/* What libcurl takes to force a cipher (IANA names for TLS 1.3, OpenSSL names for TLS 1.2), and a
 * part of the name Mbed TLS then reports for the suite. The test server's certificate is ECDSA. */
#define TLS13_AES_128_GCM "TLS_AES_128_GCM_SHA256"
#define TLS13_CHACHA20 "TLS_CHACHA20_POLY1305_SHA256"
#define TLS12_ECDSA_AES_128_GCM "ECDHE-ECDSA-AES128-GCM-SHA256"
#define TLS12_ECDSA_CHACHA20 "ECDHE-ECDSA-CHACHA20-POLY1305"
#define SUITE_AES_128_GCM "AES-128-GCM"
#define SUITE_CHACHA20 "CHACHA20"
#define TLS_1_2 "TLSv1.2"
#define TLS_1_3 "TLSv1.3"
#define TLS_1_2_ONLY (CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2)

#define WIFI_JOIN_TIMEOUT_US (30LL * 1000 * 1000)
#define DISCONNECT_TIMEOUT_US (10LL * 1000 * 1000)
/* A thread's end is awaited this long before the benchmark gives up on it. */
#define THREAD_END_TIMEOUT_US 2000000U
/* FNV-1a, as the network probe's body callback ran it per byte when T1 measured 193 KB/s. */
#define FNV_OFFSET_BASIS 2166136261U
#define FNV_PRIME 16777619U

enum {
    KB = 1024,
    MIB = 1024 * 1024,
    US_PER_MS = 1000,
    US_PER_S = 1000 * 1000,
    MS_PER_S = 1000,
    PERCENT = 100,
    TENTHS = 10,
    HTTP_PORT = 8080,
    HTTPS_PORT = 8443,
    HTTP_OK = 200,
    URL_MAX = 320,
    BEARER_MAX = SKIFF_PROBE_TOKEN_MAX + sizeof BEARER_PREFIX,
    AUTHORIZATION_MAX = BEARER_MAX + sizeof AUTHORIZATION_HEADER + 1,
    ENVIRONMENT_MAX = 160,
    LINE_MAX_BENCH = SKIFF_PROBE_LONG_LINE_MAX,
    CONNECT_TIMEOUT_S = 10,
    STALL_TIMEOUT_S = 30,
    STALL_BYTES_PER_S = 1,
    /* Latency: back-to-back connects right after joining, then one after each pause. */
    IMMEDIATE_CONNECTS = 5,
    /* curl reports a connect as slow as a retried SYN from here (BSD stacks retry after 3 s). */
    SYN_RETRY_SUSPECT_MS = 2900,
    /* CPU: a buffer hashed or decrypted in TLS-record-sized chunks, passes times. */
    CPU_BUFFER_BYTES = MIB,
    CPU_CHUNK_BYTES = 16 * KB,
    CPU_PASSES = 8,
    CPU_SMOKE_PASSES = 1,
    AEAD_NONCE_BYTES = 12,
    AEAD_TAG_BYTES = 16,
    AES_128_KEY_BITS = 128,
    CHACHA20_KEY_BITS = 256,
    KEY_BYTES_MAX = 32,
    KEY_FILL_BYTE = 0x5A,
    BITS_PER_BYTE = 8,
    HASH_MAX_BYTES = 20,
    /* Memory Stick: one file of this size per block size, written then read back. */
    MS_FILE_BYTES = 16 * MIB,
    MS_SMOKE_FILE_BYTES = 2 * MIB,
    MS_ALIGNMENT = 64,
    MS_HEADROOM_BYTES = MIB,
    /* A download written as it arrives goes out in blocks of this size. */
    MS_SINK_BYTES = 128 * KB,
    /* The idle counter runs below every other thread; it pauses briefly now and then so it can
     * never starve a system thread at the same priority. */
    IDLE_THREAD_PRIORITY = 0x6F,
    IDLE_THREAD_STACK_BYTES = 0x1000,
    IDLE_SPIN = 256,
    IDLE_TICKS_PER_PAUSE = 1024,
    IDLE_PAUSE_US = 100,
    IDLE_CALIBRATION_US = 500 * 1000,
    /* Keeps auto-sleep and the backlight timer from ending a long run. */
    TICKER_THREAD_PRIORITY = 0x18,
    TICKER_THREAD_STACK_BYTES = 0x1000,
    TICKER_INTERVAL_US = 1000 * 1000,
    CLOCK_FAST_CPU_MHZ = 333,
    CLOCK_FAST_BUS_MHZ = 166,
    /* For the time estimates printed before each section. */
    ASSUMED_KB_PER_S = 200,
};

static const long LATENCY_PAUSES_MS[] = {2000, 5000, 10000};
static const long IDLE_GAPS_MS[] = {0, 500, 2000, 5000, 15000};
static const int MS_BLOCK_BYTES[] = {16 * KB, 32 * KB, 64 * KB, 128 * KB, 256 * KB, 512 * KB};
static const int MS_SMOKE_BLOCK_BYTES[] = {16 * KB, 512 * KB};

/* One way of downloading the seeded file. Zero fields keep curl's and the system's defaults. */
typedef struct download_spec {
    const char *id; /* in bench-log.txt */
    const char *label;
    int https;
    long buffer_bytes;         /* CURLOPT_BUFFERSIZE (curl's default is 16 KB) */
    int receive_buffer_bytes;  /* SO_RCVBUF on the socket */
    long ssl_version;          /* CURLOPT_SSLVERSION */
    const char *tls13_ciphers; /* CURLOPT_TLS13_CIPHERS */
    const char *cipher_list;   /* CURLOPT_SSL_CIPHER_LIST, TLS 1.2 */
    const char *expect_version;
    const char *expect_suite; /* part of the negotiated suite's name */
    int to_memory_stick;
    /* One of the curl buffer sizes compared to choose the best for the Memory Stick download. */
    int buffer_candidate;
} download_spec;

static const download_spec NET_SPECS[] = {
    {.id = "https-buf16k",
     .label = "HTTPS, curl buffer 16 KB",
     .https = 1,
     .buffer_bytes = 16 * KB,
     .buffer_candidate = 1},
    {.id = "https-buf64k",
     .label = "HTTPS, curl buffer 64 KB",
     .https = 1,
     .buffer_bytes = 64 * KB,
     .buffer_candidate = 1},
    {.id = "https-buf128k",
     .label = "HTTPS, curl buffer 128 KB",
     .https = 1,
     .buffer_bytes = 128 * KB,
     .buffer_candidate = 1},
    {.id = "https-buf256k",
     .label = "HTTPS, curl buffer 256 KB",
     .https = 1,
     .buffer_bytes = 256 * KB,
     .buffer_candidate = 1},
    {.id = "https-buf512k",
     .label = "HTTPS, curl buffer 512 KB",
     .https = 1,
     .buffer_bytes = 512 * KB,
     .buffer_candidate = 1},
    {.id = "http-buf16k", .label = "plain HTTP, curl buffer 16 KB", .buffer_bytes = 16 * KB},
    {.id = "http-buf512k", .label = "plain HTTP, curl buffer 512 KB", .buffer_bytes = 512 * KB},
    {.id = "https-rcvbuf32k",
     .label = "HTTPS, SO_RCVBUF 32 KB",
     .https = 1,
     .receive_buffer_bytes = 32 * KB},
    {.id = "https-rcvbuf64k",
     .label = "HTTPS, SO_RCVBUF 64 KB",
     .https = 1,
     .receive_buffer_bytes = 64 * KB},
    {.id = "https-rcvbuf128k",
     .label = "HTTPS, SO_RCVBUF 128 KB",
     .https = 1,
     .receive_buffer_bytes = 128 * KB},
    {.id = "tls13-aes128gcm",
     .label = "TLS 1.3, AES-128-GCM",
     .https = 1,
     .tls13_ciphers = TLS13_AES_128_GCM,
     .expect_version = TLS_1_3,
     .expect_suite = SUITE_AES_128_GCM},
    {.id = "tls13-chacha20",
     .label = "TLS 1.3, ChaCha20-Poly1305",
     .https = 1,
     .tls13_ciphers = TLS13_CHACHA20,
     .expect_version = TLS_1_3,
     .expect_suite = SUITE_CHACHA20},
    {.id = "tls12-aes128gcm",
     .label = "TLS 1.2, ECDHE-ECDSA AES-128-GCM",
     .https = 1,
     .ssl_version = TLS_1_2_ONLY,
     .cipher_list = TLS12_ECDSA_AES_128_GCM,
     .expect_version = TLS_1_2,
     .expect_suite = SUITE_AES_128_GCM},
    {.id = "tls12-chacha20",
     .label = "TLS 1.2, ECDHE-ECDSA ChaCha20-Poly1305",
     .https = 1,
     .ssl_version = TLS_1_2_ONLY,
     .cipher_list = TLS12_ECDSA_CHACHA20,
     .expect_version = TLS_1_2,
     .expect_suite = SUITE_CHACHA20},
};

/* Run at 333 MHz: if TLS is what limits the speed, HTTPS gets faster and plain HTTP does not. */
static const download_spec FAST_CLOCK_SPECS[] = {
    {.id = "https-buf16k-333mhz", .label = "HTTPS at 333 MHz", .https = 1, .buffer_bytes = 16 * KB},
    {.id = "http-buf16k-333mhz", .label = "plain HTTP at 333 MHz", .buffer_bytes = 16 * KB},
};

typedef struct bench {
    skiff_psp_report report;
    const char *program_path;
    skiff_probe_config config;
    FILE *log;
    int has_ark;
    /* PPSSPP without ARK: small sizes, only to check that the code runs. */
    int smoke;
    int joined;
    int failures;
    long long joined_at_us;
    int initial_cpu_mhz;
    int initial_bus_mhz;
    /* Idle-counter ticks per second with nothing else running, at the current clock. */
    unsigned long long idle_ticks_per_s;
    /* The curl buffer size with the best HTTPS median, for the Memory Stick download. */
    long best_buffer_bytes;
    unsigned long long best_buffer_kb_per_s;
    char ca[SKIFF_PROBE_PATH_MAX];
    /* "Bearer <token>", and the whole header line for raw libcurl requests. */
    char bearer[BEARER_MAX];
    char authorization[AUTHORIZATION_MAX];
    /* An aligned block for downloads written to the Memory Stick. */
    unsigned char *ms_sink;
} bench;

/* ---- Output ---------------------------------------------------------------------------------- */

static void say(bench *b, const char *line) { skiff_psp_report_line(&b->report, line); }

static void check(bench *b, int ok, const char *text) {
    skiff_probe_report_check(&b->report, ok, text);
    b->failures += ok ? 0 : 1;
}

static int apctl_info(int code, SceNetApctlInfo *info) {
    memset(info, 0, sizeof *info);
    return sceNetApctlGetInfo(code, info) >= 0;
}

/* What can change a result between runs: clock, power source, Wi-Fi power save and signal. */
static void describe_environment(const bench *b, char *out, size_t out_size) {
    int wlan_power_save = -1;
    if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE, &wlan_power_save) < 0) {
        wlan_power_save = -1;
    }
    int written = snprintf(out, out_size, "cpu=%d bus=%d ac=%d wlan_ps_setting=%d",
                           scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency(),
                           scePowerIsPowerOnline(), wlan_power_save);
    if (!b->joined || written < 0 || (size_t)written >= out_size) {
        return;
    }
    SceNetApctlInfo strength;
    SceNetApctlInfo channel;
    SceNetApctlInfo security;
    SceNetApctlInfo power_save;
    snprintf(
        out + written, out_size - (size_t)written, " signal=%d channel=%d security=%d wlan_ps=%d",
        apctl_info(PSP_NET_APCTL_INFO_STRENGTH, &strength) ? (int)strength.strength : -1,
        apctl_info(PSP_NET_APCTL_INFO_CHANNEL, &channel) ? (int)channel.channel : -1,
        apctl_info(PSP_NET_APCTL_INFO_SECURITY_TYPE, &security) ? (int)security.securityType : -1,
        apctl_info(PSP_NET_APCTL_INFO_POWER_SAVE, &power_save) ? (int)power_save.powerSave : -1);
}

/* One line in bench-log.txt: the environment, then "item=<id>" and the values. */
static void log_item(bench *b, const char *id, const char *values) {
    if (b->log == NULL) {
        return;
    }
    char environment[ENVIRONMENT_MAX];
    describe_environment(b, environment, sizeof environment);
    fprintf(b->log, "%s item=%s %s\n", environment, id, values);
    fflush(b->log);
}

static void open_log(bench *b) {
    char path[SKIFF_PROBE_PATH_MAX];
    b->log = skiff_probe_sibling(b->program_path, BENCH_LOG_FILE, path) ? fopen(path, "a") : NULL;
    check(b, b->log != NULL, "run log: " BENCH_LOG_FILE " opened for appending");
}

static void close_log(bench *b) {
    if (b->log != NULL) {
        check(b, fclose(b->log) == 0, "run log: " BENCH_LOG_FILE " closed");
        b->log = NULL;
    }
}

static long long now_us(void) { return (long long)sceKernelGetSystemTimeWide(); }

static void say_estimate(bench *b, const char *section, long long seconds) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "== %s: about %lld min %lld s", section, seconds / 60,
             seconds % 60);
    say(b, line);
}

/* ---- Helper threads -------------------------------------------------------------------------- */

static volatile unsigned idle_ticks;
static volatile int idle_stop;
static volatile int ticker_stop;

static int idle_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    while (!idle_stop) {
        for (volatile unsigned spin = 0; spin < IDLE_SPIN; spin++) {
        }
        idle_ticks++;
        if (idle_ticks % IDLE_TICKS_PER_PAUSE == 0) {
            sceKernelDelayThread(IDLE_PAUSE_US);
        }
    }
    return 0;
}

static int ticker_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    while (!ticker_stop) {
        scePowerTick(PSP_POWER_TICK_ALL);
        sceKernelDelayThread(TICKER_INTERVAL_US);
    }
    return 0;
}

static SceUID start_thread(const char *name, SceKernelThreadEntry entry, int priority,
                           int stack_bytes) {
    const SceUID id = sceKernelCreateThread(name, entry, priority, stack_bytes, 0, NULL);
    if (id >= 0 && sceKernelStartThread(id, 0, NULL) < 0) {
        sceKernelDeleteThread(id);
        return -1;
    }
    return id;
}

static void stop_thread(SceUID id, volatile int *stop_flag) {
    if (id < 0) {
        return;
    }
    *stop_flag = 1;
    SceUInt timeout = THREAD_END_TIMEOUT_US;
    if (sceKernelWaitThreadEnd(id, &timeout) < 0) {
        sceKernelTerminateThread(id);
    }
    sceKernelDeleteThread(id);
}

/* Idle ticks per second while this thread sleeps: the baseline for "how busy was the CPU". */
static void calibrate_idle(bench *b) {
    const unsigned start_ticks = idle_ticks;
    const long long start = now_us();
    sceKernelDelayThread(IDLE_CALIBRATION_US);
    const long long elapsed = now_us() - start;
    const unsigned ticks = idle_ticks - start_ticks;
    b->idle_ticks_per_s =
        elapsed > 0 ? (unsigned long long)ticks * US_PER_S / (unsigned long long)elapsed : 0;
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "idle counter at %d MHz: %llu ticks/s",
             scePowerGetCpuClockFrequency(), b->idle_ticks_per_s);
    say(b, line);
}

/* The share of elapsed_us the CPU was busy, from the idle ticks counted meanwhile; -1 if unknown.
 */
static int busy_percent(const bench *b, unsigned ticks, long long elapsed_us) {
    if (b->idle_ticks_per_s == 0 || elapsed_us <= 0) {
        return -1;
    }
    const unsigned long long expected =
        b->idle_ticks_per_s * (unsigned long long)elapsed_us / US_PER_S;
    if (expected == 0 || ticks >= expected) {
        return 0;
    }
    return (int)(PERCENT - (unsigned long long)ticks * PERCENT / expected);
}

static int set_clock(bench *b, int cpu_mhz, int bus_mhz) {
    const int result = scePowerSetClockFrequency(cpu_mhz, cpu_mhz, bus_mhz);
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "clock set to %d/%d MHz (now %d/%d)", cpu_mhz, bus_mhz,
             scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());
    check(b, result >= 0, text);
    return result >= 0;
}

/* ---- Latency --------------------------------------------------------------------------------- */

static size_t discard_body(char *data, size_t size, size_t count, void *context) {
    (void)data;
    (void)context;
    return size * count;
}

/* A plain TCP connect to the TLS port (CONNECT_ONLY sends nothing): no TLS cost in the time. */
static void measure_connect(bench *b, int number) {
    char url[URL_MAX];
    snprintf(url, sizeof url, "http://%s:%d/", b->config.host, HTTPS_PORT);
    CURL *curl = curl_easy_init();
    CURLcode code = CURLE_OUT_OF_MEMORY;
    curl_off_t connect_us = 0;
    const long long since_join_us = now_us() - b->joined_at_us;
    if (curl != NULL && curl_easy_setopt(curl, CURLOPT_URL, url) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)CONNECT_TIMEOUT_S) == CURLE_OK) {
        code = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &connect_us);
    }
    curl_easy_cleanup(curl);
    const long long connect_ms = (long long)connect_us / US_PER_MS;
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "TCP connect %d, %lld.%lld s after joining: %lld ms%s (curl %d)",
             number, since_join_us / US_PER_S, since_join_us % US_PER_S / (US_PER_S / TENTHS),
             connect_ms, connect_ms >= SYN_RETRY_SUSPECT_MS ? ", SYN retried?" : "", (int)code);
    check(b, code == CURLE_OK, text);
    char values[SKIFF_SELFTEST_LINE_MAX];
    char id[SKIFF_SELFTEST_LINE_MAX];
    snprintf(id, sizeof id, "connect-%d", number);
    snprintf(values, sizeof values, "after_join_ms=%lld connect_ms=%lld curl=%d",
             since_join_us / US_PER_MS, connect_ms, (int)code);
    log_item(b, id, values);
}

static int configure_heartbeat(const bench *b, CURL *curl, char *url) {
    snprintf(url, URL_MAX, "https://%s:%d" HEARTBEAT_PATH, b->config.host, HTTPS_PORT);
    return curl_easy_setopt(curl, CURLOPT_URL, url) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_CAINFO, b->ca) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)CONNECT_TIMEOUT_S) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)STALL_TIMEOUT_S) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body) == CURLE_OK;
}

/* Heartbeats on one kept connection after growing idle gaps: power save adds latency to the first
 * request after the radio has dozed off, a healthy link does not. */
static void measure_idle_gaps(bench *b) {
    CURL *curl = curl_easy_init();
    char url[URL_MAX];
    if (curl == NULL || !configure_heartbeat(b, curl, url)) {
        check(b, 0, "kept connection: curl setup failed");
        curl_easy_cleanup(curl);
        return;
    }
    CURLcode code = curl_easy_perform(curl);
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "kept connection opened (curl %d)", (int)code);
    check(b, code == CURLE_OK, text);
    for (size_t i = 0; code == CURLE_OK && i < sizeof IDLE_GAPS_MS / sizeof IDLE_GAPS_MS[0]; i++) {
        sceKernelDelayThread((SceUInt)(IDLE_GAPS_MS[i] * US_PER_MS));
        code = curl_easy_perform(curl);
        curl_off_t total_us = 0;
        long new_connections = -1;
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total_us);
        curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &new_connections);
        snprintf(text, sizeof text,
                 "heartbeat after %ld ms idle: %lld ms, %ld new connection(s) (curl %d)",
                 IDLE_GAPS_MS[i], (long long)total_us / US_PER_MS, new_connections, (int)code);
        check(b, code == CURLE_OK && new_connections == 0, text);
        char id[SKIFF_SELFTEST_LINE_MAX];
        char values[SKIFF_SELFTEST_LINE_MAX];
        snprintf(id, sizeof id, "idle-%ldms", IDLE_GAPS_MS[i]);
        snprintf(values, sizeof values, "total_ms=%lld new_connections=%ld curl=%d",
                 (long long)total_us / US_PER_MS, new_connections, (int)code);
        log_item(b, id, values);
    }
    curl_easy_cleanup(curl);
}

static void run_latency(bench *b) {
    say_estimate(b, "latency (TCP connects after joining, kept connection after idle gaps)",
                 (2000 + 5000 + 10000 + 500 + 2000 + 5000 + 15000) / MS_PER_S + 5);
    int number = 1;
    for (; number <= IMMEDIATE_CONNECTS; number++) {
        measure_connect(b, number);
    }
    for (size_t i = 0; i < sizeof LATENCY_PAUSES_MS / sizeof LATENCY_PAUSES_MS[0]; i++, number++) {
        sceKernelDelayThread((SceUInt)(LATENCY_PAUSES_MS[i] * US_PER_MS));
        measure_connect(b, number);
    }
    measure_idle_gaps(b);
}

/* ---- CPU ------------------------------------------------------------------------------------- */

typedef struct cpu_bench {
    unsigned char *input;
    int passes;
    /* Results are folded in here, so the compiler cannot drop the work. */
    volatile uint32_t sink;
} cpu_bench;

typedef int (*cpu_kernel)(cpu_bench *c);

static int cpu_crc32_bitwise(cpu_bench *c) {
    uint32_t crc = 0;
    for (int pass = 0; pass < c->passes; pass++) {
        for (size_t offset = 0; offset < CPU_BUFFER_BYTES; offset += CPU_CHUNK_BYTES) {
            crc = skiff_probe_crc32_bitwise(crc, c->input + offset, CPU_CHUNK_BYTES);
        }
    }
    c->sink ^= crc;
    return 1;
}

static int cpu_crc32_table(cpu_bench *c) {
    uint32_t crc = 0;
    for (int pass = 0; pass < c->passes; pass++) {
        for (size_t offset = 0; offset < CPU_BUFFER_BYTES; offset += CPU_CHUNK_BYTES) {
            crc = skiff_probe_crc32_table(crc, c->input + offset, CPU_CHUNK_BYTES);
        }
    }
    c->sink ^= crc;
    return 1;
}

static int cpu_crc32_zlib(cpu_bench *c) {
    uLong crc = crc32(0L, Z_NULL, 0);
    for (int pass = 0; pass < c->passes; pass++) {
        for (size_t offset = 0; offset < CPU_BUFFER_BYTES; offset += CPU_CHUNK_BYTES) {
            crc = crc32(crc, c->input + offset, CPU_CHUNK_BYTES);
        }
    }
    c->sink ^= (uint32_t)crc;
    return 1;
}

/* What the network probe's body callback did for every byte when T1 measured 193 KB/s. */
static int cpu_probe_digest(cpu_bench *c) {
    uint32_t crc = 0;
    uint32_t whole = FNV_OFFSET_BASIS;
    uint32_t suffix = FNV_OFFSET_BASIS;
    for (int pass = 0; pass < c->passes; pass++) {
        for (size_t offset = 0; offset < CPU_BUFFER_BYTES; offset += CPU_CHUNK_BYTES) {
            const unsigned char *data = c->input + offset;
            crc = skiff_probe_crc32_bitwise(crc, data, CPU_CHUNK_BYTES);
            for (size_t i = 0; i < CPU_CHUNK_BYTES; i++) {
                whole = (whole ^ data[i]) * FNV_PRIME;
                suffix = (suffix ^ data[i]) * FNV_PRIME;
            }
        }
    }
    c->sink ^= crc ^ whole ^ suffix;
    return 1;
}

static int cpu_hash(cpu_bench *c, psa_algorithm_t algorithm) {
    unsigned char hash[HASH_MAX_BYTES];
    size_t hash_length = 0;
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    psa_status_t status = psa_hash_setup(&operation, algorithm);
    for (int pass = 0; status == PSA_SUCCESS && pass < c->passes; pass++) {
        for (size_t offset = 0; status == PSA_SUCCESS && offset < CPU_BUFFER_BYTES;
             offset += CPU_CHUNK_BYTES) {
            status = psa_hash_update(&operation, c->input + offset, CPU_CHUNK_BYTES);
        }
    }
    if (status == PSA_SUCCESS) {
        status = psa_hash_finish(&operation, hash, sizeof hash, &hash_length);
    }
    psa_hash_abort(&operation);
    c->sink ^= hash_length > 0 ? (uint32_t)hash[0] : 0U;
    return status == PSA_SUCCESS;
}

static int cpu_md5(cpu_bench *c) { return cpu_hash(c, PSA_ALG_MD5); }

static int cpu_sha1(cpu_bench *c) { return cpu_hash(c, PSA_ALG_SHA_1); }

/* Decrypts one TLS-record-sized chunk over and over, as TLS does with every record it receives. */
static int cpu_aead(cpu_bench *c, psa_key_type_t key_type, size_t key_bits,
                    psa_algorithm_t algorithm) {
    static const unsigned char nonce[AEAD_NONCE_BYTES] = {0};
    unsigned char key[KEY_BYTES_MAX];
    memset(key, KEY_FILL_BYTE, sizeof key);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, algorithm);
    psa_set_key_type(&attributes, key_type);
    psa_set_key_bits(&attributes, key_bits);
    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    unsigned char *sealed = malloc(CPU_CHUNK_BYTES + AEAD_TAG_BYTES);
    unsigned char *opened = malloc(CPU_CHUNK_BYTES + AEAD_TAG_BYTES);
    size_t sealed_length = 0;
    size_t opened_length = 0;
    psa_status_t status = sealed != NULL && opened != NULL
                              ? psa_import_key(&attributes, key, key_bits / BITS_PER_BYTE, &key_id)
                              : PSA_ERROR_INSUFFICIENT_MEMORY;
    if (status == PSA_SUCCESS) {
        status = psa_aead_encrypt(key_id, algorithm, nonce, sizeof nonce, NULL, 0, c->input,
                                  CPU_CHUNK_BYTES, sealed, CPU_CHUNK_BYTES + AEAD_TAG_BYTES,
                                  &sealed_length);
    }
    const int records = c->passes * (CPU_BUFFER_BYTES / CPU_CHUNK_BYTES);
    for (int record = 0; status == PSA_SUCCESS && record < records; record++) {
        status =
            psa_aead_decrypt(key_id, algorithm, nonce, sizeof nonce, NULL, 0, sealed, sealed_length,
                             opened, CPU_CHUNK_BYTES + AEAD_TAG_BYTES, &opened_length);
    }
    const int ok = status == PSA_SUCCESS && opened_length == CPU_CHUNK_BYTES &&
                   memcmp(opened, c->input, CPU_CHUNK_BYTES) == 0;
    psa_destroy_key(key_id);
    free(sealed);
    free(opened);
    return ok;
}

static int cpu_chacha20_poly1305(cpu_bench *c) {
    return cpu_aead(c, PSA_KEY_TYPE_CHACHA20, CHACHA20_KEY_BITS, PSA_ALG_CHACHA20_POLY1305);
}

static int cpu_aes_128_gcm(cpu_bench *c) {
    return cpu_aead(c, PSA_KEY_TYPE_AES, AES_128_KEY_BITS, PSA_ALG_GCM);
}

typedef struct cpu_item {
    const char *id;
    const char *label;
    cpu_kernel run;
    int needs_psa;
} cpu_item;

static const cpu_item CPU_ITEMS[] = {
    {"cpu-crc32-bitwise", "CRC-32, bitwise", cpu_crc32_bitwise, 0},
    {"cpu-crc32-table", "CRC-32, 1 KB table", cpu_crc32_table, 0},
    {"cpu-crc32-zlib", "CRC-32, zlib", cpu_crc32_zlib, 0},
    {"cpu-probe-digest", "network probe's per-byte digest (CRC-32 bitwise + 2 FNV-1a)",
     cpu_probe_digest, 0},
    {"cpu-md5", "MD5 (PSA)", cpu_md5, 1},
    {"cpu-sha1", "SHA-1 (PSA)", cpu_sha1, 1},
    {"cpu-chacha20-poly1305", "ChaCha20-Poly1305 decrypt, 16 KB records (PSA)",
     cpu_chacha20_poly1305, 1},
    {"cpu-aes128-gcm", "AES-128-GCM decrypt, 16 KB records (PSA)", cpu_aes_128_gcm, 1},
};

static void run_cpu_item(bench *b, cpu_bench *c, const cpu_item *item, int psa_ready) {
    char text[LINE_MAX_BENCH];
    if (item->needs_psa && !psa_ready) {
        snprintf(text, sizeof text, "skip %s: PSA crypto did not start (needs ARK)", item->label);
        say(b, text);
        return;
    }
    const long long start = now_us();
    const int ok = item->run(c);
    const long long elapsed_us = now_us() - start;
    const unsigned long long bytes = (unsigned long long)c->passes * CPU_BUFFER_BYTES;
    const int mhz = scePowerGetCpuClockFrequency();
    const unsigned long long cycles_tenths =
        (unsigned long long)mhz * (unsigned long long)elapsed_us * TENTHS / bytes;
    const unsigned long long kb_per_s = skiff_probe_kb_per_s(bytes, elapsed_us);
    snprintf(text, sizeof text,
             "%s: %llu KB/s, %llu.%llu cycles/byte at %d MHz (%llu KB in %lld ms)", item->label,
             kb_per_s, cycles_tenths / TENTHS, cycles_tenths % TENTHS, mhz, bytes / KB,
             elapsed_us / US_PER_MS);
    check(b, ok, text);
    char values[SKIFF_SELFTEST_LINE_MAX];
    snprintf(values, sizeof values, "kbs=%llu cycles_per_byte=%llu.%llu bytes=%llu ok=%d", kb_per_s,
             cycles_tenths / TENTHS, cycles_tenths % TENTHS, bytes, ok);
    log_item(b, item->id, values);
}

static void run_cpu(bench *b) {
    say_estimate(b, "cpu (hashes and TLS ciphers on 1 MiB, in 16 KB chunks)", b->smoke ? 5 : 40);
    cpu_bench c = {.passes = b->smoke ? CPU_SMOKE_PASSES : CPU_PASSES};
    c.input = memalign(MS_ALIGNMENT, CPU_BUFFER_BYTES);
    if (c.input == NULL) {
        check(b, 0, "cpu: no memory for the 1 MiB buffer");
        return;
    }
    for (size_t i = 0; i < CPU_BUFFER_BYTES; i++) {
        c.input[i] = (unsigned char)(i * 131U + (i >> 8));
    }
    /* libcurl already started PSA crypto with ARK; without ARK it refuses (no entropy). */
    const int psa_ready = b->has_ark && psa_crypto_init() == PSA_SUCCESS;
    for (size_t i = 0; i < sizeof CPU_ITEMS / sizeof CPU_ITEMS[0]; i++) {
        run_cpu_item(b, &c, &CPU_ITEMS[i], psa_ready);
    }
    free(c.input);
}

/* ---- Downloads ------------------------------------------------------------------------------- */

typedef struct download_result {
    int setup_failed;
    CURLcode code;
    long status;
    unsigned long long bytes;
    uint32_t crc32;
    /* From the first body chunk's arrival to the last's, and the bytes that arrived in between (all
     * but the first chunk): no TCP or TLS setup, no wait for the first byte. CPU busy is measured
     * over the same window. */
    long long transfer_us;
    unsigned long long transfer_bytes;
    long long tls_us;
    int busy_percent;
    char tls_version[SKIFF_PROBE_TLS_NAME_MAX];
    char cipher[SKIFF_PROBE_TLS_NAME_MAX];
    /* The socket's SO_RCVBUF after setup (-1 unknown), and whether setting it failed. */
    int receive_buffer;
    int receive_buffer_refused;
    /* Memory Stick download: whether every block was written, and the time spent writing. */
    int write_failed;
    long long write_us;
} download_result;

typedef struct download {
    const bench *owner;
    const download_spec *spec;
    download_result *result;
    CURL *curl;
    SceUID file;
    unsigned char *sink;
    size_t sink_used;
    /* When the first body chunk was done with (hashed, buffered), the idle ticks then, and its
     * size: the measured window starts there. */
    int receiving;
    long long first_chunk_us;
    unsigned first_chunk_ticks;
    unsigned long long first_chunk_bytes;
} download;

/* Writes the buffered block; 0 if the Memory Stick took less than all of it. The block is dropped
 * either way, so the buffer always has room again. */
static int write_sink(download *d) {
    if (d->sink_used == 0) {
        return !d->result->write_failed;
    }
    const long long start = now_us();
    const int written = sceIoWrite(d->file, d->sink, (SceSize)d->sink_used);
    d->result->write_us += now_us() - start;
    d->result->write_failed |= written != (int)d->sink_used;
    d->sink_used = 0;
    return !d->result->write_failed;
}

/* Closes the measured window after a unit of body work: from the end of the first chunk's work to
 * now, the bytes after the first chunk, and the CPU's busy share meanwhile. */
static void mark_progress(download *d) {
    const long long now = now_us();
    const unsigned ticks = idle_ticks;
    if (!d->receiving) {
        d->receiving = 1;
        d->first_chunk_us = now;
        d->first_chunk_ticks = ticks;
        d->first_chunk_bytes = d->result->bytes;
        return;
    }
    d->result->transfer_us = now - d->first_chunk_us;
    d->result->transfer_bytes = d->result->bytes - d->first_chunk_bytes;
    d->result->busy_percent =
        busy_percent(d->owner, ticks - d->first_chunk_ticks, d->result->transfer_us);
}

static size_t on_download_body(char *data, size_t size, size_t count, void *context) {
    download *d = context;
    const size_t bytes = size * count;
    if (d->result->tls_version[0] == '\0' && d->spec->https) {
        skiff_probe_read_tls_session(d->curl, d->result->tls_version, d->result->cipher);
    }
    const unsigned char *bytes_in = (const unsigned char *)data;
    d->result->crc32 = skiff_probe_crc32_table(d->result->crc32, bytes_in, bytes);
    d->result->bytes += bytes;
    for (size_t offset = 0; d->sink != NULL && offset < bytes;) {
        const size_t room = MS_SINK_BYTES - d->sink_used;
        const size_t take = bytes - offset < room ? bytes - offset : room;
        memcpy(d->sink + d->sink_used, bytes_in + offset, take);
        d->sink_used += take;
        offset += take;
        if (d->sink_used == MS_SINK_BYTES && !write_sink(d)) {
            return 0; /* short of bytes: curl stops with CURLE_WRITE_ERROR */
        }
    }
    mark_progress(d);
    return bytes;
}

static int on_socket(void *context, curl_socket_t socket_fd, curlsocktype purpose) {
    download *d = context;
    if (purpose != CURLSOCKTYPE_IPCXN) {
        return CURL_SOCKOPT_OK;
    }
    if (d->spec->receive_buffer_bytes > 0) {
        const int wanted = d->spec->receive_buffer_bytes;
        d->result->receive_buffer_refused =
            setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &wanted, sizeof wanted) != 0;
    }
    int actual = -1;
    socklen_t length = sizeof actual;
    if (getsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &actual, &length) != 0) {
        actual = -1;
    }
    d->result->receive_buffer = actual;
    /* A refused size would measure the default buffer under the wrong name: stop before connecting.
     */
    return d->result->receive_buffer_refused ? CURL_SOCKOPT_ERROR : CURL_SOCKOPT_OK;
}

static void content_url(const bench *b, int https, char *out) {
    snprintf(out, URL_MAX, "%s://%s:%d/api/roms/%s/content/%s", https ? "https" : "http",
             b->config.host, https ? HTTPS_PORT : HTTP_PORT, b->config.rom_id, b->config.file_name);
}

static int configure_download(const bench *b, download *d, const char *url,
                              struct curl_slist *headers) {
    const download_spec *spec = d->spec;
    CURL *curl = d->curl;
    int ok = curl_easy_setopt(curl, CURLOPT_URL, url) == CURLE_OK &&
             curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
             curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
             curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)CONNECT_TIMEOUT_S) == CURLE_OK &&
             curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, (long)STALL_BYTES_PER_S) == CURLE_OK &&
             curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)STALL_TIMEOUT_S) == CURLE_OK &&
             curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_download_body) == CURLE_OK &&
             curl_easy_setopt(curl, CURLOPT_WRITEDATA, d) == CURLE_OK &&
             curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, on_socket) == CURLE_OK &&
             curl_easy_setopt(curl, CURLOPT_SOCKOPTDATA, d) == CURLE_OK;
    if (ok && spec->https) {
        ok = curl_easy_setopt(curl, CURLOPT_CAINFO, b->ca) == CURLE_OK;
    }
    if (ok && spec->buffer_bytes > 0) {
        ok = curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, spec->buffer_bytes) == CURLE_OK;
    }
    if (ok && spec->ssl_version != 0) {
        ok = curl_easy_setopt(curl, CURLOPT_SSLVERSION, spec->ssl_version) == CURLE_OK;
    }
    if (ok && spec->tls13_ciphers != NULL) {
        ok = curl_easy_setopt(curl, CURLOPT_TLS13_CIPHERS, spec->tls13_ciphers) == CURLE_OK;
    }
    if (ok && spec->cipher_list != NULL) {
        ok = curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST, spec->cipher_list) == CURLE_OK;
    }
    return ok;
}

static void read_timings(CURL *curl, download_result *result) {
    curl_off_t tcp_done = 0;
    curl_off_t tls_done = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->status);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &tcp_done);
    curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME_T, &tls_done);
    result->tls_us = tls_done > tcp_done ? (long long)(tls_done - tcp_done) : 0;
}

/* Opens the Memory Stick file a download is written to; 0 if it cannot. */
static int open_download_file(const bench *b, download *d) {
    char path[SKIFF_PROBE_PATH_MAX];
    if (!skiff_probe_sibling(b->program_path, MS_TEST_FILE, path)) {
        return 0;
    }
    d->file = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    d->sink = b->ms_sink;
    return d->file >= 0 && d->sink != NULL;
}

/* The last partial block and the close (which flushes the file system) are part of the download's
 * cost, so the measured window extends over them. */
static void close_download_file(const bench *b, download *d) {
    write_sink(d);
    const long long start = now_us();
    d->result->write_failed |= sceIoClose(d->file) < 0;
    d->result->write_us += now_us() - start;
    if (d->result->code == CURLE_OK) {
        mark_progress(d);
    }
    char path[SKIFF_PROBE_PATH_MAX];
    if (skiff_probe_sibling(b->program_path, MS_TEST_FILE, path)) {
        sceIoRemove(path);
    }
}

/* One download on a fresh connection (a full handshake), hashed as it arrives. */
static void run_download(bench *b, const download_spec *spec, download_result *result) {
    memset(result, 0, sizeof *result);
    result->receive_buffer = -1;
    result->busy_percent = -1;
    download d = {.owner = b, .spec = spec, .result = result, .file = -1};
    char url[URL_MAX];
    content_url(b, spec->https, url);
    struct curl_slist *headers = curl_slist_append(NULL, b->authorization);
    d.curl = curl_easy_init();
    if (headers == NULL || d.curl == NULL || !configure_download(b, &d, url, headers) ||
        (spec->to_memory_stick && !open_download_file(b, &d))) {
        result->setup_failed = 1;
    } else {
        result->code = curl_easy_perform(d.curl);
        read_timings(d.curl, result);
    }
    if (d.file >= 0) {
        close_download_file(b, &d);
    }
    curl_easy_cleanup(d.curl);
    curl_slist_free_all(headers);
}

static int download_ok(const bench *b, const download_spec *spec, const download_result *result) {
    return !result->setup_failed && result->code == CURLE_OK && result->status == HTTP_OK &&
           result->transfer_us > 0 && result->bytes == b->config.size &&
           result->crc32 == b->config.crc32 && !result->write_failed &&
           (spec->expect_version == NULL ||
            strcmp(result->tls_version, spec->expect_version) == 0) &&
           (spec->expect_suite == NULL || strstr(result->cipher, spec->expect_suite) != NULL);
}

typedef struct download_summary {
    int ran;
    int ok;
    unsigned long long median_kb_per_s;
} download_summary;

static void report_run(bench *b, const download_spec *spec, int run, const download_result *r,
                       int ok) {
    char writes[SKIFF_SELFTEST_LINE_MAX] = "";
    if (spec->to_memory_stick) {
        snprintf(writes, sizeof writes, ", Memory Stick writes %lld ms%s", r->write_us / US_PER_MS,
                 r->write_failed ? " (FAILED)" : "");
    }
    char text[LINE_MAX_BENCH];
    snprintf(text, sizeof text,
             "  %s run %d: %llu KB/s, CPU busy %d%%, TLS %lld ms, curl %d (%s), HTTP %ld, %llu "
             "bytes, CRC-32 %08lx, %s %s, SO_RCVBUF %d%s",
             spec->id, run + 1, skiff_probe_kb_per_s(r->transfer_bytes, r->transfer_us),
             r->busy_percent, r->tls_us / US_PER_MS, (int)r->code, curl_easy_strerror(r->code),
             r->status, r->bytes, (unsigned long)r->crc32, r->tls_version, r->cipher,
             r->receive_buffer, writes);
    check(b, ok, text);
}

/* runs downloads of spec; the median speed, CPU use and handshake, also to bench-log.txt. */
static download_summary measure_download(bench *b, const download_spec *spec) {
    download_summary summary = {0};
    long long kb_per_s[SKIFF_PROBE_RUNS_MAX];
    long long busy[SKIFF_PROBE_RUNS_MAX];
    long long tls_ms[SKIFF_PROBE_RUNS_MAX];
    long long write_ms[SKIFF_PROBE_RUNS_MAX];
    download_result result = {0};
    int all_ok = 1;
    int runs = 0;
    for (; runs < b->config.runs && !skiff_psp_exit_requested(); runs++) {
        run_download(b, spec, &result);
        if (result.receive_buffer_refused) {
            char text[SKIFF_SELFTEST_LINE_MAX];
            snprintf(text, sizeof text, "skip %s: the PSP refused SO_RCVBUF %d (now %d)", spec->id,
                     spec->receive_buffer_bytes, result.receive_buffer);
            say(b, text);
            log_item(b, spec->id, "refused=1");
            return summary;
        }
        const int ok = download_ok(b, spec, &result);
        report_run(b, spec, runs, &result, ok);
        all_ok &= ok;
        kb_per_s[runs] = (long long)skiff_probe_kb_per_s(result.transfer_bytes, result.transfer_us);
        busy[runs] = result.busy_percent;
        tls_ms[runs] = result.tls_us / US_PER_MS;
        write_ms[runs] = result.write_us / US_PER_MS;
    }
    if (runs == 0) {
        return summary;
    }
    summary.ran = 1;
    summary.ok = all_ok;
    const size_t count = (size_t)runs;
    summary.median_kb_per_s = (unsigned long long)skiff_probe_median(kb_per_s, count);
    const long long busy_median = skiff_probe_median(busy, count);
    const long long tls_median = skiff_probe_median(tls_ms, count);
    const long long write_median = skiff_probe_median(write_ms, count);
    char text[LINE_MAX_BENCH];
    snprintf(text, sizeof text,
             "%s: median %llu KB/s (min %lld, max %lld) of %d, CPU busy %lld%%, TLS %lld ms, "
             "SO_RCVBUF %d, %s %s",
             spec->label, summary.median_kb_per_s, kb_per_s[0], kb_per_s[count - 1], runs,
             busy_median, tls_median, result.receive_buffer, result.tls_version, result.cipher);
    check(b, all_ok, text);
    char values[LINE_MAX_BENCH];
    snprintf(values, sizeof values,
             "median_kbs=%llu min_kbs=%lld max_kbs=%lld runs=%d busy_pct=%lld tls_ms=%lld "
             "write_ms=%lld rcvbuf=%d tls=%s cipher=%s ok=%d",
             summary.median_kb_per_s, kb_per_s[0], kb_per_s[count - 1], runs, busy_median,
             tls_median, write_median, result.receive_buffer,
             result.tls_version[0] != '\0' ? result.tls_version : "-",
             result.cipher[0] != '\0' ? result.cipher : "-", all_ok);
    log_item(b, spec->id, values);
    return summary;
}

static void measure_specs(bench *b, const download_spec *specs, size_t count) {
    for (size_t i = 0; i < count && !skiff_psp_exit_requested(); i++) {
        if (!specs[i].https && !b->config.plain_http) {
            char text[SKIFF_SELFTEST_LINE_MAX];
            snprintf(text, sizeof text,
                     "skip %s: plain HTTP stays local (SKIFF_LAN_PLAIN_HTTP=1 scripts/dev.sh "
                     "romm-lan, then install again)",
                     specs[i].id);
            say(b, text);
            continue;
        }
        const download_summary summary = measure_download(b, &specs[i]);
        if (summary.ran && summary.ok && specs[i].buffer_candidate &&
            summary.median_kb_per_s > b->best_buffer_kb_per_s) {
            b->best_buffer_kb_per_s = summary.median_kb_per_s;
            b->best_buffer_bytes = specs[i].buffer_bytes;
        }
    }
}

static long long download_seconds(const bench *b, size_t downloads) {
    return (long long)(downloads * (size_t)b->config.runs *
                       (b->config.size / KB / ASSUMED_KB_PER_S + 1));
}

static void run_net(bench *b) {
    const size_t specs = sizeof NET_SPECS / sizeof NET_SPECS[0];
    const size_t fast_specs = sizeof FAST_CLOCK_SPECS / sizeof FAST_CLOCK_SPECS[0];
    char section[SKIFF_SELFTEST_LINE_MAX];
    snprintf(section, sizeof section, "net (%u downloads x %d runs of %llu KB, at %d KB/s)",
             (unsigned)(specs + fast_specs), b->config.runs, b->config.size / KB, ASSUMED_KB_PER_S);
    say_estimate(b, section, download_seconds(b, specs + fast_specs));
    calibrate_idle(b);
    measure_specs(b, NET_SPECS, specs);
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "best curl buffer for HTTPS: %ld KB (%llu KB/s)",
             b->best_buffer_bytes / KB, b->best_buffer_kb_per_s);
    say(b, text);
    if (set_clock(b, CLOCK_FAST_CPU_MHZ, CLOCK_FAST_BUS_MHZ)) {
        calibrate_idle(b);
        measure_specs(b, FAST_CLOCK_SPECS, fast_specs);
    }
    /* Back to where the run started, whatever happened. */
    set_clock(b, b->initial_cpu_mhz, b->initial_bus_mhz);
    calibrate_idle(b);
}

/* ---- Memory Stick ---------------------------------------------------------------------------- */

static int memory_stick_free(unsigned long long *free_bytes) {
    SceDevInf info;
    memset(&info, 0, sizeof info);
    SceDevctlCmd command = {&info};
    if (sceIoDevctl(MS_DEVICE, SCE_PR_GETDEV, &command, sizeof command, NULL, 0) < 0) {
        return 0;
    }
    *free_bytes = (unsigned long long)info.freeClusters * (unsigned long long)info.sectorCount *
                  (unsigned long long)info.sectorSize;
    return 1;
}

/* Writes file_bytes in blocks of block_bytes, closes (flushing), then reads them back and compares.
 * Returns 0 when any step fails; the file is removed in every case. */
static int memory_stick_block(bench *b, const char *path, const unsigned char *pattern,
                              unsigned char *readback, int block_bytes, int file_bytes) {
    const SceSize block = (SceSize)block_bytes;
    const int blocks = file_bytes / block_bytes;
    long long start = now_us();
    SceUID file = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    int ok = file >= 0;
    for (int i = 0; ok && i < blocks; i++) {
        ok = sceIoWrite(file, pattern, block) == block_bytes;
    }
    ok = file >= 0 && sceIoClose(file) >= 0 && ok;
    const long long write_us = now_us() - start;

    start = now_us();
    file = ok ? sceIoOpen(path, PSP_O_RDONLY, 0) : -1;
    int same = file >= 0;
    for (int i = 0; same && i < blocks; i++) {
        same = sceIoRead(file, readback, block) == block_bytes &&
               memcmp(readback, pattern, block) == 0;
    }
    same = file >= 0 && sceIoClose(file) >= 0 && same;
    const long long read_us = now_us() - start;
    sceIoRemove(path);

    const unsigned long long bytes = (unsigned long long)blocks * block;
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text,
             "Memory Stick, %d KB blocks: write %llu KB/s, read %llu KB/s (%llu KB, data %s)",
             block_bytes / KB, skiff_probe_kb_per_s(bytes, write_us),
             skiff_probe_kb_per_s(bytes, read_us), bytes / KB,
             same ? "read back intact" : "NOT read back intact");
    check(b, ok && same, text);
    char id[SKIFF_SELFTEST_LINE_MAX];
    char values[SKIFF_SELFTEST_LINE_MAX];
    snprintf(id, sizeof id, "ms-%dk", block_bytes / KB);
    snprintf(values, sizeof values, "write_kbs=%llu read_kbs=%llu bytes=%llu ok=%d",
             skiff_probe_kb_per_s(bytes, write_us), skiff_probe_kb_per_s(bytes, read_us), bytes,
             ok && same);
    log_item(b, id, values);
    return ok && same;
}

static void run_memory_stick_blocks(bench *b) {
    const int file_bytes = b->smoke ? MS_SMOKE_FILE_BYTES : MS_FILE_BYTES;
    const int *sizes = b->smoke ? MS_SMOKE_BLOCK_BYTES : MS_BLOCK_BYTES;
    const size_t count = b->smoke ? sizeof MS_SMOKE_BLOCK_BYTES / sizeof MS_SMOKE_BLOCK_BYTES[0]
                                  : sizeof MS_BLOCK_BYTES / sizeof MS_BLOCK_BYTES[0];
    const int largest = sizes[count - 1];
    say_estimate(b, "ms (write and read back by block size)", b->smoke ? 5 : 90);
    char path[SKIFF_PROBE_PATH_MAX];
    unsigned long long free_bytes = 0;
    char text[SKIFF_SELFTEST_LINE_MAX];
    if (memory_stick_free(&free_bytes)) {
        snprintf(text, sizeof text, "Memory Stick: %llu MB free", free_bytes / MIB);
        say(b, text);
        if (free_bytes < (unsigned long long)file_bytes + MS_HEADROOM_BYTES) {
            check(b, 0, "Memory Stick: not enough free space for the test file");
            return;
        }
    } else {
        say(b, "Memory Stick: free space unknown (sceIoDevctl failed)");
    }
    unsigned char *pattern = memalign(MS_ALIGNMENT, (size_t)largest);
    unsigned char *readback = memalign(MS_ALIGNMENT, (size_t)largest);
    if (pattern == NULL || readback == NULL ||
        !skiff_probe_sibling(b->program_path, MS_TEST_FILE, path)) {
        check(b, 0, "Memory Stick: no memory for the blocks, or no path for the test file");
    } else {
        for (int i = 0; i < largest; i++) {
            pattern[i] = (unsigned char)(i * 7 + (i >> 9));
        }
        for (size_t i = 0; i < count && !skiff_psp_exit_requested(); i++) {
            memory_stick_block(b, path, pattern, readback, sizes[i], file_bytes);
        }
    }
    free(pattern);
    free(readback);
}

static void run_memory_stick_download(bench *b) {
    const download_spec spec = {
        .id = "https-ms-write",
        .label = "HTTPS, written to the Memory Stick in 128 KB blocks",
        .https = 1,
        .buffer_bytes = b->best_buffer_bytes,
        .to_memory_stick = 1,
    };
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "download + Memory Stick: curl buffer %ld KB (best HTTPS)",
             b->best_buffer_bytes / KB);
    say_estimate(b, text, download_seconds(b, 1));
    b->ms_sink = memalign(MS_ALIGNMENT, MS_SINK_BYTES);
    if (b->ms_sink == NULL) {
        check(b, 0, "download + Memory Stick: no memory for the 128 KB block");
        return;
    }
    measure_download(b, &spec);
    free(b->ms_sink);
    b->ms_sink = NULL;
}

/* ---- Skiff's transport ----------------------------------------------------------------------- */

typedef struct app_digest {
    unsigned long long bytes;
    uint32_t crc32;
} app_digest;

static skiff_err app_body(void *context, const unsigned char *data, size_t size) {
    app_digest *digest = context;
    digest->crc32 = skiff_probe_crc32_table(digest->crc32, data, size);
    digest->bytes += size;
    return SKIFF_OK;
}

/* The download as the app makes it: one kept connection, the transport's defaults. The first run
 * also pays the handshake. */
static void run_app(bench *b) {
    say_estimate(b, "app (Skiff's transport, defaults)", download_seconds(b, 1));
    const skiff_http_header headers[] = {{AUTHORIZATION_HEADER, b->bearer}};
    const skiff_curl_config config = {
        .ca_file = b->ca, .default_headers = headers, .default_header_count = 1};
    skiff_transport *transport = NULL;
    skiff_err err = skiff_curl_transport_create(&config, &transport);
    if (err != SKIFF_OK) {
        char text[SKIFF_SELFTEST_LINE_MAX];
        snprintf(text, sizeof text, "app: creating the transport failed: %s", skiff_err_name(err));
        check(b, 0, text);
        return;
    }
    char url[URL_MAX];
    content_url(b, 1, url);
    long long kb_per_s[SKIFF_PROBE_RUNS_MAX];
    int all_ok = 1;
    int runs = 0;
    for (; runs < b->config.runs && !skiff_psp_exit_requested(); runs++) {
        app_digest digest = {0};
        skiff_http_response response;
        const skiff_http_request request = {.url = url, .on_body = app_body, .body_ctx = &digest};
        const long long start = now_us();
        err = skiff_transport_perform(transport, &request, &response);
        const long long elapsed_us = now_us() - start;
        const int ok = err == SKIFF_OK && response.status == HTTP_OK &&
                       digest.bytes == b->config.size && digest.crc32 == b->config.crc32;
        kb_per_s[runs] = (long long)skiff_probe_kb_per_s(digest.bytes, elapsed_us);
        char text[SKIFF_SELFTEST_LINE_MAX];
        snprintf(text, sizeof text,
                 "  app run %d: %lld KB/s, %s, HTTP %ld, %llu bytes, %ld new connection(s)",
                 runs + 1, kb_per_s[runs], skiff_err_name(err), response.status, digest.bytes,
                 response.new_connections);
        check(b, ok, text);
        all_ok &= ok;
    }
    skiff_transport_destroy(transport);
    if (runs == 0) {
        return;
    }
    const long long median = skiff_probe_median(kb_per_s, (size_t)runs);
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "Skiff's transport: median %lld KB/s (min %lld, max %lld) of %d",
             median, kb_per_s[0], kb_per_s[runs - 1], runs);
    check(b, all_ok, text);
    char values[SKIFF_SELFTEST_LINE_MAX];
    snprintf(values, sizeof values, "median_kbs=%lld min_kbs=%lld max_kbs=%lld runs=%d ok=%d",
             median, kb_per_s[0], kb_per_s[runs - 1], runs, all_ok);
    log_item(b, "app-transport", values);
}

/* ---- Runs ------------------------------------------------------------------------------------ */

static int wants(const bench *b, skiff_probe_section section) {
    return (b->config.sections & (unsigned)section) != 0 && !skiff_psp_exit_requested();
}

static void report_environment(bench *b) {
    char environment[ENVIRONMENT_MAX];
    describe_environment(b, environment, sizeof environment);
    char text[LINE_MAX_BENCH];
    snprintf(text, sizeof text, "environment: %s", environment);
    say(b, text);
}

/* No ARK: TLS cannot start. Checks the network modules and that libcurl refuses, then the parts
 * that need no network, on small sizes. */
static void run_without_ark(bench *b) {
    say(b, "no ARK: TLS cannot start; checking that it refuses, then CPU and Memory Stick code");
    b->smoke = 1;
    b->config.runs = 1;
    report_environment(b);
    skiff_psp_net net;
    const int loaded = skiff_probe_load_network(&b->report, &net);
    b->failures += loaded ? 0 : 1;
    const CURLcode curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "curl_global_init() refuses without entropy: %d", (int)curl_status);
    check(b, curl_status != CURLE_OK, text);
    if (curl_status == CURLE_OK) {
        curl_global_cleanup();
    }
    const skiff_err unload = skiff_psp_net_unload(&net);
    if (unload != SKIFF_OK) {
        skiff_probe_report_net_failure(&b->report, &net, unload);
        b->failures++;
    }
    run_cpu(b);
    run_memory_stick_blocks(b);
}

static int prepare_requests(bench *b) {
    snprintf(b->bearer, sizeof b->bearer, BEARER_PREFIX "%s", b->config.token);
    snprintf(b->authorization, sizeof b->authorization, AUTHORIZATION_HEADER ": %s", b->bearer);
    return skiff_probe_existing_sibling(&b->report, b->program_path, CA_FILE, b->ca);
}

static void run_sections(bench *b) {
    if (wants(b, SKIFF_PROBE_SECTION_LATENCY)) {
        run_latency(b);
    }
    if (wants(b, SKIFF_PROBE_SECTION_CPU)) {
        run_cpu(b);
    }
    if (wants(b, SKIFF_PROBE_SECTION_NET)) {
        run_net(b);
    }
    if (wants(b, SKIFF_PROBE_SECTION_MS)) {
        run_memory_stick_blocks(b);
        run_memory_stick_download(b);
    }
    if (wants(b, SKIFF_PROBE_SECTION_APP)) {
        run_app(b);
    }
}

static void run_with_ark(bench *b) {
    if (!skiff_probe_load_config(&b->report, b->program_path, BENCH_CONFIG_FILE, &b->config)) {
        b->failures++;
        return;
    }
    if (!skiff_probe_config_complete(&b->config)) {
        check(b, 0,
              "config: " BENCH_CONFIG_FILE " needs the server, the token and the seeded file");
        return;
    }
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text,
             "config: server %s, profile %d, %llu bytes, %d run(s), sections 0x%x", b->config.host,
             b->config.profile, b->config.size, b->config.runs, b->config.sections);
    check(b, 1, text);
    if (!prepare_requests(b)) {
        b->failures++;
        return;
    }
    skiff_psp_net net;
    if (!skiff_probe_load_network(&b->report, &net)) {
        b->failures++;
        skiff_psp_net_unload(&net);
        return;
    }
    const long long join_start = now_us();
    const skiff_err joined = skiff_psp_net_connect(&net, b->config.profile, WIFI_JOIN_TIMEOUT_US);
    b->joined_at_us = now_us();
    if (joined != SKIFF_OK) {
        skiff_probe_report_net_failure(&b->report, &net, joined);
        b->failures++;
        skiff_probe_tear_down(&b->report, &net, DISCONNECT_TIMEOUT_US);
        return;
    }
    b->joined = 1;
    snprintf(text, sizeof text, "Wi-Fi: profile %d joined in %lld ms", b->config.profile,
             (b->joined_at_us - join_start) / US_PER_MS);
    check(b, 1, text);
    const CURLcode curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    snprintf(text, sizeof text, "curl_global_init() = %d", (int)curl_status);
    check(b, curl_status == CURLE_OK, text);
    /* Right after joining first: the latency section is about what happens then. */
    report_environment(b);
    if (curl_status == CURLE_OK) {
        run_sections(b);
        curl_global_cleanup();
    }
    report_environment(b);
    b->joined = 0;
    b->failures += skiff_probe_tear_down(&b->report, &net, DISCONNECT_TIMEOUT_US) ? 0 : 1;
}

int main(int argc, char *argv[]) {
    static bench b;
    b.program_path = argc > 0 ? argv[0] : NULL;
    b.best_buffer_bytes = CURL_MAX_WRITE_SIZE;
    skiff_psp_install_exit_callback();
    skiff_psp_report_open(&b.report, b.program_path);
    say(&b, "Skiff benchmark");
    b.initial_cpu_mhz = scePowerGetCpuClockFrequency();
    b.initial_bus_mhz = scePowerGetBusClockFrequency();
    skiff_probe_config_defaults(&b.config);
    open_log(&b);
    const SceUID ticker = start_thread("skiff_bench_ticker", ticker_thread, TICKER_THREAD_PRIORITY,
                                       TICKER_THREAD_STACK_BYTES);
    const SceUID idle = start_thread("skiff_bench_idle", idle_thread, IDLE_THREAD_PRIORITY,
                                     IDLE_THREAD_STACK_BYTES);
    check(&b, ticker >= 0 && idle >= 0, "helper threads started (power tick, idle counter)");

    b.has_ark = skiff_psp_entropy_status() != SKIFF_ERR_NET_NEEDS_ARK;
    if (b.has_ark) {
        run_with_ark(&b);
    } else {
        run_without_ark(&b);
    }

    /* HOME → Quit ends the run between downloads: whatever is missing makes it incomplete. */
    if (skiff_psp_exit_requested()) {
        check(&b, 0, "stopped from the HOME menu: the results are incomplete");
    }
    stop_thread(idle, &idle_stop);
    stop_thread(ticker, &ticker_stop);
    close_log(&b);
    const int passed = b.failures == 0;
    say(&b, b.has_ark ? (passed ? BENCH_OK_MARKER : BENCH_FAIL_MARKER)
                      : (passed ? BENCH_NO_ARK_OK_MARKER : BENCH_NO_ARK_FAIL_MARKER));
    skiff_psp_report_close(&b.report);
    sceKernelExitGame();
    return passed ? 0 : 1;
}
