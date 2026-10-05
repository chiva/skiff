#include "skiff/http.h"

#include <string.h>

enum {
    HTTP_STATUS_OK_FIRST = 200,
    HTTP_STATUS_OK_LAST = 299,
    HTTP_STATUS_UNAUTHORIZED = 401,
    HTTP_STATUS_FORBIDDEN = 403,
    HTTP_STATUS_NOT_FOUND = 404,
    HTTP_STATUS_REQUEST_TIMEOUT = 408,
    HTTP_STATUS_BAD_GATEWAY = 502,
    HTTP_STATUS_GATEWAY_TIMEOUT = 504,
    HTTP_STATUS_SERVER_FIRST = 500,
    HTTP_STATUS_SERVER_LAST = 599,
    HTTP_STATUS_DIGITS = 3,
    DECIMAL_BASE = 10,
};

static const char STATUS_LINE_PREFIX[] = "HTTP/";
static const char RANGE_UNIT_PREFIX[] = "bytes ";

/* A view of part of a header line: not NUL-terminated. */
typedef struct span {
    const char *text;
    size_t length;
} span;

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static span trim(span s) {
    while (s.length > 0 && is_space(s.text[0])) {
        s.text++;
        s.length--;
    }
    while (s.length > 0 && is_space(s.text[s.length - 1])) {
        s.length--;
    }
    return s;
}

static int equals_ignoring_case(span s, const char *expected) {
    const size_t expected_length = strlen(expected);
    if (s.length != expected_length) {
        return 0;
    }
    for (size_t i = 0; i < expected_length; i++) {
        if (ascii_lower(s.text[i]) != expected[i]) {
            return 0;
        }
    }
    return 1;
}

static int starts_with(span s, const char *prefix, size_t prefix_length) {
    return s.length >= prefix_length && memcmp(s.text, prefix, prefix_length) == 0;
}

/* Parses the decimal digits at the start of *s into *value and advances past them. Fails on no
 * digits or on overflow; newlib's strtoull is not relied on for 64-bit sizes. */
static int take_uint64(span *s, uint64_t *value) {
    uint64_t result = 0;
    size_t used = 0;
    while (used < s->length && is_digit(s->text[used])) {
        const uint64_t digit = (uint64_t)(s->text[used] - '0');
        if (result > (UINT64_MAX - digit) / DECIMAL_BASE) {
            return 0;
        }
        result = result * DECIMAL_BASE + digit;
        used++;
    }
    if (used == 0) {
        return 0;
    }
    *value = result;
    s->text += used;
    s->length -= used;
    return 1;
}

static int take_char(span *s, char expected) {
    if (s->length == 0 || s->text[0] != expected) {
        return 0;
    }
    s->text++;
    s->length--;
    return 1;
}

static void clear_header_fields(skiff_http_response *response) {
    response->etag[0] = '\0';
    response->has_content_length = 0;
    response->content_length = 0;
    response->has_content_range = 0;
    response->range_start = 0;
    response->range_end = 0;
    response->range_total = 0;
}

/* "HTTP/1.1 206 Partial Content" or "HTTP/2 200": exactly three digits after the first space. */
static void parse_status_line(skiff_http_response *response, span line) {
    clear_header_fields(response);
    response->status = 0;
    const char *space = memchr(line.text, ' ', line.length);
    if (space == NULL) {
        return;
    }
    span rest = {space + 1, line.length - (size_t)(space + 1 - line.text)};
    const size_t before = rest.length;
    uint64_t status = 0;
    if (take_uint64(&rest, &status) && before - rest.length == HTTP_STATUS_DIGITS &&
        (rest.length == 0 || rest.text[0] == ' ')) {
        response->status = (long)status;
    }
}

static void parse_etag(skiff_http_response *response, span value) {
    if (value.length == 0 || value.length >= SKIFF_HTTP_ETAG_MAX) {
        response->etag[0] = '\0';
        return;
    }
    memcpy(response->etag, value.text, value.length);
    response->etag[value.length] = '\0';
}

static void parse_content_length(skiff_http_response *response, span value) {
    uint64_t length = 0;
    response->has_content_length = take_uint64(&value, &length) && value.length == 0;
    response->content_length = response->has_content_length ? length : 0;
}

/* "bytes <start>-<end>/<total>" with start <= end < total. */
static void parse_content_range(skiff_http_response *response, span value) {
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t total = 0;
    const size_t prefix_length = sizeof RANGE_UNIT_PREFIX - 1;
    response->has_content_range = 0;
    if (!starts_with(value, RANGE_UNIT_PREFIX, prefix_length)) {
        return;
    }
    value.text += prefix_length;
    value.length -= prefix_length;
    if (!take_uint64(&value, &start) || !take_char(&value, '-') || !take_uint64(&value, &end) ||
        !take_char(&value, '/') || !take_uint64(&value, &total) || value.length != 0 ||
        start > end || end >= total) {
        return;
    }
    response->has_content_range = 1;
    response->range_start = start;
    response->range_end = end;
    response->range_total = total;
}

void skiff_http_response_reset(skiff_http_response *response) {
    if (response != NULL) {
        memset(response, 0, sizeof *response);
    }
}

skiff_err skiff_http_response_parse_header(skiff_http_response *response, const char *line,
                                           size_t length) {
    if (response == NULL || (line == NULL && length > 0)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const span whole = trim((span){line, length});
    if (starts_with(whole, STATUS_LINE_PREFIX, sizeof STATUS_LINE_PREFIX - 1)) {
        parse_status_line(response, whole);
        return SKIFF_OK;
    }
    const char *colon = whole.length > 0 ? memchr(whole.text, ':', whole.length) : NULL;
    if (colon == NULL) {
        return SKIFF_OK;
    }
    const span name = trim((span){whole.text, (size_t)(colon - whole.text)});
    const span value = trim((span){colon + 1, whole.length - (size_t)(colon - whole.text) - 1});
    if (equals_ignoring_case(name, "etag")) {
        parse_etag(response, value);
    } else if (equals_ignoring_case(name, "content-length")) {
        parse_content_length(response, value);
    } else if (equals_ignoring_case(name, "content-range")) {
        parse_content_range(response, value);
    }
    return SKIFF_OK;
}

skiff_err skiff_http_status_error(long status) {
    if (status >= HTTP_STATUS_OK_FIRST && status <= HTTP_STATUS_OK_LAST) {
        return SKIFF_OK;
    }
    switch (status) {
    case HTTP_STATUS_UNAUTHORIZED:
        return SKIFF_ERR_ROMM_UNAUTHORIZED;
    case HTTP_STATUS_FORBIDDEN:
        return SKIFF_ERR_ROMM_FORBIDDEN;
    case HTTP_STATUS_NOT_FOUND:
        return SKIFF_ERR_ROMM_NOT_FOUND;
    case HTTP_STATUS_REQUEST_TIMEOUT:
    case HTTP_STATUS_GATEWAY_TIMEOUT:
        return SKIFF_ERR_NET_TIMEOUT;
    case HTTP_STATUS_BAD_GATEWAY:
        return SKIFF_ERR_NET_CONNECT;
    default:
        break;
    }
    if (status >= HTTP_STATUS_SERVER_FIRST && status <= HTTP_STATUS_SERVER_LAST) {
        return SKIFF_ERR_ROMM_SERVER;
    }
    return SKIFF_ERR_ROMM_BAD_RESPONSE;
}
