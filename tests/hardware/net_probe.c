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
 *   - plain HTTP for comparison;
 *   - Skiff's own network layer (skiff_net, include/skiff/curl_transport.h) on the PSP: keep-alive,
 *     downloading the seeded file and resuming it with Range and If-Range (current and stale ETag),
 *     and a clock forced back to 2000 reported as SKIFF_ERR_NET_TLS_CLOCK (108).
 *
 * scripts/memstick.sh install writes net-probe.ini (the server's address, the profile number, and
 * the seeded file and API token from the test server) and copies the test CA and client
 * certificates next to the EBOOT. Each run appends one line to net-log.txt.
 *
 * Without ARK (PPSSPP in CI) TLS cannot start, since its entropy comes only from KIRK through ARK.
 * The probe then loads and unloads the network modules, checks that libcurl refuses to start, and
 * ends with SKIFF NET PROBE NO ARK OK.
 */
#include <curl/curl.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/ssl.h>
#include <pspkernel.h>
#include <psputility.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "skiff/curl_transport.h"
#include "skiff/selftest.h"

#include "kirk_entropy.h"
#include "lifecycle.h"
#include "net_psp.h"
#include "probe_psp.h"
#include "probe_support.h"
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
    PATH_MAX_LEN = SKIFF_PROBE_PATH_MAX,
    URL_MAX = 160,
    BODY_EXCERPT_MAX = 256,
    TLS_NAME_MAX = SKIFF_PROBE_TLS_NAME_MAX,
    TOKEN_MAX = SKIFF_PROBE_TOKEN_MAX,
    /* Resume the seeded file from here; the bytes after it are compared between both downloads. */
    RESUME_OFFSET = 1000,
    CLOCK_TEXT_MAX = 32,
    LONG_LINE_MAX = SKIFF_PROBE_LONG_LINE_MAX,
    /* https://<host>:8443/api/roms/<id>/content/<URL-encoded file name> */
    CONTENT_URL_MAX = 320,
    HANDSHAKE_RUNS = 5,
    HTTP_PORT = 8080,
    HTTPS_PORT = 8443,
    MTLS_PORT = 8444,
    HTTP_OK = 200,
    HTTP_STATUS_PARTIAL = 206,
    BYTES_PER_KB = 1024,
    US_PER_MS = 1000,
    CURL_CONNECT_TIMEOUT_S = 10,
    CURL_TIMEOUT_S = 30,
};

#define WIFI_JOIN_TIMEOUT_US (30LL * 1000 * 1000)
/* A PSP whose battery ran flat: its clock restarts years before any certificate was issued. */
#define CLOCK_RESET_TO_2000 ((mbedtls_time_t)946684800)
#define STALE_ETAG "\"stale\""
#define BEARER_PREFIX "Bearer "
/* FNV-1a, to compare the bytes of two downloads without keeping either in memory. */
#define FNV_OFFSET_BASIS 2166136261U
#define FNV_PRIME 16777619U
#define DISCONNECT_TIMEOUT_US (10LL * 1000 * 1000)

typedef struct probe {
    skiff_psp_report report;
    const char *program_path;
    skiff_probe_config config;
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

/* Response bytes kept for the heartbeat check, and the request they belong to. */
typedef struct body_buffer {
    char data[BODY_EXCERPT_MAX];
    size_t length;
    CURL *curl;
    request_result *result;
} body_buffer;

static void track_memory(probe *p) {
    const skiff_probe_memory now = skiff_probe_memory_now();
    if (now.system_free < p->system_free_low) {
        p->system_free_low = now.system_free;
    }
    if (now.heap_used > p->heap_peak) {
        p->heap_peak = now.heap_used;
    }
}

static void report_check(probe *p, int ok, const char *text) {
    skiff_probe_report_check(&p->report, ok, text);
}

static int sibling(const probe *p, const char *file_name, char *out) {
    return skiff_probe_sibling(p->program_path, file_name, out);
}

/* net-probe.ini (tests/hardware/probe_support.h): the server and the seeded file are required. */
static int read_config(probe *p) {
    if (!skiff_probe_load_config(&p->report, p->program_path, PROBE_CONFIG_FILE, &p->config)) {
        return 0;
    }
    char text[SKIFF_SELFTEST_LINE_MAX];
    const int ok = skiff_probe_config_complete(&p->config) && p->config.size > RESUME_OFFSET;
    snprintf(text, sizeof text,
             "config: server %s, Network Settings profile %d, ROM %s (%llu bytes)",
             p->config.host[0] != '\0' ? p->config.host : "(missing)", p->config.profile,
             p->config.rom_id[0] != '\0' ? p->config.rom_id : "(missing)", p->config.size);
    report_check(p, ok, text);
    return ok;
}

static void read_tls_session(CURL *curl, request_result *result);

static size_t keep_body(char *data, size_t size, size_t count, void *context) {
    body_buffer *body = context;
    /* libcurl exposes the TLS session only while the transfer runs, so it is read here. */
    if (body->result != NULL && body->result->tls_version[0] == '\0') {
        read_tls_session(body->curl, body->result);
    }
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
    return skiff_probe_existing_sibling(&p->report, p->program_path, file_name, out);
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

static void read_tls_session(CURL *curl, request_result *result) {
    skiff_probe_read_tls_session(curl, result->tls_version, result->cipher);
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
    body_buffer body = {{0}, 0, curl, result};
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

/* HANDSHAKE_RUNS fresh connections; the median TLS handshake time, or -1 if any run failed. */
static long long median_handshake(probe *p, const request_spec *spec) {
    long long tls_us[HANDSHAKE_RUNS];
    int all_ok = 1;
    for (int run = 0; run < HANDSHAKE_RUNS; run++) {
        request_result result;
        all_ok &= check_request(p, spec, &result);
        tls_us[run] = result.tls_us;
    }
    const long long median_us = skiff_probe_median(tls_us, HANDSHAKE_RUNS);
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "%s %s: median TLS handshake %lld ms of %d (min %lld, max %lld)",
             all_ok ? "ok  " : "FAIL", spec->label, median_us / US_PER_MS, HANDSHAKE_RUNS,
             tls_us[0] / US_PER_MS, tls_us[HANDSHAKE_RUNS - 1] / US_PER_MS);
    skiff_psp_report_line(&p->report, line);
    return all_ok ? median_us : -1;
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
    request_result first;
    request_result second;
    memset(&first, 0, sizeof first);
    memset(&second, 0, sizeof second);
    body_buffer body = {{0}, 0, curl, &first};
    char url[URL_MAX];
    long new_connections = -1;
    const int configured = configure(p, curl, spec, &body, url);
    if (configured) {
        read_result(curl, &body, curl_easy_perform(curl), &first);
        body.length = 0;
        body.data[0] = '\0';
        body.result = &second;
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

/* A download hashed as it arrives: every byte of the body (whole), and the bytes from file offset
 * RESUME_OFFSET on (suffix), which a resumed 206 must match. */
typedef struct download_digest {
    const skiff_http_response *response;
    int started;
    unsigned long long position; /* file offset of the next byte */
    uint32_t whole;
    uint32_t suffix;
    /* CRC-32 of the whole body, against the seeded file's: checks the download against the
     * source, not just against itself. */
    uint32_t crc32;
} download_digest;

/* The status and headers are parsed before the first chunk: a 206 starts where its Content-Range
 * says, anything else (a whole file, also after a stale If-Range) at byte 0. */
static skiff_err digest_body(void *ctx, const unsigned char *data, size_t size) {
    download_digest *digest = ctx;
    if (!digest->started) {
        digest->started = 1;
        digest->position = digest->response->has_content_range ? digest->response->range_start : 0;
    }
    digest->crc32 = skiff_probe_crc32_bitwise(digest->crc32, data, size);
    for (size_t i = 0; i < size; i++, digest->position++) {
        digest->whole = (digest->whole ^ data[i]) * FNV_PRIME;
        if (digest->position >= RESUME_OFFSET) {
            digest->suffix = (digest->suffix ^ data[i]) * FNV_PRIME;
        }
    }
    return SKIFF_OK;
}

/* One request through skiff_net; with resume, from RESUME_OFFSET with if_range. */
static skiff_err transport_get(probe *p, skiff_transport *transport, const char *url, int resume,
                               const char *if_range, download_digest *digest,
                               skiff_http_response *response, long long *elapsed_us) {
    digest->response = response;
    digest->started = 0;
    digest->position = 0;
    digest->whole = FNV_OFFSET_BASIS;
    digest->suffix = FNV_OFFSET_BASIS;
    digest->crc32 = 0;
    const skiff_http_request request = {.url = url,
                                        .has_range = resume,
                                        .range_start = resume ? RESUME_OFFSET : 0,
                                        .if_range = if_range,
                                        .on_body = digest_body,
                                        .body_ctx = digest};
    const long long start_us = sceKernelGetSystemTimeWide();
    const skiff_err err = skiff_transport_perform(transport, &request, response);
    *elapsed_us = sceKernelGetSystemTimeWide() - start_us;
    track_memory(p);
    return err;
}

static int report_transport(probe *p, int ok, const char *label, skiff_err err,
                            const skiff_http_response *response, long long elapsed_us) {
    char text[LONG_LINE_MAX];
    snprintf(text, sizeof text,
             "skiff_net %s: %s, HTTP %ld, %llu bytes, %ld new connection(s), %lld ms", label,
             skiff_err_name(err), response->status, (unsigned long long)response->body_bytes,
             response->new_connections, elapsed_us / US_PER_MS);
    report_check(p, ok, text);
    return ok;
}

/* A fresh transport (no kept connection, no TLS session) trusting the test CA, with the token. */
static skiff_transport *create_transport(probe *p, const char *authorization) {
    char ca[PATH_MAX_LEN];
    if (!existing_sibling(p, CA_FILE, ca)) {
        return NULL;
    }
    const skiff_http_header headers[] = {{"Authorization", authorization}};
    const skiff_curl_config config = {
        .ca_file = ca, .default_headers = headers, .default_header_count = 1};
    skiff_transport *transport = NULL;
    const skiff_err err = skiff_curl_transport_create(&config, &transport);
    if (err != SKIFF_OK) {
        char text[SKIFF_SELFTEST_LINE_MAX];
        snprintf(text, sizeof text, "skiff_net: creating the transport failed: %s",
                 skiff_err_name(err));
        report_check(p, 0, text);
    }
    return transport;
}

static mbedtls_time_t clock_reset_to_2000(mbedtls_time_t *out) {
    if (out != NULL) {
        *out = CLOCK_RESET_TO_2000;
    }
    return CLOCK_RESET_TO_2000;
}

/* The app's transport on the PSP: keep-alive, a whole download, resuming it, and the clock rule. */
static int run_transport_checks(probe *p, long long *download_us) {
    const skiff_probe_config *config = &p->config;
    char authorization[TOKEN_MAX + sizeof BEARER_PREFIX];
    char heartbeat[URL_MAX];
    char content[CONTENT_URL_MAX];
    snprintf(authorization, sizeof authorization, BEARER_PREFIX "%s", config->token);
    snprintf(heartbeat, sizeof heartbeat, "https://%s:%d" HEARTBEAT_PATH, config->host, HTTPS_PORT);
    snprintf(content, sizeof content, "https://%s:%d/api/roms/%s/content/%s", config->host,
             HTTPS_PORT, config->rom_id, config->file_name);
    skiff_transport *transport = create_transport(p, authorization);
    if (transport == NULL) {
        return 0;
    }
    skiff_http_response response;
    download_digest digest;
    long long elapsed_us = 0;

    skiff_err err =
        transport_get(p, transport, heartbeat, 0, NULL, &digest, &response, &elapsed_us);
    int ok = report_transport(p, err == SKIFF_OK && response.status == HTTP_OK, "heartbeat", err,
                              &response, elapsed_us);
    err = transport_get(p, transport, heartbeat, 0, NULL, &digest, &response, &elapsed_us);
    ok &= report_transport(
        p, err == SKIFF_OK && response.status == HTTP_OK && response.new_connections == 0,
        "heartbeat on the kept connection", err, &response, elapsed_us);

    err = transport_get(p, transport, content, 0, NULL, &digest, &response, download_us);
    char etag[SKIFF_HTTP_ETAG_MAX];
    snprintf(etag, sizeof etag, "%s", response.etag);
    const download_digest whole_file = digest;
    ok &= report_transport(
        p,
        err == SKIFF_OK && response.status == HTTP_OK && response.body_bytes == config->size &&
            etag[0] != '\0' && digest.crc32 == config->crc32,
        "whole download (CRC32 of the seeded file)", err, &response, *download_us);
    char text[LONG_LINE_MAX];
    snprintf(text, sizeof text,
             "skiff_net download speed: %llu KB/s, ETag %s, CRC32 %08lx (seeded %08lx)",
             skiff_probe_kb_per_s(response.body_bytes, *download_us),
             etag[0] != '\0' ? etag : "(none)", (unsigned long)whole_file.crc32,
             (unsigned long)config->crc32);
    skiff_psp_report_line(&p->report, text);

    err = transport_get(p, transport, content, 1, etag, &digest, &response, &elapsed_us);
    ok &= report_transport(
        p,
        err == SKIFF_OK && response.status == HTTP_STATUS_PARTIAL && response.has_content_range &&
            response.range_start == RESUME_OFFSET &&
            response.body_bytes == config->size - RESUME_OFFSET &&
            digest.suffix == whole_file.suffix,
        "resume with the current ETag (206, same bytes)", err, &response, elapsed_us);
    err = transport_get(p, transport, content, 1, STALE_ETAG, &digest, &response, &elapsed_us);
    ok &= report_transport(
        p,
        err == SKIFF_OK && response.status == HTTP_OK && response.body_bytes == config->size &&
            digest.whole == whole_file.whole,
        "resume with a stale ETag (whole file again)", err, &response, elapsed_us);
    skiff_transport_destroy(transport);

    /* The real clock goes back whatever happens: later requests verify certificates against it. */
    mbedtls_time_t (*const psp_clock)(mbedtls_time_t *) = mbedtls_time;
    mbedtls_platform_set_time(clock_reset_to_2000);
    transport = create_transport(p, authorization);
    err = transport != NULL
              ? transport_get(p, transport, heartbeat, 0, NULL, &digest, &response, &elapsed_us)
              : SKIFF_ERR_NO_MEMORY;
    mbedtls_platform_set_time(psp_clock);
    skiff_transport_destroy(transport);
    ok &= report_transport(p, err == SKIFF_ERR_NET_TLS_CLOCK, "clock forced to 2000 (must be 108)",
                           err, &response, elapsed_us);
    return ok;
}

typedef struct tls_findings {
    request_result https;
    long long https_median_us;
    long long ecdsa_median_us;
    long long rsa_median_us;
    long long http_total_us;
    long long download_us;
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
    ok &= run_transport_checks(p, &findings->download_us);
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
             "rsa_tls_ms=%lld http_total_ms=%lld download_ms=%lld system_free_low_kb=%u "
             "heap_peak_kb=%u",
             ip, join_us / US_PER_MS, findings->https.tls_version, findings->https.cipher,
             findings->https_median_us / US_PER_MS, findings->ecdsa_median_us / US_PER_MS,
             findings->rsa_median_us / US_PER_MS,
             findings->http_total_us < 0 ? -1LL : findings->http_total_us / US_PER_MS,
             findings->download_us / US_PER_MS, (unsigned)(p->system_free_low / BYTES_PER_KB),
             (unsigned)(p->heap_peak / BYTES_PER_KB));
    const int written = fprintf(file, "%s\n", line) > 0;
    const int closed = fclose(file) == 0;
    report_check(p, written && closed, "run appended to " PROBE_LOG_FILE);
    return written && closed;
}

/* No ARK: the network stack still loads, but TLS (and so libcurl) must refuse to start. */
static int run_without_ark(probe *p) {
    skiff_psp_report_line(&p->report, "no ARK: TLS cannot start; checking that it refuses");
    skiff_psp_net net;
    const int loaded = skiff_probe_load_network(&p->report, &net, SKIFF_PSP_NET_CPU_MHZ);
    const CURLcode curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    char text[SKIFF_SELFTEST_LINE_MAX];
    snprintf(text, sizeof text, "curl_global_init() refuses without entropy: %d", (int)curl_status);
    report_check(p, curl_status != CURLE_OK, text);
    if (curl_status == CURLE_OK) {
        curl_global_cleanup();
    }
    const int unloaded = skiff_probe_unload_network(&p->report, &net);
    return loaded && curl_status != CURLE_OK && unloaded;
}

static int run_with_ark(probe *p) {
    if (!read_config(p)) {
        return 0;
    }
    skiff_psp_net net;
    if (!skiff_probe_load_network(&p->report, &net, SKIFF_PSP_NET_CPU_MHZ)) {
        skiff_psp_net_unload(&net);
        return 0;
    }

    const long long join_start_us = sceKernelGetSystemTimeWide();
    const skiff_err joined = skiff_psp_net_connect(&net, p->config.profile, WIFI_JOIN_TIMEOUT_US);
    const long long join_us = sceKernelGetSystemTimeWide() - join_start_us;
    char ip[SKIFF_PSP_NET_IP_MAX] = "";
    char text[SKIFF_SELFTEST_LINE_MAX];
    if (joined != SKIFF_OK) {
        skiff_probe_report_net_failure(&p->report, &net, joined);
        snprintf(text, sizeof text, "Wi-Fi: profile %d gave up after %lld ms", p->config.profile,
                 join_us / US_PER_MS);
        skiff_psp_report_line(&p->report, text);
        skiff_probe_tear_down(&p->report, &net, DISCONNECT_TIMEOUT_US);
        return 0;
    }
    skiff_psp_net_ip(&net, ip, sizeof ip);
    snprintf(text, sizeof text, "Wi-Fi: profile %d joined in %lld ms, IP %s", p->config.profile,
             join_us / US_PER_MS, ip);
    report_check(p, 1, text);
    const skiff_probe_memory connected = skiff_probe_memory_now();
    skiff_probe_report_memory(&p->report, "after joining", &connected);
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
    ok &= skiff_probe_tear_down(&p->report, &net, DISCONNECT_TIMEOUT_US);
    return ok;
}

static void report_one_clock(probe *p, const char *label, time_t now) {
    struct tm utc;
    char clock_text[CLOCK_TEXT_MAX] = "unknown";
    if (gmtime_r(&now, &utc) != NULL) {
        strftime(clock_text, sizeof clock_text, "%Y-%m-%d %H:%M:%S", &utc);
    }
    char line[SKIFF_SELFTEST_LINE_MAX];
    snprintf(line, sizeof line, "%s: %s UTC (%lld)", label, clock_text, (long long)now);
    skiff_psp_report_line(&p->report, line);
}

/*
 * Certificates are valid between two dates, checked against Mbed TLS's clock (the PSP's real-time
 * clock, src/platform/psp/mbedtls_time.c). The C library's time() is shown next to it: on the PSP
 * it carries only the time of day.
 */
static void report_clock(probe *p) {
    report_one_clock(p, "TLS clock (certificate dates)", (time_t)mbedtls_time(NULL));
    report_one_clock(p, "C library time()", time(NULL));
    /* The RTC holds what the PSP takes for UTC: a wrong time zone setting shifts it. */
    int zone_minutes = 0;
    int daylight_saving = 0;
    char line[SKIFF_SELFTEST_LINE_MAX];
    if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_TIMEZONE, &zone_minutes) >= 0 &&
        sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_DAYLIGHTSAVINGS, &daylight_saving) >=
            0) {
        snprintf(line, sizeof line, "PSP time zone: UTC%+d min, daylight saving %s", zone_minutes,
                 daylight_saving ? "on" : "off");
    } else {
        snprintf(line, sizeof line, "PSP time zone: unknown");
    }
    skiff_psp_report_line(&p->report, line);
}

int main(int argc, char *argv[]) {
    probe p;
    memset(&p, 0, sizeof p);
    p.program_path = argc > 0 ? argv[0] : NULL;
    skiff_psp_install_exit_callback();
    skiff_psp_report_open(&p.report, p.program_path);
    skiff_psp_report_line(&p.report, "Skiff network probe");
    report_clock(&p);

    const int has_ark = skiff_psp_entropy_status() != SKIFF_ERR_NET_NEEDS_ARK;
    const int passed = has_ark ? run_with_ark(&p) : run_without_ark(&p);
    const char *marker = has_ark ? (passed ? PROBE_OK_MARKER : PROBE_FAIL_MARKER)
                                 : (passed ? PROBE_NO_ARK_OK_MARKER : PROBE_NO_ARK_FAIL_MARKER);
    skiff_psp_report_line(&p.report, marker);
    skiff_psp_report_close(&p.report);
    sceKernelExitGame();
    return passed ? 0 : 1;
}
