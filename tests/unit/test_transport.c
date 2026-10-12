/* The checks skiff_transport_perform() makes before any transport sees a request, and the rules
 * skiff_http_read_body() holds a streamed body's reader to for every transport. */
#include <string.h>

#include "skiff/transport.h"

#include "unity.h"

typedef struct recording_transport {
    skiff_transport base;
    int performs;
    int destroys;
    const skiff_http_request *last_request;
} recording_transport;

static skiff_err record_perform(skiff_transport *base, const skiff_http_request *request,
                                skiff_http_response *response) {
    recording_transport *recording = (recording_transport *)base;
    recording->performs++;
    recording->last_request = request;
    response->status = 204;
    return SKIFF_OK;
}

static void record_destroy(skiff_transport *base) { ((recording_transport *)base)->destroys++; }

static const skiff_transport_ops RECORDING_OPS = {record_perform, record_destroy};

static recording_transport transport;
static skiff_http_request request;
static skiff_http_response response;

void setUp(void) {
    memset(&transport, 0, sizeof transport);
    transport.base.ops = &RECORDING_OPS;
    memset(&request, 0, sizeof request);
    request.url = "https://romm.example/api/heartbeat";
    memset(&response, 0xAB, sizeof response);
}

void tearDown(void) {}

static void expect_refused(const char *why) {
    TEST_PRINTF("refused: %s", why);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        SKIFF_ERR_INVALID_ARG, skiff_transport_perform(&transport.base, &request, &response), why);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, transport.performs, "the transport must not see it");
    TEST_ASSERT_EQUAL_INT64_MESSAGE(0, response.status, "the response is reset first");
}

static void test_valid_request_reaches_the_transport_with_a_reset_response(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(&transport.base, &request, &response));
    TEST_ASSERT_EQUAL_INT(1, transport.performs);
    TEST_ASSERT_EQUAL_INT64(204, response.status);
    TEST_ASSERT_EQUAL_UINT64(0, response.body_bytes);
}

static skiff_err never_stop(void *ctx) {
    (void)ctx;
    return SKIFF_OK;
}

static void test_stop_hook_reaches_the_transport(void) {
    int stop_ctx = 0;
    request.should_stop = never_stop;
    request.stop_ctx = &stop_ctx;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(&transport.base, &request, &response));
    TEST_ASSERT_EQUAL_PTR(never_stop, transport.last_request->should_stop);
    TEST_ASSERT_EQUAL_PTR(&stop_ctx, transport.last_request->stop_ctx);
}

static void test_null_and_missing_arguments(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_transport_perform(&transport.base, &request, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_transport_perform(NULL, &request, &response));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_transport_perform(&transport.base, NULL, &response));
    request.url = NULL;
    expect_refused("no URL");
    request.url = "";
    expect_refused("empty URL");
}

static void expect_bad_address(const char *url) {
    request.url = url;
    TEST_PRINTF("bad address: %s", url);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_CONFIG_INVALID_VALUE,
                                  skiff_transport_perform(&transport.base, &request, &response),
                                  url);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, transport.performs, "nothing may be sent");
}

static void test_only_explicit_http_and_https_are_sent(void) {
    expect_bad_address("romm.example/api/heartbeat");
    expect_bad_address("romm.example:8443/api/heartbeat");
    expect_bad_address("ftp://romm.example/");
    expect_bad_address("http:/romm.example/");
    expect_bad_address("https:romm.example/");
    static const char *const ACCEPTED[] = {"http://romm.example/", "https://romm.example/",
                                           "HTTPS://romm.example/", "Http://romm.example/"};
    for (size_t i = 0; i < sizeof ACCEPTED / sizeof ACCEPTED[0]; i++) {
        request.url = ACCEPTED[i];
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            SKIFF_OK, skiff_transport_perform(&transport.base, &request, &response), ACCEPTED[i]);
    }
    TEST_ASSERT_EQUAL_INT(4, transport.performs);
}

static void test_transport_without_ops_is_refused(void) {
    skiff_transport bare = {NULL};
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_transport_perform(&bare, &request, &response));
}

static void test_header_injection_is_refused(void) {
    const skiff_http_header split_value[] = {{"CF-Access-Client-Id", "id\r\nX-Evil: 1"}};
    const skiff_http_header split_name[] = {{"X-A\nX-B", "v"}};
    const skiff_http_header colon_name[] = {{"X-A: b", "v"}};
    const skiff_http_header empty_name[] = {{"", "v"}};
    const skiff_http_header null_value[] = {{"X-A", NULL}};
    const skiff_http_header raw_range[] = {{"range", "bytes=1000-"}};
    const skiff_http_header raw_if_range[] = {{"If-Range", "\"etag\""}};
    request.header_count = 1;
    request.headers = split_value;
    expect_refused("CRLF in a value");
    request.headers = split_name;
    expect_refused("LF in a name");
    request.headers = colon_name;
    expect_refused("colon in a name");
    request.headers = empty_name;
    expect_refused("empty name");
    request.headers = null_value;
    expect_refused("NULL value");
    request.headers = raw_range;
    expect_refused("Range as a plain header would skip the ETag check");
    request.headers = raw_if_range;
    expect_refused("If-Range as a plain header");
    request.headers = NULL;
    expect_refused("count without headers");
}

static void test_valid_headers_pass(void) {
    const skiff_http_header headers[] = {{"Authorization", "Bearer rmm_test"},
                                         {"CF-Access-Client-Secret", ""}};
    TEST_ASSERT_TRUE(skiff_http_headers_valid(headers, 2));
    TEST_ASSERT_TRUE(skiff_http_headers_valid(NULL, 0));
}

static void test_if_range_needs_a_range_and_no_line_break(void) {
    request.if_range = "\"etag\"";
    expect_refused("If-Range without Range");
    request.if_range = NULL;
    request.has_range = 1;
    expect_refused("Range without If-Range: a changed file would be spliced");
    request.if_range = "";
    expect_refused("Range with an empty If-Range (the parser's absent ETag)");
    request.if_range = " \t";
    expect_refused("Range with a blank If-Range");
    request.if_range = NULL;
    request.has_range = 2;
    expect_refused("any non-zero has_range counts as a range");
    request.has_range = 1;
    request.if_range = "\"etag\"\r\nX-Evil: 1";
    expect_refused("CRLF in If-Range");
    request.if_range = "\"etag\"";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(&transport.base, &request, &response));
}

static void test_a_post_carries_its_body_and_content_type(void) {
    static const char BODY[] = "{\"device_code\":\"x\"}";
    request.method = SKIFF_HTTP_POST;
    request.body = BODY;
    request.body_size = sizeof BODY - 1;
    request.content_type = "application/json";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(&transport.base, &request, &response));
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_POST, transport.last_request->method);
    TEST_ASSERT_EQUAL_PTR(BODY, transport.last_request->body);
    TEST_ASSERT_EQUAL_size_t(sizeof BODY - 1, transport.last_request->body_size);
    TEST_PRINTF("an empty POST, without a body or a content type, is allowed too");
    request.body = NULL;
    request.body_size = 0;
    request.content_type = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(&transport.base, &request, &response));
}

static void test_bodies_belong_to_posts_and_ranges_to_gets(void) {
    request.body = "{}";
    request.body_size = 2;
    expect_refused("a GET with a body");
    request.body = NULL;
    request.body_size = 0;
    request.content_type = "application/json";
    expect_refused("a GET with a content type");
    request.method = SKIFF_HTTP_POST;
    request.content_type = "application/json\r\nX-Evil: 1";
    expect_refused("CRLF in the content type");
    request.content_type = "";
    expect_refused("an empty content type");
    request.content_type = "application/json";
    request.body_size = 4;
    expect_refused("a body size without a body");
    request.body_size = 0;
    request.has_range = 1;
    request.if_range = "\"etag\"";
    expect_refused("a POST with a range");
    request.method = SKIFF_HTTP_PUT;
    expect_refused("a PUT with a range");
    request.has_range = 0;
    request.if_range = NULL;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    request.method = (skiff_http_method)7;
    expect_refused("an unknown method");
}

static void test_a_put_carries_its_body_as_a_post_does(void) {
    static const char BODY[] = "{\"capabilities\":{\"remote_install\":true}}";
    request.method = SKIFF_HTTP_PUT;
    request.body = BODY;
    request.body_size = sizeof BODY - 1;
    request.content_type = "application/json";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(&transport.base, &request, &response));
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_PUT, transport.last_request->method);
    TEST_ASSERT_EQUAL_PTR(BODY, transport.last_request->body);
    TEST_ASSERT_EQUAL_size_t(sizeof BODY - 1, transport.last_request->body_size);
}

/* A body reader over data: gives at most per_call bytes a call (0 for whatever is asked), fails
 * with fail_with once fail_at bytes are given (SKIFF_OK for never), and with overflow gives one
 * byte more than asked. Counts its calls and records the largest capacity it was offered. */
typedef struct scripted_reader {
    const unsigned char *data;
    size_t size;
    size_t given;
    size_t per_call;
    size_t fail_at;
    skiff_err fail_with;
    int overflow;
    int calls;
    size_t largest_capacity;
} scripted_reader;

static skiff_err read_script(void *ctx, unsigned char *buffer, size_t capacity, size_t *out_size) {
    scripted_reader *script = ctx;
    script->calls++;
    script->largest_capacity =
        capacity > script->largest_capacity ? capacity : script->largest_capacity;
    if (script->fail_with != SKIFF_OK && script->given >= script->fail_at) {
        TEST_PRINTF("reader fails at byte %zu with %s", script->given,
                    skiff_err_name(script->fail_with));
        return script->fail_with;
    }
    size_t count = script->size - script->given;
    count = count < capacity ? count : capacity;
    if (script->per_call > 0 && count > script->per_call) {
        count = script->per_call;
    }
    memcpy(buffer, script->data + script->given, count);
    script->given += count;
    *out_size = script->overflow ? capacity + 1 : count;
    return SKIFF_OK;
}

static const unsigned char STREAM[] =
    "--skiff\r\nContent-Disposition: form-data; name=\"saveFile\"; "
    "filename=\"SKIF00001.zip\"\r\n\r\nPK\x03\x04 body\r\n--skiff--\r\n";
#define STREAM_SIZE (sizeof STREAM - 1)
/* An odd capacity, so the body ends part way into a call. */
#define READ_CAPACITY ((size_t)7)

static scripted_reader reader;

static void stream_request(size_t declared_size) {
    memset(&reader, 0, sizeof reader);
    reader.data = STREAM;
    reader.size = STREAM_SIZE;
    request.method = SKIFF_HTTP_POST;
    request.read_body = read_script;
    request.read_ctx = &reader;
    request.body_size = declared_size;
}

/* Reads the whole body the way a transport does; returns the first error, and what was read. */
static skiff_err drain(unsigned char *out, size_t out_size, size_t *total) {
    size_t sent = 0;
    *total = 0;
    for (;;) {
        unsigned char chunk[READ_CAPACITY];
        size_t got = 0;
        const skiff_err err = skiff_http_read_body(&request, &sent, chunk, sizeof chunk, &got);
        if (err != SKIFF_OK) {
            TEST_PRINTF("read stops after %zu bytes with %s", *total, skiff_err_name(err));
            TEST_ASSERT_EQUAL_size_t_MESSAGE(0, got, "nothing is handed out with an error");
            return err;
        }
        if (got == 0) {
            TEST_PRINTF("read %zu bytes of %zu in %d reader calls", *total, request.body_size,
                        reader.calls);
            return SKIFF_OK;
        }
        TEST_ASSERT_LESS_OR_EQUAL_size_t(out_size - *total, got);
        memcpy(out + *total, chunk, got);
        *total += got;
        TEST_ASSERT_EQUAL_size_t(*total, sent);
    }
}

static void test_a_post_streams_its_body_from_a_reader(void) {
    stream_request(STREAM_SIZE);
    request.content_type = "multipart/form-data; boundary=skiff";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_transport_perform(&transport.base, &request, &response));
    TEST_ASSERT_EQUAL_PTR(read_script, transport.last_request->read_body);
    TEST_ASSERT_EQUAL_PTR(&reader, transport.last_request->read_ctx);
    TEST_ASSERT_EQUAL_size_t(STREAM_SIZE, transport.last_request->body_size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, reader.calls, "the transport reads it, not the check");
}

static void test_a_reader_needs_a_post_a_size_and_no_body(void) {
    stream_request(STREAM_SIZE);
    request.method = SKIFF_HTTP_GET;
    expect_refused("a GET with a body reader");
    request.method = SKIFF_HTTP_POST;
    request.body = "{}";
    expect_refused("both a body and a body reader");
    request.body = NULL;
    request.body_size = 0;
    expect_refused("a body reader with no size: curl would never ask it for the end");
    TEST_ASSERT_EQUAL_INT(0, reader.calls);
}

static void test_reading_hands_out_exactly_body_size_bytes(void) {
    stream_request(STREAM_SIZE);
    unsigned char out[sizeof STREAM];
    size_t total = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, drain(out, sizeof out, &total));
    TEST_ASSERT_EQUAL_size_t(STREAM_SIZE, total);
    TEST_ASSERT_EQUAL_MEMORY(STREAM, out, STREAM_SIZE);
    TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(READ_CAPACITY, reader.largest_capacity,
                                             "never asked for more than the buffer holds");
    TEST_PRINTF("one call per chunk, plus the one that confirms the end");
    TEST_ASSERT_EQUAL_INT((int)((STREAM_SIZE + READ_CAPACITY - 1) / READ_CAPACITY) + 1,
                          reader.calls);
}

static void test_a_reader_giving_less_per_call_is_asked_again(void) {
    stream_request(STREAM_SIZE);
    reader.per_call = 3;
    unsigned char out[sizeof STREAM];
    size_t total = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, drain(out, sizeof out, &total));
    TEST_ASSERT_EQUAL_MEMORY(STREAM, out, STREAM_SIZE);
}

static void test_a_reader_is_never_asked_past_the_size(void) {
    TEST_PRINTF("the body is declared one byte short: the last byte is never asked for, but the "
                "reader still has it, so the body grew");
    const size_t declared = STREAM_SIZE - 1;
    const size_t last_chunk =
        declared % READ_CAPACITY == 0 ? READ_CAPACITY : declared % READ_CAPACITY;
    stream_request(declared);
    unsigned char out[sizeof STREAM];
    size_t total = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, drain(out, sizeof out, &total));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(declared - last_chunk, total,
                                     "the chunk that ends the declared size is withheld");
    TEST_ASSERT_EQUAL_size_t_MESSAGE(STREAM_SIZE, reader.given, "the check read the extra byte");
}

static void test_a_body_shorter_than_its_size_fails(void) {
    stream_request(STREAM_SIZE + 10);
    unsigned char out[sizeof STREAM];
    size_t total = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, drain(out, sizeof out, &total));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(STREAM_SIZE, total, "every byte the reader had went out");
}

static void test_a_reader_error_is_returned_unchanged(void) {
    stream_request(STREAM_SIZE);
    reader.fail_at = 2 * READ_CAPACITY;
    reader.fail_with = SKIFF_ERR_STORAGE_IO;
    unsigned char out[sizeof STREAM];
    size_t total = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, drain(out, sizeof out, &total));
    TEST_ASSERT_EQUAL_size_t(2 * READ_CAPACITY, total);
    TEST_PRINTF("an error from the call that confirms the end is returned too");
    stream_request(STREAM_SIZE);
    reader.fail_at = STREAM_SIZE;
    reader.fail_with = SKIFF_ERR_STORAGE_NO_MEDIA;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_MEDIA, drain(out, sizeof out, &total));
}

static void test_a_reader_overrunning_its_buffer_fails(void) {
    stream_request(STREAM_SIZE);
    reader.overflow = 1;
    unsigned char out[sizeof STREAM];
    size_t total = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, drain(out, sizeof out, &total));
    TEST_ASSERT_EQUAL_size_t(0, total);
}

static void test_reading_refuses_bad_arguments(void) {
    stream_request(STREAM_SIZE);
    unsigned char chunk[READ_CAPACITY];
    size_t sent = 0;
    size_t got = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_http_read_body(NULL, &sent, chunk, sizeof chunk, &got));
    TEST_ASSERT_EQUAL_size_t(0, got);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_http_read_body(&request, NULL, chunk, sizeof chunk, &got));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_http_read_body(&request, &sent, NULL, sizeof chunk, &got));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_http_read_body(&request, &sent, chunk, 0, &got));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_http_read_body(&request, &sent, chunk, sizeof chunk, NULL));
    sent = STREAM_SIZE + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_http_read_body(&request, &sent, chunk, sizeof chunk, &got));
    sent = 0;
    request.read_body = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_http_read_body(&request, &sent, chunk, sizeof chunk, &got));
    TEST_ASSERT_EQUAL_INT(0, reader.calls);
}

static void test_destroy(void) {
    skiff_transport_destroy(&transport.base);
    TEST_ASSERT_EQUAL_INT(1, transport.destroys);
    skiff_transport_destroy(NULL);
    skiff_transport bare = {NULL};
    skiff_transport_destroy(&bare);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_request_reaches_the_transport_with_a_reset_response);
    RUN_TEST(test_stop_hook_reaches_the_transport);
    RUN_TEST(test_null_and_missing_arguments);
    RUN_TEST(test_only_explicit_http_and_https_are_sent);
    RUN_TEST(test_transport_without_ops_is_refused);
    RUN_TEST(test_header_injection_is_refused);
    RUN_TEST(test_valid_headers_pass);
    RUN_TEST(test_if_range_needs_a_range_and_no_line_break);
    RUN_TEST(test_a_post_carries_its_body_and_content_type);
    RUN_TEST(test_bodies_belong_to_posts_and_ranges_to_gets);
    RUN_TEST(test_a_put_carries_its_body_as_a_post_does);
    RUN_TEST(test_a_post_streams_its_body_from_a_reader);
    RUN_TEST(test_a_reader_needs_a_post_a_size_and_no_body);
    RUN_TEST(test_reading_hands_out_exactly_body_size_bytes);
    RUN_TEST(test_a_reader_giving_less_per_call_is_asked_again);
    RUN_TEST(test_a_reader_is_never_asked_past_the_size);
    RUN_TEST(test_a_body_shorter_than_its_size_fails);
    RUN_TEST(test_a_reader_error_is_returned_unchanged);
    RUN_TEST(test_a_reader_overrunning_its_buffer_fails);
    RUN_TEST(test_reading_refuses_bad_arguments);
    RUN_TEST(test_destroy);
    return UNITY_END();
}
