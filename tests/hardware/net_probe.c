/*
 * Network probe (Phase 1 hardware spike): the first HTTPS requests from a PSP, against the test
 * RomM from tests/integration (`scripts/dev.sh romm-lan`). It measures and checks what the
 * networking layer will rely on:
 *
 *   - free memory before and after the network modules load, after the access point connects, and
 *     the lowest free system memory and highest heap use while TLS requests run;
 *   - joining Wi-Fi through a saved Network Settings profile, with no dialog, and how long it
 * takes;
 *   - HTTPS to RomM's heartbeat, trusted through the test CA: TLS version and cipher suite, which
 *     must be TLS 1.3 or TLS 1.2 with ECDHE;
 *   - that a client without the test CA is refused;
 *   - client certificates (mTLS): refused without one and with one from an untrusted CA, accepted
 *     with the ECDSA P-256 and the RSA-2048 test certificates. Handshake times are the median of
 *     HANDSHAKE_RUNS fresh connections each, against plain HTTPS as the baseline;
 *   - keep-alive: a second request on the same handle reuses the connection (no new handshake);
 *   - plain HTTP for comparison.
 *
 * scripts/memstick.sh install writes net-probe.ini (the server's address and the profile number)
 * and copies the test CA and client certificates next to the EBOOT. Each run appends one line to
 * net-log.txt.
 *
 * Without ARK (PPSSPP in CI) TLS cannot start, since its entropy comes only from KIRK through ARK.
 * The probe then loads and unloads the network modules, checks that libcurl refuses to start, and
 * ends with SKIFF NET PROBE NO ARK OK.
 */
#include <curl/curl.h>
#include <malloc.h>
#include <mbedtls/ssl.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/selftest.h"

#include "kirk_entropy.h"
#include "lifecycle.h"
#include "net_psp.h"
#include "report.h"

#define PROBE_OK_MARKER "SKIFF NET PROBE OK"
#define PROBE_FAIL_MARKER "SKIFF NET PROBE FAIL"
#define PROBE_NO_ARK_OK_MARKER "SKIFF NET PROBE NO ARK OK"
#define PROBE_NO_ARK_FAIL_MARKER "SKIFF NET PROBE NO ARK FAIL"
#define PROBE_CONFIG_FILE "net-probe.ini"
#define PROBE_LOG_FILE "net-log.txt"
/* Copied next to the EBOOT by scripts/memstick.sh, from build/integration/certs/. */
#define CA_FILE "ca.crt"
#define ECDSA_CERT_FILE "client-ecdsa.crt"
#define ECDSA_KEY_FILE "client-ecdsa.key"
#define RSA_CERT_FILE "client-rsa.crt"
#define RSA_KEY_FILE "client-rsa.key"
/* An unrelated CA, and a client certificate it signed: neither is trusted by the test server. */
#define OTHER_CA_FILE "wrong-ca.crt"
#define WRONG_CA_CERT_FILE "client-wrong-ca.crt"
#define WRONG_CA_KEY_FILE "client-wrong-ca.key"
#define HEARTBEAT_PATH "/api/heartbeat"
/* RomM's heartbeat reports {"SYSTEM": {"VERSION": ...}}. */
#define HEARTBEAT_MARKER "\"VERSION\""
#define TLS_1_3 "TLSv1.3"
#define TLS_1_2 "TLSv1.2"
#define ECDHE "ECDHE"

enum {
    PATH_MAX_LEN = 256,
    HOST_MAX = 64,
    URL_MAX = 160,
    CONFIG_LINE_MAX = 160,
    BODY_EXCERPT_MAX = 256,
    TLS_NAME_MAX = 64,
    /* Request and run-log lines carry labels, curl's message and TLS names: longer than a check. */
    LONG_LINE_MAX = 512,
    DEFAULT_PROFILE = 1,
    DECIMAL = 10,
    HANDSHAKE_RUNS = 5,
    HTTP_PORT = 8080,
    HTTPS_PORT = 8443,
    MTLS_PORT = 8444,
    HTTP_OK = 200,
    BYTES_PER_KB = 1024,
    US_PER_MS = 1000,
    CURL_CONNECT_TIMEOUT_S = 10,
    CURL_TIMEOUT_S = 30,
};

#define WIFI_JOIN_TIMEOUT_US (30LL * 1000 * 1000)
#define DISCONNECT_TIMEOUT_US (10LL * 1000 * 1000)

typedef struct probe_config {
    int profile;
    char host[HOST_MAX];
    /* romm-lan keeps plain HTTP off the LAN unless asked; memstick.sh says which. */
    int plain_http;
} probe_config;

typedef struct memory_snapshot {
    size_t heap_used;
    SceSize system_free;
    SceSize system_largest;
} memory_snapshot;

typedef struct probe {
    skiff_psp_report report;
    const char *program_path;
    probe_config config;
    /* Lowest free system memory and highest heap use seen while requests ran. */
    SceSize system_free_low;
    size_t heap_peak;
} probe;

/* How a request must end. */
typedef enum expectation {
    EXPECT_HEARTBEAT,
    /* The client rejects the server's certificate: it trusts another CA. */
    EXPECT_UNTRUSTED_SERVER,
    /* TCP connects, then the server ends the TLS session before any HTTP response. */
    EXPECT_TLS_REFUSED,
} expectation;

/* One request: which site, which CA file to trust, and which client certificate to present. */
typedef struct request_spec {
    const char *label;
    int port;
    int https;
    const char *ca_file;
    const char *cert_file;
    const char *key_file;
    expectation expect;
} request_spec;

typedef struct request_result {
    /* A file was missing or curl refused an option: the request never tested the server. */
    int setup_failed;
    CURLcode code;
    long status;
    long long tcp_us;
    long long tls_us;
    long long total_us;
    int body_ok;
    char tls_version[TLS_NAME_MAX];
    char cipher[TLS_NAME_MAX];
} request_result;

/* Response bytes kept for the heartbeat check; the rest is counted only. */
typedef struct body_buffer {
    char data[BODY_EXCERPT_MAX];
    size_t length;
} body_buffer;

static memory_snapshot take_memory_snapshot(void) {
    const struct mallinfo heap = mallinfo();
    memory_snapshot snapshot = {(size_t)heap.uordblks, sceKernelTotalFreeMemSize(),
                                sceKernelMaxFreeMemSize()};
    return snapshot;
}

static void report_memory(probe *p, const char *label, const memory_snapshot *snapshot) {
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "memory %s: heap used %u KB, system free %u KB (largest %u KB)",
             label, (unsigned)(snapshot->heap_used / BYTES_PER_KB),
             (unsigned)(snapshot->system_free / BYTES_PER_KB),
             (unsigned)(snapshot->system_largest / BYTES_PER_KB));
    skiff_psp_report_line(&p->report, line);
}

static void track_memory(probe *p) {
    const memory_snapshot now = take_memory_snapshot();
    if (now.system_free < p->system_free_low) {
        p->system_free_low = now.system_free;
    }
    if (now.heap_used > p->heap_peak) {
        p->heap_peak = now.heap_used;
    }
}

static void report_check(probe *p, int ok, const char *text) {
    char line[LONG_LINE_MAX];
    snprintf(line, sizeof line, "%s %s", ok ? "ok  " : "FAIL", text);
    skiff_psp_report_line(&p->report, line);
}

static void report_net_failure(probe *p, const skiff_psp_net *net, skiff_err err) {
    char line[LONG_LINE_MAX];
    snprintf(line, sizeof line,
             "FAIL %s (%d): %s returned 0x%08X, access point state %d (furthest %d, firmware "
             "error 0x%08X)",
             skiff_err_name(err), (int)err, net->failed_call != NULL ? net->failed_call : "-",
             (unsigned)net->sce_result, net->apctl_state, net->apctl_furthest_state,
             (unsigned)net->apctl_error);
    skiff_psp_report_line(&p->report, line);
}

static int sibling(const probe *p, const char *file_name, char *out) {
    return skiff_selftest_sibling_path(p->program_path, file_name, out, PATH_MAX_LEN) == SKIFF_OK;
}

static void trim_line(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r' ||
                          text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
}

/* net-probe.ini: "key=value" lines, '#' comments. host is required, profile defaults to 1. */
static int read_config(probe *p) {
    char path[PATH_MAX_LEN];
    p->config.profile = DEFAULT_PROFILE;
    p->config.host[0] = '\0';
    FILE *file = sibling(p, PROBE_CONFIG_FILE, path) ? fopen(path, "r") : NULL;
    if (file == NULL) {
        report_check(p, 0,
                     PROBE_CONFIG_FILE " not found: run scripts/dev.sh romm-lan, then "
                                       "scripts/memstick.sh install");
        return 0;
    }
    char line[CONFIG_LINE_MAX];
    while (fgets(line, sizeof line, file) != NULL) {
        trim_line(line);
        char *equals = strchr(line, '=');
        if (line[0] == '#' || equals == NULL) {
            continue;
        }
        *equals = '\0';
        const char *value = equals + 1;
        if (strcmp(line, "host") == 0) {
            snprintf(p->config.host, sizeof p->config.host, "%s", value);
        } else if (strcmp(line, "plain_http") == 0) {
            p->config.plain_http = strcmp(value, "1") == 0;
        } else if (strcmp(line, "profile") == 0) {
            p->config.profile = (int)strtol(value, NULL, DECIMAL);
        }
    }
    fclose(file);
    char text[SKIFF_SELFTEST_LINE_MAX];
    const int ok = p->config.host[0] != '\0' && p->config.profile >= DEFAULT_PROFILE;
    snprintf(text, sizeof text, "config: server %s, Network Settings profile %d",
             p->config.host[0] != '\0' ? p->config.host : "(missing)", p->config.profile);
    report_check(p, ok, text);
    return ok;
}

static size_t keep_body(char *data, size_t size, size_t count, void *context) {
    body_buffer *body = context;
    const size_t bytes = size * count;
    const size_t room = sizeof body->data - 1 - body->length;
    const size_t kept = bytes < room ? bytes : room;
    memcpy(body->data + body->length, data, kept);
    body->length += kept;
    body->data[body->length] = '\0';
    return bytes;
}

/* The full path of a file next to the EBOOT, if it is there and readable. */
static int existing_sibling(probe *p, const char *file_name, char *out) {
    FILE *file = sibling(p, file_name, out) ? fopen(out, "r") : NULL;
    if (file == NULL) {
        char text[SKIFF_SELFTEST_LINE_MAX];
        snprintf(text, sizeof text,
                 "setup: %s missing next to the EBOOT (scripts/memstick.sh "
                 "install copies it)",
                 file_name);
        report_check(p, 0, text);
        return 0;
    }
    fclose(file);
    return 1;
}

/* Sets up `curl` for spec; returns 0 if a file it needs is missing or curl refuses an option. */
static int configure(probe *p, CURL *curl, const request_spec *spec, body_buffer *body, char *url) {
    char ca[PATH_MAX_LEN];
    char cert[PATH_MAX_LEN];
    char key[PATH_MAX_LEN];
    snprintf(url, URL_MAX, "%s://%s:%d%s", spec->https ? "https" : "http", p->config.host,
             spec->port, HEARTBEAT_PATH);
    int ok =
        curl_easy_setopt(curl, CURLOPT_URL, url) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)CURL_CONNECT_TIMEOUT_S) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)CURL_TIMEOUT_S) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, keep_body) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, body) == CURLE_OK;
    if (ok && spec->ca_file != NULL) {
        ok = existing_sibling(p, spec->ca_file, ca) &&
             curl_easy_setopt(curl, CURLOPT_CAINFO, ca) == CURLE_OK;
    }
    if (ok && spec->cert_file != NULL) {
        ok = existing_sibling(p, spec->cert_file, cert) &&
             existing_sibling(p, spec->key_file, key) &&
             curl_easy_setopt(curl, CURLOPT_SSLCERT, cert) == CURLE_OK &&
             curl_easy_setopt(curl, CURLOPT_SSLKEY, key) == CURLE_OK;
    }
    return ok;
}

/* After a transfer, while the connection is still cached in the handle. */
static void read_tls_session(CURL *curl, request_result *result) {
    const struct curl_tlssessioninfo *session = NULL;
    result->tls_version[0] = '\0';
    result->cipher[0] = '\0';
    if (curl_easy_getinfo(curl, CURLINFO_TLS_SSL_PTR, &session) != CURLE_OK || session == NULL ||
        session->backend != CURLSSLBACKEND_MBEDTLS || session->internals == NULL) {
        return;
    }
    const mbedtls_ssl_context *ssl = session->internals;
    snprintf(result->tls_version, sizeof result->tls_version, "%s", mbedtls_ssl_get_version(ssl));
    snprintf(result->cipher, sizeof result->cipher, "%s", mbedtls_ssl_get_ciphersuite(ssl));
}

static void read_result(CURL *curl, const body_buffer *body, CURLcode code,
                        request_result *result) {
    curl_off_t tcp = 0;
    curl_off_t app = 0;
    curl_off_t total = 0;
    result->code = code;
    result->status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->status);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &tcp);
    curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME_T, &app);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total);
    result->tcp_us = (long long)tcp;
    /* APPCONNECT counts from the start of the transfer; the TLS part is what follows TCP. */
    result->tls_us = app > tcp ? (long long)(app - tcp) : 0;
    result->total_us = (long long)total;
    result->body_ok = strstr(body->data, HEARTBEAT_MARKER) != NULL;
    read_tls_session(curl, result);
}

static int request_ok(const request_result *result) {
    return !result->setup_failed && result->code == CURLE_OK && result->status == HTTP_OK &&
           result->body_ok;
}

/* Whether the request ended the way spec says it must. */
static int as_expected(const request_spec *spec, const request_result *result) {
    if (result->setup_failed) {
        return 0;
    }
    switch (spec->expect) {
    case EXPECT_HEARTBEAT:
        return request_ok(result);
    case EXPECT_UNTRUSTED_SERVER:
        return result->code == CURLE_PEER_FAILED_VERIFICATION;
    case EXPECT_TLS_REFUSED:
        /* TLS 1.2 fails the handshake; TLS 1.3 reports the server's alert on the first read. */
        return result->tcp_us > 0 && result->status == 0 &&
               (result->code == CURLE_SSL_CONNECT_ERROR || result->code == CURLE_RECV_ERROR);
    }
    return 0;
}

static void log_request(probe *p, const request_spec *spec, const request_result *result, int ok) {
    char line[LONG_LINE_MAX];
    snprintf(line, sizeof line,
             "%s %s: curl %d (%s), HTTP %ld, TCP %lld ms, TLS %lld ms, total %lld ms %s %s",
             ok ? "ok  " : "FAIL", spec->label, (int)result->code, curl_easy_strerror(result->code),
             result->status, result->tcp_us / US_PER_MS, result->tls_us / US_PER_MS,
             result->total_us / US_PER_MS, result->tls_version, result->cipher);
    skiff_psp_report_line(&p->report, line);
}

/* One fresh connection (a full handshake). */
static void run_request(probe *p, const request_spec *spec, request_result *result) {
    memset(result, 0, sizeof *result);
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        result->code = CURLE_OUT_OF_MEMORY;
        return;
    }
    body_buffer body = {{0}, 0};
    char url[URL_MAX];
    if (!configure(p, curl, spec, &body, url)) {
        result->setup_failed = 1;
    } else {
        read_result(curl, &body, curl_easy_perform(curl), result);
    }
    track_memory(p);
    curl_easy_cleanup(curl);
}

static int check_request(probe *p, const request_spec *spec, request_result *result) {
    run_request(p, spec, result);
    const int ok = as_expected(spec, result);
    log_request(p, spec, result, ok);
    return ok;
}

static int compare_long_long(const void *a, const void *b) {
    const long long left = *(const long long *)a;
    const long long right = *(const long long *)b;
    return (left > right) - (left < right);
}

/* HANDSHAKE_RUNS fresh connections; the median TLS handshake time, or -1 if any run failed. */
static long long median_handshake(probe *p, const request_spec *spec) {
    long long tls_us[HANDSHAKE_RUNS];
    int all_ok = 1;
    for (int run = 0; run < HANDSHAKE_RUNS; run++) {
        request_result result;
        all_ok &= check_request(p, spec, &result);
        tls_us[run] = result.tls_us;
    }
    qsort(tls_us, HANDSHAKE_RUNS, sizeof tls_us[0], compare_long_long);
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "%s %s: median TLS handshake %lld ms of %d (min %lld, max %lld)",
             all_ok ? "ok  " : "FAIL", spec->label, tls_us[HANDSHAKE_RUNS / 2] / US_PER_MS,
             HANDSHAKE_RUNS, tls_us[0] / US_PER_MS, tls_us[HANDSHAKE_RUNS - 1] / US_PER_MS);
    skiff_psp_report_line(&p->report, line);
    return all_ok ? tls_us[HANDSHAKE_RUNS / 2] : -1;
}

/* TLS 1.3, or TLS 1.2 with an ECDHE key exchange (forward secrecy). */
static int check_tls_parameters(probe *p, const request_result *result) {
    const int tls13 = strcmp(result->tls_version, TLS_1_3) == 0;
    const int tls12_ecdhe =
        strcmp(result->tls_version, TLS_1_2) == 0 && strstr(result->cipher, ECDHE) != NULL;
    char text[LONG_LINE_MAX];
    snprintf(text, sizeof text, "TLS parameters: %s, %s (need TLS 1.3, or 1.2 with ECDHE)",
             result->tls_version[0] != '\0' ? result->tls_version : "unknown",
             result->cipher[0] != '\0' ? result->cipher : "unknown");
    report_check(p, tls13 || tls12_ecdhe, text);
    return tls13 || tls12_ecdhe;
}

/* Two heartbeats on one handle: both must succeed, the second on the first's connection. */
static int check_keep_alive(probe *p, const request_spec *spec) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        report_check(p, 0, "keep-alive: curl_easy_init failed");
        return 0;
    }
    body_buffer body = {{0}, 0};
    char url[URL_MAX];
    request_result first;
    request_result second;
    memset(&first, 0, sizeof first);
    memset(&second, 0, sizeof second);
    long new_connections = -1;
    const int configured = configure(p, curl, spec, &body, url);
    if (configured) {
        read_result(curl, &body, curl_easy_perform(curl), &first);
        memset(&body, 0, sizeof body);
        read_result(curl, &body, curl_easy_perform(curl), &second);
        curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &new_connections);
    }
    track_memory(p);
    curl_easy_cleanup(curl);
    const int ok = configured && request_ok(&first) && request_ok(&second) && new_connections == 0;
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text,
             "keep-alive: HTTP %ld then %ld, new connections on the second: %ld (TLS %lld ms)",
             first.status, second.status, new_connections, second.tls_us / US_PER_MS);
    report_check(p, ok, text);
    return ok;
}

typedef struct tls_findings {
    request_result https;
    long long https_median_us;
    long long ecdsa_median_us;
    long long rsa_median_us;
    long long http_total_us;
} tls_findings;

static int run_requests(probe *p, tls_findings *findings) {
    const request_spec https = {
        .label = "HTTPS, test CA", .port = HTTPS_PORT, .https = 1, .ca_file = CA_FILE};
    /* Trusting another CA, so verification really runs and must fail on the server's certificate.
     */
    const request_spec other_ca = {.label = "HTTPS trusting another CA (must be refused)",
                                   .port = HTTPS_PORT,
                                   .https = 1,
                                   .ca_file = OTHER_CA_FILE,
                                   .expect = EXPECT_UNTRUSTED_SERVER};
    const request_spec mtls_none = {.label = "mTLS without a certificate (must be refused)",
                                    .port = MTLS_PORT,
                                    .https = 1,
                                    .ca_file = CA_FILE,
                                    .expect = EXPECT_TLS_REFUSED};
    const request_spec mtls_wrong = {.label = "mTLS, untrusted CA's certificate (must be refused)",
                                     .port = MTLS_PORT,
                                     .https = 1,
                                     .ca_file = CA_FILE,
                                     .cert_file = WRONG_CA_CERT_FILE,
                                     .key_file = WRONG_CA_KEY_FILE,
                                     .expect = EXPECT_TLS_REFUSED};
    const request_spec mtls_ecdsa = {.label = "mTLS, ECDSA P-256",
                                     .port = MTLS_PORT,
                                     .https = 1,
                                     .ca_file = CA_FILE,
                                     .cert_file = ECDSA_CERT_FILE,
                                     .key_file = ECDSA_KEY_FILE};
    const request_spec mtls_rsa = {.label = "mTLS, RSA-2048",
                                   .port = MTLS_PORT,
                                   .https = 1,
                                   .ca_file = CA_FILE,
                                   .cert_file = RSA_CERT_FILE,
                                   .key_file = RSA_KEY_FILE};
    const request_spec http = {.label = "plain HTTP", .port = HTTP_PORT};

    int ok = check_request(p, &https, &findings->https);
    ok &= check_tls_parameters(p, &findings->https);
    request_result refused;
    ok &= check_request(p, &other_ca, &refused);
    ok &= check_request(p, &mtls_none, &refused);
    ok &= check_request(p, &mtls_wrong, &refused);
    findings->https_median_us = median_handshake(p, &https);
    findings->ecdsa_median_us = median_handshake(p, &mtls_ecdsa);
    findings->rsa_median_us = median_handshake(p, &mtls_rsa);
    ok &= findings->https_median_us >= 0 && findings->ecdsa_median_us >= 0 &&
          findings->rsa_median_us >= 0;
    ok &= check_keep_alive(p, &https);
    if (p->config.plain_http) {
        request_result plain;
        ok &= check_request(p, &http, &plain);
        findings->http_total_us = plain.total_us;
    } else {
        findings->http_total_us = -1;
        skiff_psp_report_line(&p->report,
                              "skip plain HTTP: the server keeps it local (SKIFF_LAN_PLAIN_HTTP=1 "
                              "scripts/dev.sh romm-lan, then install again, to compare)");
    }
    return ok;
}

static int append_log(probe *p, const char *ip, long long join_us, const tls_findings *findings) {
    char path[PATH_MAX_LEN];
    FILE *file = sibling(p, PROBE_LOG_FILE, path) ? fopen(path, "a") : NULL;
    if (file == NULL) {
        report_check(p, 0, "run log: could not open " PROBE_LOG_FILE);
        return 0;
    }
    char line[LONG_LINE_MAX];
    snprintf(line, sizeof line,
             "ip=%s join_ms=%lld tls=%s cipher=%s https_tls_ms=%lld ecdsa_tls_ms=%lld "
             "rsa_tls_ms=%lld http_total_ms=%lld system_free_low_kb=%u heap_peak_kb=%u",
             ip, join_us / US_PER_MS, findings->https.tls_version, findings->https.cipher,
             findings->https_median_us / US_PER_MS, findings->ecdsa_median_us / US_PER_MS,
             findings->rsa_median_us / US_PER_MS,
             findings->http_total_us < 0 ? -1LL : findings->http_total_us / US_PER_MS,
             (unsigned)(p->system_free_low / BYTES_PER_KB),
             (unsigned)(p->heap_peak / BYTES_PER_KB));
    const int written = fprintf(file, "%s\n", line) > 0;
    const int closed = fclose(file) == 0;
    report_check(p, written && closed, "run appended to " PROBE_LOG_FILE);
    return written && closed;
}

/* Disconnects, then unloads only if the disconnect is proven: never under a live connection. */
static int tear_down(probe *p, skiff_psp_net *net) {
    skiff_err err = skiff_psp_net_disconnect(net, DISCONNECT_TIMEOUT_US);
    if (err != SKIFF_OK) {
        report_net_failure(p, net, err);
        report_check(p, 0, "network: not disconnected, leaving the modules loaded");
        return 0;
    }
    err = skiff_psp_net_unload(net);
    if (err != SKIFF_OK) {
        report_net_failure(p, net, err);
        return 0;
    }
    memory_snapshot after = take_memory_snapshot();
    report_memory(p, "after the network modules unload", &after);
    report_check(p, 1, "network: disconnected and modules unloaded");
    return 1;
}

static int load_network(probe *p, skiff_psp_net *net) {
    const memory_snapshot before = take_memory_snapshot();
    report_memory(p, "before the network modules", &before);
    const skiff_err err = skiff_psp_net_load(net);
    if (err != SKIFF_OK) {
        report_net_failure(p, net, err);
        return 0;
    }
    const memory_snapshot after = take_memory_snapshot();
    report_memory(p, "after the network modules load", &after);
    return 1;
}

/* No ARK: the network stack still loads, but TLS (and so libcurl) must refuse to start. */
static int run_without_ark(probe *p) {
    skiff_psp_report_line(&p->report, "no ARK: TLS cannot start; checking that it refuses");
    skiff_psp_net net;
    const int loaded = load_network(p, &net);
    const CURLcode curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "curl_global_init() refuses without entropy: %d", (int)curl_status);
    report_check(p, curl_status != CURLE_OK, text);
    if (curl_status == CURLE_OK) {
        curl_global_cleanup();
    }
    const skiff_err unload = skiff_psp_net_unload(&net);
    if (unload != SKIFF_OK) {
        report_net_failure(p, &net, unload);
    }
    return loaded && curl_status != CURLE_OK && unload == SKIFF_OK;
}

static int run_with_ark(probe *p) {
    if (!read_config(p)) {
        return 0;
    }
    skiff_psp_net net;
    if (!load_network(p, &net)) {
        skiff_psp_net_unload(&net);
        return 0;
    }

    const long long join_start_us = sceKernelGetSystemTimeWide();
    const skiff_err joined = skiff_psp_net_connect(&net, p->config.profile, WIFI_JOIN_TIMEOUT_US);
    const long long join_us = sceKernelGetSystemTimeWide() - join_start_us;
    char ip[SKIFF_PSP_NET_IP_MAX] = "";
    char text[SKIFF_SELFTEST_LINE_MAX];
    if (joined != SKIFF_OK) {
        report_net_failure(p, &net, joined);
        snprintf(text, sizeof text, "Wi-Fi: profile %d gave up after %lld ms", p->config.profile,
                 join_us / US_PER_MS);
        skiff_psp_report_line(&p->report, text);
        tear_down(p, &net);
        return 0;
    }
    skiff_psp_net_ip(&net, ip, sizeof ip);
    snprintf(text, sizeof text, "Wi-Fi: profile %d joined in %lld ms, IP %s", p->config.profile,
             join_us / US_PER_MS, ip);
    report_check(p, 1, text);
    const memory_snapshot connected = take_memory_snapshot();
    report_memory(p, "after joining", &connected);
    p->system_free_low = connected.system_free;
    p->heap_peak = connected.heap_used;

    const long long init_start_us = sceKernelGetSystemTimeWide();
    const CURLcode curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    snprintf(text, sizeof text, "curl_global_init() = %d in %lld ms", (int)curl_status,
             (sceKernelGetSystemTimeWide() - init_start_us) / US_PER_MS);
    report_check(p, curl_status == CURLE_OK, text);

    tls_findings findings;
    memset(&findings, 0, sizeof findings);
    int ok = curl_status == CURLE_OK && run_requests(p, &findings);
    if (curl_status == CURLE_OK) {
        curl_global_cleanup();
    }
    snprintf(text, sizeof text, "memory during requests: system free low %u KB, heap peak %u KB",
             (unsigned)(p->system_free_low / BYTES_PER_KB),
             (unsigned)(p->heap_peak / BYTES_PER_KB));
    skiff_psp_report_line(&p->report, text);

    const skiff_err entropy = skiff_psp_entropy_status();
    snprintf(text, sizeof text, "entropy status after the requests: %s", skiff_err_name(entropy));
    report_check(p, entropy == SKIFF_OK, text);
    ok &= entropy == SKIFF_OK;

    ok &= append_log(p, ip, join_us, &findings);
    ok &= tear_down(p, &net);
    return ok;
}

int main(int argc, char *argv[]) {
    probe p;
    memset(&p, 0, sizeof p);
    p.program_path = argc > 0 ? argv[0] : NULL;
    skiff_psp_install_exit_callback();
    skiff_psp_report_open(&p.report, p.program_path);
    skiff_psp_report_line(&p.report, "Skiff network probe");

    const int has_ark = skiff_psp_entropy_status() != SKIFF_ERR_NET_NEEDS_ARK;
    const int passed = has_ark ? run_with_ark(&p) : run_without_ark(&p);
    const char *marker = has_ark ? (passed ? PROBE_OK_MARKER : PROBE_FAIL_MARKER)
                                 : (passed ? PROBE_NO_ARK_OK_MARKER : PROBE_NO_ARK_FAIL_MARKER);
    skiff_psp_report_line(&p.report, marker);
    skiff_psp_report_close(&p.report);
    sceKernelExitGame();
    return passed ? 0 : 1;
}
