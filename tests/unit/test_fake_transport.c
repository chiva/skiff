/*
 * The fake transport replays the RomM responses recorded by `scripts/dev.sh romm-record` and
 * injects the failures layers above net/ must survive. These tests pin its behaviour, so later
 * contract tests (romm/, jobs/) can rely on it acting like the real server.
 */
#include <string.h>
#include <zlib.h>

#include "skiff/transport.h"

#include "fake_transport.h"
#include "unity.h"

enum { BODY_MAX = 8192, RESUME_OFFSET = 1000 };

/* Recorded from the integration RomM with a 4096-byte synthetic file. */
#define CONTENT_PATH "/api/roms/1/content/Skiff%20Test%20Payload.iso"
#define CONTENT_ETAG "\"6acba45e-1000\""
#define CONTENT_BYTES 4096U
#define HEARTBEAT_BYTES 1570U

typedef struct body_sink {
    unsigned char bytes[BODY_MAX];
    size_t size;
    int chunks;
    skiff_err fail_with;
    /* The status the response held when the first chunk arrived. */
    long status_at_first_chunk;
} body_sink;

static fake_transport fake;
static body_sink sink;
static skiff_http_request request;
static skiff_http_response response;
static unsigned char full_content[CONTENT_BYTES];

static skiff_err collect(void *ctx, const unsigned char *data, size_t size) {
    body_sink *target = ctx;
    if (target->chunks == 0) {
        target->status_at_first_chunk = response.status;
    }
    if (target->fail_with != SKIFF_OK) {
        return target->fail_with;
    }
    TEST_ASSERT_LESS_OR_EQUAL_size_t(sizeof target->bytes - target->size, size);
    memcpy(target->bytes + target->size, data, size);
    target->size += size;
    target->chunks++;
    return SKIFF_OK;
}

void setUp(void) {
    fake_transport_init(&fake);
    memset(&sink, 0, sizeof sink);
    memset(&request, 0, sizeof request);
    request.on_body = collect;
    request.body_ctx = &sink;
}

void tearDown(void) { skiff_transport_destroy(&fake.base); }

static fake_route *add(const char *path, const char *fixture) {
    fake_route *route = fake_transport_add_fixture(&fake, path, fixture);
    TEST_ASSERT_NOT_NULL_MESSAGE(route, fixture);
    return route;
}

static skiff_err get(const char *url) {
    memset(&sink.bytes, 0, sizeof sink.bytes);
    sink.size = 0;
    sink.chunks = 0;
    request.url = url;
    const skiff_err err = skiff_transport_perform(&fake.base, &request, &response);
    TEST_PRINTF("GET %s -> %s, HTTP %ld, %llu body bytes in %d chunk(s), %ld new connection(s)",
                url, skiff_err_name(err), response.status, (unsigned long long)response.body_bytes,
                sink.chunks, response.new_connections);
    return err;
}

/* The whole recorded file, for comparing ranged replies against. */
static void load_full_content(void) {
    add(CONTENT_PATH, "romm/rom-content.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_size_t(CONTENT_BYTES, sink.size);
    memcpy(full_content, sink.bytes, CONTENT_BYTES);
}

static void test_replays_a_recorded_json_response(void) {
    add("/api/heartbeat", "romm/heartbeat.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test:8443/api/heartbeat"));
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_UINT64(HEARTBEAT_BYTES, response.body_bytes);
    TEST_ASSERT_FALSE_MESSAGE(response.has_content_length, "recorded with chunked encoding");
    TEST_ASSERT_NOT_NULL(strstr((const char *)sink.bytes, "\"VERSION\":\"5.4.0\""));
}

static void test_replays_a_download_in_chunks_with_its_etag(void) {
    load_full_content();
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_STRING(CONTENT_ETAG, response.etag);
    TEST_ASSERT_TRUE(response.has_content_length);
    TEST_ASSERT_EQUAL_UINT64(CONTENT_BYTES, response.content_length);
    TEST_ASSERT_EQUAL_INT(CONTENT_BYTES / FAKE_TRANSPORT_CHUNK_BYTES, sink.chunks);
}

static void test_resume_with_the_current_etag_returns_the_rest(void) {
    load_full_content();
    request.has_range = 1;
    request.range_start = RESUME_OFFSET;
    request.if_range = CONTENT_ETAG;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_INT64(206, response.status);
    TEST_ASSERT_TRUE(response.has_content_range);
    TEST_ASSERT_EQUAL_UINT64(RESUME_OFFSET, response.range_start);
    TEST_ASSERT_EQUAL_UINT64(CONTENT_BYTES - 1, response.range_end);
    TEST_ASSERT_EQUAL_UINT64(CONTENT_BYTES, response.range_total);
    TEST_ASSERT_EQUAL_UINT64(CONTENT_BYTES - RESUME_OFFSET, response.content_length);
    TEST_ASSERT_EQUAL_size_t(CONTENT_BYTES - RESUME_OFFSET, sink.size);
    TEST_ASSERT_EQUAL_MEMORY(full_content + RESUME_OFFSET, sink.bytes, sink.size);
}

static void test_resume_without_if_range_is_refused(void) {
    add(CONTENT_PATH, "romm/rom-content.http");
    request.has_range = 1;
    request.range_start = RESUME_OFFSET;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, fake.request_count, "the fake must not see it");
}

static void test_resume_with_a_stale_etag_restarts_with_the_whole_file(void) {
    load_full_content();
    request.has_range = 1;
    request.range_start = RESUME_OFFSET;
    request.if_range = "\"stale\"";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_FALSE(response.has_content_range);
    TEST_ASSERT_EQUAL_MEMORY(full_content, sink.bytes, CONTENT_BYTES);
}

static void test_file_changed_on_the_server(void) {
    load_full_content();
    fake.routes[0].current_etag = "\"changed-1000\"";
    request.has_range = 1;
    request.range_start = RESUME_OFFSET;
    request.if_range = CONTENT_ETAG;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_INT64_MESSAGE(200, response.status, "the recorded ETag no longer matches");
    TEST_ASSERT_EQUAL_STRING("\"changed-1000\"", response.etag);
    TEST_ASSERT_EQUAL_size_t(CONTENT_BYTES, sink.size);
}

static void test_resume_past_the_end_is_not_satisfiable(void) {
    add(CONTENT_PATH, "romm/rom-content.http");
    request.has_range = 1;
    request.range_start = CONTENT_BYTES;
    request.if_range = CONTENT_ETAG;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_INT64(416, response.status);
    TEST_ASSERT_EQUAL_UINT64(0, response.body_bytes);
}

static void test_timeout_mid_download_after_the_bytes_that_came(void) {
    fake_route *route = add(CONTENT_PATH, "romm/rom-content.http");
    route->fail_after_bytes = 1500;
    route->fail_mid_body = SKIFF_ERR_NET_TIMEOUT;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_TIMEOUT, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_UINT64(1500, response.body_bytes);
    TEST_ASSERT_EQUAL_size_t(1500, sink.size);
    route->fail_mid_body = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_INT64_MESSAGE(1, response.new_connections, "a failure drops the connection");
}

static void test_connection_lost_mid_download(void) {
    fake_route *route = add(CONTENT_PATH, "romm/rom-content.http");
    route->fail_after_bytes = 0;
    route->fail_mid_body = SKIFF_ERR_NET_CONNECTION_LOST;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECTION_LOST, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_UINT64(0, response.body_bytes);
}

static void test_failure_threshold_beyond_the_body_fails_after_all_of_it(void) {
    fake_route *route = add(CONTENT_PATH, "romm/rom-content.http");
    route->fail_after_bytes = (uint64_t)CONTENT_BYTES * 2;
    route->fail_mid_body = SKIFF_ERR_NET_CONNECTION_LOST;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECTION_LOST, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_UINT64(CONTENT_BYTES, response.body_bytes);
}

static void test_failure_before_any_response(void) {
    fake_route *route = add("/api/heartbeat", "romm/heartbeat.http");
    route->fail_before_response = SKIFF_ERR_NET_CONNECT;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECT, get("https://romm.test/api/heartbeat"));
    TEST_ASSERT_EQUAL_INT64(0, response.status);
    TEST_ASSERT_EQUAL_UINT64(0, response.body_bytes);
}

static void test_body_callback_error_is_returned(void) {
    add(CONTENT_PATH, "romm/rom-content.http");
    sink.fail_with = SKIFF_ERR_STORAGE_NO_SPACE;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_UINT64(0, response.body_bytes);
}

/* Stops with SKIFF_ERR_NET_UNAVAILABLE once asked more than allowed_polls times. */
typedef struct stop_hook {
    int polls;
    int allowed_polls;
} stop_hook;

static skiff_err stop_after_polls(void *ctx) {
    stop_hook *hook = ctx;
    hook->polls++;
    return hook->polls > hook->allowed_polls ? SKIFF_ERR_NET_UNAVAILABLE : SKIFF_OK;
}

static void test_stop_hook_before_the_response(void) {
    add(CONTENT_PATH, "romm/rom-content.http");
    stop_hook hook = {0, 0};
    request.should_stop = stop_after_polls;
    request.stop_ctx = &hook;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_UNAVAILABLE, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_INT64_MESSAGE(0, response.status, "stopped before any response");
    TEST_ASSERT_EQUAL_INT(1, hook.polls);
}

static void test_stop_hook_mid_body_keeps_what_came(void) {
    add(CONTENT_PATH, "romm/rom-content.http");
    /* One poll before the response, then one per chunk: two chunks get through. */
    stop_hook hook = {0, 3};
    request.should_stop = stop_after_polls;
    request.stop_ctx = &hook;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_UNAVAILABLE, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_UINT64(2 * FAKE_TRANSPORT_CHUNK_BYTES, response.body_bytes);
    TEST_ASSERT_EQUAL_size_t(2 * FAKE_TRANSPORT_CHUNK_BYTES, sink.size);
    request.should_stop = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_INT64_MESSAGE(1, response.new_connections, "a stop drops the connection");
}

static void test_server_ignoring_range_sends_the_whole_file(void) {
    load_full_content();
    fake.routes[0].ignore_range = 1;
    request.has_range = 1;
    request.range_start = RESUME_OFFSET;
    request.if_range = CONTENT_ETAG;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_INT64_MESSAGE(200, response.status, "the ETag matches, but no range support");
    TEST_ASSERT_FALSE(response.has_content_range);
    TEST_ASSERT_EQUAL_size_t(CONTENT_BYTES, sink.size);
    TEST_ASSERT_EQUAL_MEMORY(full_content, sink.bytes, CONTENT_BYTES);
}

static void test_body_is_counted_without_a_callback(void) {
    add(CONTENT_PATH, "romm/rom-content.http");
    request.on_body = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_UINT64(CONTENT_BYTES, response.body_bytes);
}

static void test_recorded_refusal_maps_to_a_romm_error(void) {
    add("/api/roms", "romm/roms-unauthorized.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test/api/roms"));
    TEST_ASSERT_EQUAL_INT64(401, response.status);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(401, sink.status_at_first_chunk,
                                    "a sink can refuse an error page before writing it");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_UNAUTHORIZED, skiff_http_status_error(response.status));
}

static void test_unknown_path_is_404(void) {
    add("/api/heartbeat", "romm/heartbeat.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test/api/nothing"));
    TEST_ASSERT_EQUAL_INT64(404, response.status);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test"));
    TEST_ASSERT_EQUAL_INT64(404, response.status);
}

static void test_logs_requests_and_keeps_the_connection(void) {
    static const skiff_http_header HEADERS[] = {{"Authorization", "Bearer rmm_test"},
                                                {"CF-Access-Client-Id", "skiff-test.access"}};
    add(CONTENT_PATH, "romm/rom-content.http");
    request.headers = HEADERS;
    request.header_count = 2;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_INT64(1, response.new_connections);
    request.has_range = 1;
    request.range_start = RESUME_OFFSET;
    request.if_range = CONTENT_ETAG;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test" CONTENT_PATH));
    TEST_ASSERT_EQUAL_INT64(0, response.new_connections);

    TEST_ASSERT_EQUAL_size_t(2, fake.log_count);
    TEST_ASSERT_EQUAL_STRING("https://romm.test" CONTENT_PATH, fake.log[0].url);
    TEST_ASSERT_EQUAL_STRING(
        "Authorization: Bearer rmm_test\nCF-Access-Client-Id: skiff-test.access\n",
        fake.log[0].headers);
    TEST_ASSERT_FALSE(fake.log[0].has_range);
    TEST_ASSERT_TRUE(fake.log[1].has_range);
    TEST_ASSERT_EQUAL_UINT64(RESUME_OFFSET, fake.log[1].range_start);
    TEST_ASSERT_EQUAL_STRING(CONTENT_ETAG, fake.log[1].if_range);
}

static void test_log_keeps_counting_when_full(void) {
    add("/api/heartbeat", "romm/heartbeat.http");
    request.on_body = NULL;
    for (int i = 0; i < FAKE_TRANSPORT_MAX_LOG + 2; i++) {
        request.url = "https://romm.test/api/heartbeat";
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(&fake.base, &request, &response));
    }
    TEST_ASSERT_EQUAL_size_t(FAKE_TRANSPORT_MAX_LOG, fake.log_count);
    TEST_ASSERT_EQUAL_size_t(FAKE_TRANSPORT_MAX_LOG + 2, fake.request_count);
}

static void test_raw_response_without_a_blank_line_has_no_body(void) {
    static const char RAW[] = "HTTP/1.1 204 No Content\r\nETag: \"x\"";
    TEST_ASSERT_NOT_NULL(fake_transport_add_raw(&fake, "/raw", RAW, sizeof RAW - 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("http://romm.test/raw"));
    TEST_ASSERT_EQUAL_INT64(204, response.status);
    TEST_ASSERT_EQUAL_UINT64(0, response.body_bytes);
}

static void test_routes_answer_by_method_and_in_turn(void) {
    static const char BODY[] = "{\"device_code\":\"skiff\"}";
    fake_route *pending = add("/api/auth/device/token", "romm/device-pending.http");
    pending->match_method = 1;
    pending->method = SKIFF_HTTP_POST;
    pending->max_uses = 2;
    fake_route *token = add("/api/auth/device/token", "romm/device-token.http");
    token->match_method = 1;
    token->method = SKIFF_HTTP_POST;
    request.method = SKIFF_HTTP_POST;
    request.body = BODY;
    request.body_size = sizeof BODY - 1;
    request.content_type = "application/json";
    TEST_PRINTF("two pending answers, then the token, for as long as the client polls");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test/api/auth/device/token"));
    TEST_ASSERT_EQUAL_INT64(400, response.status);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test/api/auth/device/token"));
    TEST_ASSERT_EQUAL_INT64(400, response.status);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test/api/auth/device/token"));
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test/api/auth/device/token"));
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_POST, fake.log[0].method);
    TEST_ASSERT_EQUAL_STRING(BODY, fake.log[0].body);
    TEST_ASSERT_EQUAL_size_t(sizeof BODY - 1, fake.log[0].body_size);
    TEST_ASSERT_EQUAL_STRING("application/json", fake.log[0].content_type);
    TEST_PRINTF("a GET does not match a POST-only route");
    memset(&request, 0, sizeof request);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get("https://romm.test/api/auth/device/token"));
    TEST_ASSERT_EQUAL_INT64(404, response.status);
}

/* A streamed body of size pattern bytes; fails with fail_with once fail_at bytes are given
 * (SKIFF_OK for never). */
typedef struct pattern_reader {
    size_t size;
    size_t given;
    size_t fail_at;
    skiff_err fail_with;
    int calls;
} pattern_reader;

#define UPLOAD_PATH "/api/saves"
#define UPLOAD_URL "https://romm.test" UPLOAD_PATH
#define UPLOAD_REPLY "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}"
/* Two and a half of the fake's reads. */
#define UPLOAD_BYTES                                                                               \
    ((size_t)2 * FAKE_TRANSPORT_UPLOAD_CHUNK_BYTES + FAKE_TRANSPORT_UPLOAD_CHUNK_BYTES / 2)

static unsigned char pattern_byte(size_t index) {
    enum { PATTERN_STEP = 7, PATTERN_OFFSET = 3, BYTE_MASK = 0xFF };
    return (unsigned char)((index * PATTERN_STEP + PATTERN_OFFSET) & BYTE_MASK);
}

static skiff_err read_pattern(void *ctx, unsigned char *buffer, size_t capacity, size_t *out_size) {
    pattern_reader *reader = ctx;
    reader->calls++;
    if (reader->fail_with != SKIFF_OK && reader->given >= reader->fail_at) {
        return reader->fail_with;
    }
    size_t count = reader->size - reader->given;
    count = count < capacity ? count : capacity;
    for (size_t i = 0; i < count; i++) {
        buffer[i] = pattern_byte(reader->given + i);
    }
    reader->given += count;
    *out_size = count;
    return SKIFF_OK;
}

static uint32_t pattern_crc32(size_t size) {
    uLong crc = crc32(0L, Z_NULL, 0);
    for (size_t i = 0; i < size; i++) {
        const unsigned char byte = pattern_byte(i);
        crc = crc32(crc, &byte, 1);
    }
    return (uint32_t)crc;
}

static pattern_reader upload_reader;

static skiff_err upload(size_t declared) {
    memset(&upload_reader, 0, sizeof upload_reader);
    upload_reader.size = UPLOAD_BYTES;
    request.method = SKIFF_HTTP_POST;
    request.read_body = read_pattern;
    request.read_ctx = &upload_reader;
    request.body_size = declared;
    request.content_type = "multipart/form-data; boundary=skiff";
    return get(UPLOAD_URL);
}

static void test_a_streamed_body_is_read_whole_and_recorded(void) {
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_raw(&fake, UPLOAD_PATH, UPLOAD_REPLY, sizeof UPLOAD_REPLY - 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, upload(UPLOAD_BYTES));
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    const fake_request *sent = &fake.log[0];
    TEST_PRINTF("received %zu of %zu bytes, CRC-32 %08x, in %d reader calls", sent->body_received,
                sent->body_size, (unsigned)sent->body_crc32, upload_reader.calls);
    TEST_ASSERT_TRUE(sent->body_streamed);
    TEST_ASSERT_EQUAL_size_t(UPLOAD_BYTES, sent->body_size);
    TEST_ASSERT_EQUAL_size_t(UPLOAD_BYTES, sent->body_received);
    TEST_ASSERT_EQUAL_HEX32(pattern_crc32(UPLOAD_BYTES), sent->body_crc32);
    for (size_t i = 0; i < FAKE_TRANSPORT_BODY_MAX - 1; i++) {
        TEST_ASSERT_EQUAL_HEX8(pattern_byte(i), (unsigned char)sent->body[i]);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sent->body[FAKE_TRANSPORT_BODY_MAX - 1], "kept bytes end");
    TEST_PRINTF("three reads of at most %d bytes, then the one that confirms the end",
                FAKE_TRANSPORT_UPLOAD_CHUNK_BYTES);
    TEST_ASSERT_EQUAL_INT(4, upload_reader.calls);
    TEST_ASSERT_EQUAL_STRING("multipart/form-data; boundary=skiff", sent->content_type);
}

static void test_a_body_sent_whole_is_hashed_too(void) {
    static const char BODY[] = "{\"operations_completed\":1}";
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_raw(&fake, UPLOAD_PATH, UPLOAD_REPLY, sizeof UPLOAD_REPLY - 1));
    request.method = SKIFF_HTTP_POST;
    request.body = BODY;
    request.body_size = sizeof BODY - 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, get(UPLOAD_URL));
    TEST_ASSERT_FALSE(fake.log[0].body_streamed);
    TEST_ASSERT_EQUAL_size_t(sizeof BODY - 1, fake.log[0].body_received);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)crc32(0L, (const Bytef *)BODY, sizeof BODY - 1),
                            fake.log[0].body_crc32);
}

static void test_an_upload_cut_part_way_gets_no_response(void) {
    fake_route *route =
        fake_transport_add_raw(&fake, UPLOAD_PATH, UPLOAD_REPLY, sizeof UPLOAD_REPLY - 1);
    TEST_ASSERT_NOT_NULL(route);
    route->fail_after_upload_bytes = FAKE_TRANSPORT_UPLOAD_CHUNK_BYTES + 1;
    route->fail_mid_upload = SKIFF_ERR_NET_CONNECTION_LOST;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECTION_LOST, upload(UPLOAD_BYTES));
    TEST_ASSERT_EQUAL_INT64(0, response.status);
    TEST_ASSERT_EQUAL_size_t(FAKE_TRANSPORT_UPLOAD_CHUNK_BYTES + 1, fake.log[0].body_received);
    TEST_PRINTF("a threshold past the body never fires");
    route->fail_after_upload_bytes = UPLOAD_BYTES + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, upload(UPLOAD_BYTES));
    TEST_ASSERT_EQUAL_INT64_MESSAGE(1, response.new_connections, "the cut dropped the connection");
}

static void test_stop_hook_during_an_upload(void) {
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_raw(&fake, UPLOAD_PATH, UPLOAD_REPLY, sizeof UPLOAD_REPLY - 1));
    /* Asked before each read: the third read is stopped. */
    stop_hook hook = {0, 2};
    request.should_stop = stop_after_polls;
    request.stop_ctx = &hook;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_UNAVAILABLE, upload(UPLOAD_BYTES));
    TEST_ASSERT_EQUAL_size_t(2 * FAKE_TRANSPORT_UPLOAD_CHUNK_BYTES, fake.log[0].body_received);
    TEST_ASSERT_EQUAL_INT64(0, response.status);
}

static void test_a_reader_that_fails_or_lies_stops_the_upload(void) {
    TEST_ASSERT_NOT_NULL(
        fake_transport_add_raw(&fake, UPLOAD_PATH, UPLOAD_REPLY, sizeof UPLOAD_REPLY - 1));
    memset(&upload_reader, 0, sizeof upload_reader);
    upload_reader.size = UPLOAD_BYTES;
    upload_reader.fail_at = FAKE_TRANSPORT_UPLOAD_CHUNK_BYTES;
    upload_reader.fail_with = SKIFF_ERR_STORAGE_IO;
    request.method = SKIFF_HTTP_POST;
    request.read_body = read_pattern;
    request.read_ctx = &upload_reader;
    request.body_size = UPLOAD_BYTES;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, get(UPLOAD_URL));
    TEST_ASSERT_EQUAL_size_t(FAKE_TRANSPORT_UPLOAD_CHUNK_BYTES, fake.log[0].body_received);
    TEST_PRINTF("a body shorter or longer than announced fails as curl's would");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, upload(UPLOAD_BYTES + 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, upload(UPLOAD_BYTES - 1));
    TEST_ASSERT_EQUAL_INT64(0, response.status);
}

static void test_an_unknown_path_still_reads_the_body(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, upload(UPLOAD_BYTES));
    TEST_ASSERT_EQUAL_INT64(404, response.status);
    TEST_ASSERT_EQUAL_size_t(UPLOAD_BYTES, fake.log[0].body_received);
}

static void test_route_errors(void) {
    TEST_ASSERT_NULL(fake_transport_add_fixture(&fake, "/x", "romm/missing.http"));
    for (int i = 0; i < FAKE_TRANSPORT_MAX_ROUTES; i++) {
        TEST_ASSERT_NOT_NULL(fake_transport_add_raw(&fake, "/x", "HTTP/1.1 200 OK\r\n\r\n", 19));
    }
    TEST_ASSERT_NULL_MESSAGE(fake_transport_add_raw(&fake, "/y", "HTTP/1.1 200 OK\r\n\r\n", 19),
                             "the route table is full");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_replays_a_recorded_json_response);
    RUN_TEST(test_replays_a_download_in_chunks_with_its_etag);
    RUN_TEST(test_resume_with_the_current_etag_returns_the_rest);
    RUN_TEST(test_resume_without_if_range_is_refused);
    RUN_TEST(test_resume_with_a_stale_etag_restarts_with_the_whole_file);
    RUN_TEST(test_file_changed_on_the_server);
    RUN_TEST(test_resume_past_the_end_is_not_satisfiable);
    RUN_TEST(test_timeout_mid_download_after_the_bytes_that_came);
    RUN_TEST(test_connection_lost_mid_download);
    RUN_TEST(test_failure_threshold_beyond_the_body_fails_after_all_of_it);
    RUN_TEST(test_failure_before_any_response);
    RUN_TEST(test_body_callback_error_is_returned);
    RUN_TEST(test_stop_hook_before_the_response);
    RUN_TEST(test_stop_hook_mid_body_keeps_what_came);
    RUN_TEST(test_server_ignoring_range_sends_the_whole_file);
    RUN_TEST(test_body_is_counted_without_a_callback);
    RUN_TEST(test_recorded_refusal_maps_to_a_romm_error);
    RUN_TEST(test_unknown_path_is_404);
    RUN_TEST(test_logs_requests_and_keeps_the_connection);
    RUN_TEST(test_log_keeps_counting_when_full);
    RUN_TEST(test_raw_response_without_a_blank_line_has_no_body);
    RUN_TEST(test_routes_answer_by_method_and_in_turn);
    RUN_TEST(test_a_streamed_body_is_read_whole_and_recorded);
    RUN_TEST(test_a_body_sent_whole_is_hashed_too);
    RUN_TEST(test_an_upload_cut_part_way_gets_no_response);
    RUN_TEST(test_stop_hook_during_an_upload);
    RUN_TEST(test_a_reader_that_fails_or_lies_stops_the_upload);
    RUN_TEST(test_an_unknown_path_still_reads_the_body);
    RUN_TEST(test_route_errors);
    return UNITY_END();
}
