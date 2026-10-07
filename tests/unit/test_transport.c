/* The checks skiff_transport_perform() makes before any transport sees a request. */
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
    request.has_range = 0;
    request.if_range = NULL;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    request.method = (skiff_http_method)7;
    expect_refused("an unknown method");
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
    RUN_TEST(test_destroy);
    return UNITY_END();
}
