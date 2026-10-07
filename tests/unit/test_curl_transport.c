/*
 * The curl transport against a scripted local server (tests/support/local_http_server.h): what goes
 * on the wire, how responses are read, keep-alive, and how each kind of failure is reported. No
 * RomM and no network needed; tests/integration/test_transport_romm.c covers TLS against a real
 * proxy.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "skiff/curl_transport.h"

#include "local_http_server.h"
#include "unity.h"

enum { URL_MAX = 96, BODY_MAX = 64 };

/* Short timeouts keep the timeout tests quick; curl checks the stall rule once a second. */
#define TEST_CONNECT_TIMEOUT_S 2L
#define TEST_STALL_TIMEOUT_S 1L
/* The stop-hook tests use a stall limit far above how soon the hook ends the transfer: the hook
 * stops once STOP_AFTER_MS have passed, which it can only see if curl keeps asking while nothing
 * arrives. */
#define TEST_LONG_STALL_TIMEOUT_S 30L
#define TEST_STOP_AFTER_MS 2000L
#define TEST_STOP_DEADLINE_MS 10000L
#define MS_PER_SECOND 1000L
#define NS_PER_MS 1000000L

#define STR_AND_SIZE(literal) literal, sizeof literal - 1

static local_http_server server;
static int server_running;
static skiff_transport *transport;
static skiff_http_request request;
static skiff_http_response response;
static char url[URL_MAX];

typedef struct body_sink {
    char bytes[BODY_MAX];
    size_t size;
    skiff_err fail_with;
    /* The status the response held when the first chunk arrived. */
    long status_at_first_chunk;
} body_sink;

static body_sink sink;

static skiff_err collect_body(void *ctx, const unsigned char *data, size_t size) {
    body_sink *target = ctx;
    if (target->size == 0 && target->status_at_first_chunk == 0) {
        target->status_at_first_chunk = response.status;
    }
    if (target->fail_with != SKIFF_OK) {
        TEST_PRINTF("body callback refuses %zu bytes with %s", size,
                    skiff_err_name(target->fail_with));
        return target->fail_with;
    }
    const size_t room = sizeof target->bytes - 1 - target->size;
    const size_t kept = size < room ? size : room;
    memcpy(target->bytes + target->size, data, kept);
    target->size += kept;
    target->bytes[target->size] = '\0';
    return SKIFF_OK;
}

static long milliseconds_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)now.tv_sec * MS_PER_SECOND + now.tv_nsec / NS_PER_MS;
}

/* Counts the polls and stops with stop_with once stop_after_ms have passed since started_ms (a
 * negative stop_after_ms never stops). */
typedef struct stop_hook {
    int polls;
    long started_ms;
    long stop_after_ms;
    skiff_err stop_with;
} stop_hook;

static stop_hook hook;

static skiff_err poll_stop_hook(void *ctx) {
    stop_hook *state = ctx;
    state->polls++;
    const long waited_ms = milliseconds_now() - state->started_ms;
    if (state->stop_after_ms >= 0 && waited_ms >= state->stop_after_ms) {
        TEST_PRINTF("stop hook stops the transfer on poll %d, %ld ms in, with %s", state->polls,
                    waited_ms, skiff_err_name(state->stop_with));
        return state->stop_with;
    }
    return SKIFF_OK;
}

static void use_stop_hook(long stop_after_ms, skiff_err stop_with) {
    hook.polls = 0;
    hook.started_ms = milliseconds_now();
    hook.stop_after_ms = stop_after_ms;
    hook.stop_with = stop_with;
    request.should_stop = poll_stop_hook;
    request.stop_ctx = &hook;
}

static const skiff_curl_config TEST_CONFIG = {
    .connect_timeout_s = TEST_CONNECT_TIMEOUT_S,
    .stall_timeout_s = TEST_STALL_TIMEOUT_S,
};

void setUp(void) {
    server_running = 0;
    transport = NULL;
    memset(&sink, 0, sizeof sink);
    memset(&hook, 0, sizeof hook);
    memset(&request, 0, sizeof request);
    request.url = url;
    request.on_body = collect_body;
    request.body_ctx = &sink;
}

void tearDown(void) {
    skiff_transport_destroy(transport);
    if (server_running) {
        local_http_server_stop(&server);
    }
}

static void serve(const local_http_reply *replies, size_t count, const char *scheme) {
    TEST_ASSERT_EQUAL_INT(0, local_http_server_start(&server, replies, count));
    server_running = 1;
    snprintf(url, sizeof url, "%s://127.0.0.1:%u/api/heartbeat", scheme, (unsigned)server.port);
}

static void create(const skiff_curl_config *config) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_curl_transport_create(config, &transport));
    TEST_ASSERT_NOT_NULL(transport);
}

static skiff_err perform(void) {
    const skiff_err err = skiff_transport_perform(transport, &request, &response);
    TEST_PRINTF("GET %s -> %s, HTTP %ld, %llu body bytes, %ld new connection(s)", url,
                skiff_err_name(err), response.status, (unsigned long long)response.body_bytes,
                response.new_connections);
    return err;
}

static void stop_server(void) {
    local_http_server_stop(&server);
    server_running = 0;
}

static void assert_request_has(size_t index, const char *line) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(server.requests[index], line), line);
}

static void test_headers_on_the_wire_and_partial_response(void) {
    static const local_http_reply REPLIES[] = {
        {STR_AND_SIZE("HTTP/1.1 206 Partial Content\r\nETag: \"e1\"\r\nContent-Length: 5\r\n"
                      "Content-Range: bytes 1000-1004/1005\r\n\r\nhello"),
         LOCAL_HTTP_CLOSE, 0},
    };
    static const skiff_http_header DEFAULTS[] = {{"CF-Access-Client-Id", "skiff-test.access"}};
    static const skiff_http_header HEADERS[] = {{"Authorization", "Bearer rmm_test"}};
    skiff_curl_config config = TEST_CONFIG;
    config.default_headers = DEFAULTS;
    config.default_header_count = 1;
    serve(REPLIES, 1, "http");
    create(&config);
    request.headers = HEADERS;
    request.header_count = 1;
    request.has_range = 1;
    request.range_start = 1000;
    request.if_range = "\"e1\"";

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, perform());
    TEST_ASSERT_EQUAL_INT64(206, response.status);
    TEST_ASSERT_EQUAL_STRING("\"e1\"", response.etag);
    TEST_ASSERT_TRUE(response.has_content_range);
    TEST_ASSERT_EQUAL_UINT64(1000, response.range_start);
    TEST_ASSERT_EQUAL_UINT64(1005, response.range_total);
    TEST_ASSERT_EQUAL_STRING("hello", sink.bytes);
    TEST_ASSERT_EQUAL_UINT64(5, response.body_bytes);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", response.tls_version, "plain HTTP has no TLS session");
    TEST_ASSERT_EQUAL_STRING("", response.tls_cipher);

    stop_server();
    TEST_PRINTF("request on the wire:\n%s", server.requests[0]);
    assert_request_has(0, "GET /api/heartbeat HTTP/1.1\r\n");
    assert_request_has(0, "CF-Access-Client-Id: skiff-test.access\r\n");
    assert_request_has(0, "Authorization: Bearer rmm_test\r\n");
    assert_request_has(0, "Range: bytes=1000-\r\n");
    assert_request_has(0, "If-Range: \"e1\"\r\n");
}

static void test_range_offset_above_4_gib(void) {
    static const local_http_reply REPLIES[] = {
        {STR_AND_SIZE("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\n\r\n"),
         LOCAL_HTTP_CLOSE, 0},
    };
    serve(REPLIES, 1, "http");
    create(&TEST_CONFIG);
    request.has_range = 1;
    request.range_start = 4294967296ULL;
    request.if_range = "\"e1\"";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, perform());
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, skiff_http_status_error(response.status));
    stop_server();
    assert_request_has(0, "Range: bytes=4294967296-\r\n");
}

static void test_keep_alive_reuses_the_connection(void) {
    static const local_http_reply REPLIES[] = {
        {STR_AND_SIZE("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"), LOCAL_HTTP_KEEP_OPEN, 0},
        {STR_AND_SIZE("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"), LOCAL_HTTP_CLOSE, 0},
    };
    serve(REPLIES, 2, "http");
    create(&TEST_CONFIG);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, perform());
    TEST_ASSERT_EQUAL_INT64(1, response.new_connections);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, perform());
    TEST_ASSERT_EQUAL_INT64_MESSAGE(0, response.new_connections, "second request reuses it");
    stop_server();
    TEST_ASSERT_EQUAL_INT(1, server.connections);
}

static void test_a_post_sends_its_body_then_a_get_on_the_same_connection_sends_none(void) {
    static const local_http_reply REPLIES[] = {
        {STR_AND_SIZE("HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\n{}"), LOCAL_HTTP_KEEP_OPEN,
         0},
        {STR_AND_SIZE("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"), LOCAL_HTTP_CLOSE, 0},
    };
    static const char BODY[] = "{\"device_code\":\"abc\"}";
    serve(REPLIES, 2, "http");
    create(&TEST_CONFIG);
    request.method = SKIFF_HTTP_POST;
    request.body = BODY;
    request.body_size = sizeof BODY - 1;
    request.content_type = "application/json";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, perform());
    TEST_ASSERT_EQUAL_INT64(201, response.status);
    TEST_PRINTF("the handle is reused: the next GET must not resend the body");
    memset(&request, 0, sizeof request);
    request.url = url;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, perform());
    TEST_ASSERT_EQUAL_INT64_MESSAGE(0, response.new_connections, "the GET reuses the connection");
    stop_server();
    TEST_PRINTF("POST request:\n%s", server.requests[0]);
    TEST_ASSERT_EQUAL_INT(0, strncmp(server.requests[0], "POST /api/heartbeat ", 20));
    assert_request_has(0, "Content-Type: application/json\r\n");
    assert_request_has(0, "Content-Length: 21\r\n");
    assert_request_has(0, "\r\n\r\n{\"device_code\":\"abc\"}");
    TEST_ASSERT_NULL_MESSAGE(strstr(server.requests[0], "Expect:"), "no 100-continue round trip");
    TEST_PRINTF("GET request:\n%s", server.requests[1]);
    TEST_ASSERT_EQUAL_INT(0, strncmp(server.requests[1], "GET /api/heartbeat ", 19));
    TEST_ASSERT_NULL(strstr(server.requests[1], "Content-Length"));
    TEST_ASSERT_NULL(strstr(server.requests[1], "device_code"));
}

static void test_http_error_status_is_a_response_not_a_failure(void) {
    static const local_http_reply REPLIES[] = {
        {STR_AND_SIZE("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found"),
         LOCAL_HTTP_CLOSE, 0},
    };
    serve(REPLIES, 1, "http");
    create(&TEST_CONFIG);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, perform());
    TEST_ASSERT_EQUAL_INT64(404, response.status);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(404, sink.status_at_first_chunk,
                                    "a sink can refuse an error page before writing it");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_NOT_FOUND, skiff_http_status_error(response.status));
}

static void test_connection_refused(void) {
    const unsigned short port = local_http_unused_port();
    TEST_ASSERT_NOT_EQUAL(0, port);
    snprintf(url, sizeof url, "http://127.0.0.1:%u/", (unsigned)port);
    create(&TEST_CONFIG);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECT, perform());
}

static void test_stall_before_the_response_times_out(void) {
    static const local_http_reply REPLIES[] = {{"", 0, LOCAL_HTTP_STALL, 0}};
    serve(REPLIES, 1, "http");
    create(&TEST_CONFIG);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_TIMEOUT, perform());
}

static void test_stall_mid_body_times_out_after_delivering_what_came(void) {
    static const local_http_reply REPLIES[] = {
        {STR_AND_SIZE("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc"), LOCAL_HTTP_STALL, 0},
    };
    serve(REPLIES, 1, "http");
    create(&TEST_CONFIG);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_TIMEOUT, perform());
    TEST_ASSERT_EQUAL_STRING("abc", sink.bytes);
    TEST_ASSERT_EQUAL_UINT64(3, response.body_bytes);
}

static void test_cut_off_body_is_a_lost_connection(void) {
    static const local_http_reply REPLIES[] = {
        {STR_AND_SIZE("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc"), LOCAL_HTTP_CLOSE, 0},
    };
    serve(REPLIES, 1, "http");
    create(&TEST_CONFIG);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECTION_LOST, perform());
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_UINT64(3, response.body_bytes);
}

static void test_body_callback_error_stops_the_transfer_and_the_transport_survives(void) {
    static const local_http_reply REPLIES[] = {
        {STR_AND_SIZE("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"), LOCAL_HTTP_CLOSE, 0},
        {STR_AND_SIZE("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nagain"), LOCAL_HTTP_CLOSE, 0},
    };
    serve(REPLIES, 2, "http");
    create(&TEST_CONFIG);
    sink.fail_with = SKIFF_ERR_STORAGE_NO_SPACE;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, perform());
    TEST_ASSERT_EQUAL_UINT64(0, response.body_bytes);
    sink.fail_with = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, perform());
    TEST_ASSERT_EQUAL_STRING("again", sink.bytes);
}

static void test_stop_hook_ends_a_stalled_transfer_before_the_stall_timeout(void) {
    static const local_http_reply REPLIES[] = {
        {STR_AND_SIZE("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc"), LOCAL_HTTP_STALL, 0},
    };
    skiff_curl_config config = TEST_CONFIG;
    config.stall_timeout_s = TEST_LONG_STALL_TIMEOUT_S;
    serve(REPLIES, 1, "http");
    create(&config);
    use_stop_hook(TEST_STOP_AFTER_MS, SKIFF_ERR_NET_UNAVAILABLE);

    const skiff_err err = perform();
    const long elapsed_ms = milliseconds_now() - hook.started_ms;
    TEST_PRINTF("stopped after %ld ms and %d polls (stall limit %ld s)", elapsed_ms, hook.polls,
                TEST_LONG_STALL_TIMEOUT_S);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_NET_UNAVAILABLE, err, "the hook's error, unchanged");
    TEST_ASSERT_TRUE_MESSAGE(elapsed_ms < TEST_STOP_DEADLINE_MS, "the hook is polled while idle");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("abc", sink.bytes, "what arrived was still delivered");
}

static void test_stop_hook_on_the_first_poll_and_the_transport_survives(void) {
    /* Whether curl's first poll comes before or after the request reaches the server, the second
     * request gets a "hello". */
    static const local_http_reply REPLIES[] = {
        {STR_AND_SIZE("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"), LOCAL_HTTP_CLOSE, 0},
        {STR_AND_SIZE("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"), LOCAL_HTTP_CLOSE, 0},
    };
    serve(REPLIES, 2, "http");
    create(&TEST_CONFIG);
    use_stop_hook(0, SKIFF_ERR_NET_CONNECTION_LOST);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECTION_LOST, perform());
    TEST_ASSERT_EQUAL_INT(1, hook.polls);
    TEST_ASSERT_EQUAL_UINT64(0, response.body_bytes);

    use_stop_hook(-1, SKIFF_OK);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, perform());
    TEST_ASSERT_EQUAL_STRING("hello", sink.bytes);
    TEST_ASSERT_TRUE_MESSAGE(hook.polls > 0, "a hook that never stops is still asked");
}

static void test_body_is_discarded_without_a_callback(void) {
    static const local_http_reply REPLIES[] = {
        {STR_AND_SIZE("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"), LOCAL_HTTP_CLOSE, 0},
    };
    serve(REPLIES, 1, "http");
    create(&TEST_CONFIG);
    request.on_body = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, perform());
    TEST_ASSERT_EQUAL_UINT64(5, response.body_bytes);
}

/* A TLS client that gets plain HTTP back cannot complete the handshake. */
static const local_http_reply NOT_TLS[] = {
    {STR_AND_SIZE("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"), LOCAL_HTTP_CLOSE, 1},
};

static void test_handshake_failure(void) {
    serve(NOT_TLS, 1, "https");
    create(&TEST_CONFIG);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_TLS_HANDSHAKE, perform());
}

static void test_unreadable_client_certificate(void) {
    skiff_curl_config config = TEST_CONFIG;
    config.client_cert = "/nonexistent/client.crt";
    config.client_key = "/nonexistent/client.key";
    serve(NOT_TLS, 1, "https");
    create(&config);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_TLS_CLIENT_CERT, perform());
}

static void test_unreadable_ca_file(void) {
    skiff_curl_config config = TEST_CONFIG;
    config.ca_file = "/nonexistent/ca.crt";
    serve(NOT_TLS, 1, "https");
    create(&config);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_INVALID_VALUE, perform());
}

static void test_bad_addresses_are_configuration_errors(void) {
    create(&TEST_CONFIG);
    snprintf(url, sizeof url, "ftp://127.0.0.1/");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_INVALID_VALUE, perform());
    snprintf(url, sizeof url, "127.0.0.1/api/heartbeat");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_INVALID_VALUE, perform());
    snprintf(url, sizeof url, "http://[::1/");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_INVALID_VALUE, perform());
}

static void test_create_validates_its_configuration(void) {
    static const skiff_http_header INJECTED[] = {{"X-Token", "a\r\nX-Evil: 1"}};
    skiff_curl_config config = TEST_CONFIG;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_curl_transport_create(&config, NULL));
    config.client_key = "client.key";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_curl_transport_create(&config, &transport));
    TEST_ASSERT_NULL(transport);
    config = TEST_CONFIG;
    config.default_headers = INJECTED;
    config.default_header_count = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_curl_transport_create(&config, &transport));
    config = TEST_CONFIG;
    config.stall_timeout_s = -1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_curl_transport_create(&config, &transport));
}

static void test_null_config_uses_the_defaults(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_curl_transport_create(NULL, &transport));
    TEST_ASSERT_NOT_NULL(transport);
}

int main(void) {
    if (skiff_net_global_init() != SKIFF_OK) {
        printf("skiff_net_global_init() failed: TLS could not be seeded\n");
        return 1;
    }
    UNITY_BEGIN();
    RUN_TEST(test_headers_on_the_wire_and_partial_response);
    RUN_TEST(test_range_offset_above_4_gib);
    RUN_TEST(test_keep_alive_reuses_the_connection);
    RUN_TEST(test_a_post_sends_its_body_then_a_get_on_the_same_connection_sends_none);
    RUN_TEST(test_http_error_status_is_a_response_not_a_failure);
    RUN_TEST(test_connection_refused);
    RUN_TEST(test_stall_before_the_response_times_out);
    RUN_TEST(test_stall_mid_body_times_out_after_delivering_what_came);
    RUN_TEST(test_cut_off_body_is_a_lost_connection);
    RUN_TEST(test_body_callback_error_stops_the_transfer_and_the_transport_survives);
    RUN_TEST(test_stop_hook_ends_a_stalled_transfer_before_the_stall_timeout);
    RUN_TEST(test_stop_hook_on_the_first_poll_and_the_transport_survives);
    RUN_TEST(test_body_is_discarded_without_a_callback);
    RUN_TEST(test_handshake_failure);
    RUN_TEST(test_unreadable_client_certificate);
    RUN_TEST(test_unreadable_ca_file);
    RUN_TEST(test_bad_addresses_are_configuration_errors);
    RUN_TEST(test_create_validates_its_configuration);
    RUN_TEST(test_null_config_uses_the_defaults);
    const int failures = UNITY_END();
    skiff_net_global_cleanup();
    return failures;
}
