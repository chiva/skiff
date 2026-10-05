#include <string.h>

#include "skiff/http.h"

#include "unity.h"

static skiff_http_response response;

void setUp(void) { skiff_http_response_reset(&response); }

void tearDown(void) {}

static void parse(const char *line) {
    TEST_PRINTF("header> %s", line);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_http_response_parse_header(&response, line, strlen(line)));
}

typedef struct status_row {
    const char *line;
    long status;
} status_row;

static void test_status_lines(void) {
    // clang-format off
    static const status_row ROWS[] = {
        {"HTTP/1.1 200 OK\r\n", 200},
        {"HTTP/1.1 206 Partial Content", 206},
        {"HTTP/2 404", 404},
        {"HTTP/1.0 503 Service Unavailable\r\n", 503},
        {"HTTP/1.1 20 Short", 0},
        {"HTTP/1.1 2000 Long", 0},
        {"HTTP/1.1 abc", 0},
        {"HTTP/1.1", 0},
        {"HTTP/1.1 200x", 0},
    };
    // clang-format on
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) {
        response.status = -1;
        parse(ROWS[i].line);
        TEST_ASSERT_EQUAL_INT64_MESSAGE(ROWS[i].status, response.status, ROWS[i].line);
    }
}

static void test_etag_is_kept_verbatim_and_names_ignore_case(void) {
    parse("ETag: \"5f2b-1000\"\r\n");
    TEST_ASSERT_EQUAL_STRING("\"5f2b-1000\"", response.etag);
    parse("etag:W/\"weak\"  ");
    TEST_ASSERT_EQUAL_STRING("W/\"weak\"", response.etag);
}

static void test_oversized_etag_counts_as_absent(void) {
    char line[SKIFF_HTTP_ETAG_MAX + 16];
    strcpy(line, "ETag: ");
    memset(line + strlen(line), 'a', SKIFF_HTTP_ETAG_MAX);
    line[strlen("ETag: ") + SKIFF_HTTP_ETAG_MAX] = '\0';
    parse("ETag: \"short\"");
    parse(line);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", response.etag, "a cut ETag would never match If-Range");
}

static void test_longest_etag_that_fits_is_kept(void) {
    char line[SKIFF_HTTP_ETAG_MAX + 16];
    strcpy(line, "ETag: ");
    const size_t prefix = strlen(line);
    memset(line + prefix, 'b', SKIFF_HTTP_ETAG_MAX - 1);
    line[prefix + SKIFF_HTTP_ETAG_MAX - 1] = '\0';
    parse(line);
    TEST_ASSERT_EQUAL_size_t(SKIFF_HTTP_ETAG_MAX - 1, strlen(response.etag));
}

static void test_content_length(void) {
    parse("Content-Length: 1048576\r\n");
    TEST_ASSERT_TRUE(response.has_content_length);
    TEST_ASSERT_EQUAL_UINT64(1048576U, response.content_length);
    parse("CONTENT-LENGTH: 12x");
    TEST_ASSERT_FALSE_MESSAGE(response.has_content_length, "trailing junk is malformed");
    parse("Content-Length: 18446744073709551616");
    TEST_ASSERT_FALSE_MESSAGE(response.has_content_length, "2^64 overflows");
    parse("Content-Length: 18446744073709551615");
    TEST_ASSERT_TRUE(response.has_content_length);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, response.content_length);
}

typedef struct range_row {
    const char *line;
    int valid;
    uint64_t start;
    uint64_t end;
    uint64_t total;
} range_row;

static void test_content_range(void) {
    static const range_row ROWS[] = {
        {"Content-Range: bytes 1000-1048575/1048576\r\n", 1, 1000U, 1048575U, 1048576U},
        {"content-range: bytes 0-0/1", 1, 0U, 0U, 1U},
        {"Content-Range: bytes 4294967296-8589934591/8589934592", 1, 4294967296ULL, 8589934591ULL,
         8589934592ULL},
        {"Content-Range: bytes 0-9/*", 0, 0, 0, 0},
        {"Content-Range: bytes */100", 0, 0, 0, 0},
        {"Content-Range: items 0-9/10", 0, 0, 0, 0},
        {"Content-Range: bytes 10-9/100", 0, 0, 0, 0},
        {"Content-Range: bytes 0-100/100", 0, 0, 0, 0},
        {"Content-Range: bytes 0-9/10 extra", 0, 0, 0, 0},
        {"Content-Range: bytes 0-9", 0, 0, 0, 0},
    };
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) {
        skiff_http_response_reset(&response);
        parse(ROWS[i].line);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ROWS[i].valid, response.has_content_range, ROWS[i].line);
        if (ROWS[i].valid) {
            TEST_ASSERT_EQUAL_UINT64(ROWS[i].start, response.range_start);
            TEST_ASSERT_EQUAL_UINT64(ROWS[i].end, response.range_end);
            TEST_ASSERT_EQUAL_UINT64(ROWS[i].total, response.range_total);
        }
    }
}

static void test_status_line_clears_earlier_headers(void) {
    parse("HTTP/1.1 100 Continue");
    parse("ETag: \"interim\"");
    parse("Content-Length: 5");
    parse("Content-Range: bytes 0-4/5");
    parse("HTTP/1.1 200 OK");
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_STRING("", response.etag);
    TEST_ASSERT_FALSE(response.has_content_length);
    TEST_ASSERT_FALSE(response.has_content_range);
}

static void test_other_lines_are_ignored(void) {
    parse("HTTP/1.1 200 OK");
    parse("Content-Type: application/json");
    parse("no colon here");
    parse("\r\n");
    parse(": empty name");
    TEST_ASSERT_EQUAL_INT64(200, response.status);
    TEST_ASSERT_EQUAL_STRING("", response.etag);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_http_response_parse_header(&response, NULL, 0));
}

static void test_line_is_not_read_past_its_length(void) {
    const char line[] = "Content-Length: 123456";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_http_response_parse_header(&response, line, 19));
    TEST_ASSERT_EQUAL_UINT64(123U, response.content_length);
}

static void test_null_arguments(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_http_response_parse_header(NULL, "ETag: x", 7));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_http_response_parse_header(&response, NULL, 3));
    skiff_http_response_reset(NULL);
}

typedef struct status_error_row {
    long status;
    skiff_err expected;
} status_error_row;

static void test_status_errors(void) {
    static const status_error_row ROWS[] = {
        {200, SKIFF_OK},
        {206, SKIFF_OK},
        {299, SKIFF_OK},
        {301, SKIFF_ERR_ROMM_BAD_RESPONSE},
        {400, SKIFF_ERR_ROMM_BAD_RESPONSE},
        {401, SKIFF_ERR_ROMM_UNAUTHORIZED},
        {403, SKIFF_ERR_ROMM_FORBIDDEN},
        {404, SKIFF_ERR_ROMM_NOT_FOUND},
        {408, SKIFF_ERR_NET_TIMEOUT},
        {416, SKIFF_ERR_ROMM_BAD_RESPONSE},
        {500, SKIFF_ERR_ROMM_SERVER},
        {502, SKIFF_ERR_NET_CONNECT},
        {503, SKIFF_ERR_ROMM_SERVER},
        {504, SKIFF_ERR_NET_TIMEOUT},
        {599, SKIFF_ERR_ROMM_SERVER},
        {600, SKIFF_ERR_ROMM_BAD_RESPONSE},
        {0, SKIFF_ERR_ROMM_BAD_RESPONSE},
        {199, SKIFF_ERR_ROMM_BAD_RESPONSE},
    };
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++) {
        const skiff_err actual = skiff_http_status_error(ROWS[i].status);
        TEST_PRINTF("HTTP %ld -> %s", ROWS[i].status, skiff_err_name(actual));
        TEST_ASSERT_EQUAL_INT(ROWS[i].expected, actual);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_status_lines);
    RUN_TEST(test_etag_is_kept_verbatim_and_names_ignore_case);
    RUN_TEST(test_oversized_etag_counts_as_absent);
    RUN_TEST(test_longest_etag_that_fits_is_kept);
    RUN_TEST(test_content_length);
    RUN_TEST(test_content_range);
    RUN_TEST(test_status_line_clears_earlier_headers);
    RUN_TEST(test_other_lines_are_ignored);
    RUN_TEST(test_line_is_not_read_past_its_length);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_status_errors);
    return UNITY_END();
}
