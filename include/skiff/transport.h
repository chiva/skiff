#ifndef SKIFF_TRANSPORT_H
#define SKIFF_TRANSPORT_H

/*
 * The seam between Skiff and the network. Layers above (romm/, jobs/) send requests through a
 * skiff_transport and never see libcurl: the app uses the curl transport (skiff/curl_transport.h),
 * host tests a fake one that replays recorded RomM responses and injects failures
 * (tests/support/fake_transport.h). A transport keeps its connection alive between requests, so a
 * session pays the TLS handshake once. A transport is used by one thread at a time.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"
#include "skiff/http.h"

typedef struct skiff_http_header {
    const char *name;
    const char *value;
} skiff_http_header;

/* Receives the response body in chunks as it arrives. Any result other than SKIFF_OK stops the
 * transfer, and skiff_transport_perform() returns that result unchanged. By the first call the
 * status and headers are already in the skiff_http_response passed to skiff_transport_perform(), so
 * a download sink must check them (200 or 206 with the expected Content-Range) before writing: an
 * error page must never land in a partial file. */
typedef skiff_err (*skiff_http_body_fn)(void *ctx, const unsigned char *data, size_t size);

/* Asked whether to stop the transfer: about once a second while it runs, also when nothing arrives
 * (a Wi-Fi switch turned off delivers no bytes, so the body callback alone would only notice at the
 * stall timeout), and between body chunks. Any result other than SKIFF_OK stops the transfer, and
 * skiff_transport_perform() returns that result unchanged. */
typedef skiff_err (*skiff_http_stop_fn)(void *ctx);

typedef enum skiff_http_method {
    /* The zero value, so a request cleared with memset is a GET. */
    SKIFF_HTTP_GET,
    /* For RomM's pairing calls: a small body sent whole, with Content-Length. */
    SKIFF_HTTP_POST,
} skiff_http_method;

typedef struct skiff_http_request {
    skiff_http_method method;
    /* Must start with http:// or https://. */
    const char *url;
    /* Sent on this request after the transport's own default headers. */
    const skiff_http_header *headers;
    size_t header_count;
    /* POST only: body_size bytes sent as the request body (NULL for none), labelled with
     * "Content-Type: <content_type>" when content_type is set. Sent whole with Content-Length,
     * never with "Expect: 100-continue", which would cost a PSP a round trip per request. */
    const char *body;
    size_t body_size;
    const char *content_type;
    /* GET only. Resume: "Range: bytes=<range_start>-" plus "If-Range: <if_range>" (the ETag
     * recorded when the download started), so a file that changed on the server comes back whole
     * (200) instead of as a range (206) spliced onto the old start. A range always needs its
     * ETag. */
    int has_range;
    uint64_t range_start;
    const char *if_range;
    /* NULL discards the body. */
    skiff_http_body_fn on_body;
    void *body_ctx;
    /* NULL never stops the transfer. */
    skiff_http_stop_fn should_stop;
    void *stop_ctx;
} skiff_http_request;

typedef struct skiff_transport skiff_transport;

typedef struct skiff_transport_ops {
    skiff_err (*perform)(skiff_transport *transport, const skiff_http_request *request,
                         skiff_http_response *response);
    void (*destroy)(skiff_transport *transport);
} skiff_transport_ops;

/* Implementations embed this as their first member. */
struct skiff_transport {
    const skiff_transport_ops *ops;
};

/*
 * 1 if every header has a non-empty name and a value, the name has no ':' and neither has a CR or
 * LF (values come from config.ini and must not be able to inject header lines), and none is Range
 * or If-Range, which only has_range and if_range may set; 0 otherwise, or for NULL headers with a
 * non-zero count.
 */
int skiff_http_headers_valid(const skiff_http_header *headers, size_t count);

/*
 * 1 if url starts with an explicit http:// or https:// (any case), 0 otherwise or for NULL. curl
 * guesses a scheme for anything else, falling back to plain HTTP, which would send the token and
 * custom headers in the clear; config.ini's server address is held to the same rule.
 */
int skiff_http_url_scheme_valid(const char *url);

/*
 * Sends request and fills response. An HTTP response of any status is SKIFF_OK with
 * response->status set; map it with skiff_http_status_error(). Otherwise returns the network
 * failure (1xx codes), the body callback's or the stop hook's error, or SKIFF_ERR_INVALID_ARG for
 * a NULL argument, a missing URL, an unknown method, invalid headers (skiff_http_headers_valid()),
 * has_range without a non-blank if_range (or if_range without has_range), an if_range with a line
 * break, a body or content type on a GET, a range on a POST, a NULL body with a non-zero
 * body_size, or a content type that is empty or has a line break, and
 * SKIFF_ERR_CONFIG_INVALID_VALUE for a URL without an explicit http:// or https:// (curl would
 * guess plain HTTP). The response is reset first, so after a failure it holds whatever arrived
 * before it. The connection stays usable after any failure.
 */
skiff_err skiff_transport_perform(skiff_transport *transport, const skiff_http_request *request,
                                  skiff_http_response *response);

/* Closes the connection and frees the transport. Does nothing for NULL. */
void skiff_transport_destroy(skiff_transport *transport);

#endif
