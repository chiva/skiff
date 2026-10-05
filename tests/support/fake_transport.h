#ifndef SKIFF_TEST_FAKE_TRANSPORT_H
#define SKIFF_TEST_FAKE_TRANSPORT_H

/*
 * A skiff_transport for host tests of the layers above net/ (romm/, jobs/): it replays recorded
 * RomM responses (the .http files in tests/fixtures/romm/, made by `scripts/dev.sh romm-record`)
 * and injects the failures a PSP on Wi-Fi meets: timeouts, connections cut mid-download, a file
 * that changed on the server. It reads headers with the curl transport's parser
 * (skiff_http_response_parse_header()), so both see a response the same way.
 *
 * A fixture is the response as curl received it: status line and headers, a blank line, then the
 * body (already de-chunked, so a "Transfer-Encoding: chunked" header is ignored).
 *
 * Range requests are answered the way RomM's server does, from the recorded 200 body: a matching
 * If-Range (or none) gives 206 with Content-Range, a stale one the whole body with 200, an offset
 * at or past the end 416.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/transport.h"

#define FAKE_TRANSPORT_MAX_ROUTES 16
#define FAKE_TRANSPORT_MAX_LOG 16
#define FAKE_TRANSPORT_URL_MAX 256
#define FAKE_TRANSPORT_HEADERS_MAX 512
/* Body bytes per call to the body callback, so a test sees more than one chunk. */
#define FAKE_TRANSPORT_CHUNK_BYTES 1024

typedef struct fake_route {
    /* Matched against the URL after the scheme and host: path and query. */
    char path[FAKE_TRANSPORT_URL_MAX];
    char *raw; /* owned */
    size_t raw_size;
    /* Fail before any response arrives (e.g. SKIFF_ERR_NET_CONNECT); SKIFF_OK for none. */
    skiff_err fail_before_response;
    /* Stop with fail_mid_body (e.g. SKIFF_ERR_NET_TIMEOUT or SKIFF_ERR_NET_CONNECTION_LOST) after
     * fail_after_bytes body bytes, or the whole body if shorter; SKIFF_OK for none. */
    uint64_t fail_after_bytes;
    skiff_err fail_mid_body;
    /* Replaces the recorded ETag: the file changed on the server since it was recorded. */
    const char *current_etag;
} fake_route;

/* One request as the fake received it. */
typedef struct fake_request {
    char url[FAKE_TRANSPORT_URL_MAX];
    int has_range;
    uint64_t range_start;
    char if_range[SKIFF_HTTP_ETAG_MAX];
    /* "Name: value\n" for every request header, in order. */
    char headers[FAKE_TRANSPORT_HEADERS_MAX];
} fake_request;

typedef struct fake_transport {
    skiff_transport base;
    fake_route routes[FAKE_TRANSPORT_MAX_ROUTES];
    size_t route_count;
    fake_request log[FAKE_TRANSPORT_MAX_LOG];
    size_t log_count;
    /* Requests beyond the log's capacity are counted here too. */
    size_t request_count;
    int connected;
} fake_transport;

/* Starts an empty fake. Free its routes with skiff_transport_destroy(&fake->base). */
void fake_transport_init(fake_transport *fake);

/* Serves path from fixture_file (relative to SKIFF_FIXTURES_DIR). NULL if it cannot be read or
 * parsed, or the route table is full. */
fake_route *fake_transport_add_fixture(fake_transport *fake, const char *path,
                                       const char *fixture_file);

/* Serves path from a raw response in memory (copied). NULL if the table is full. */
fake_route *fake_transport_add_raw(fake_transport *fake, const char *path, const char *raw,
                                   size_t raw_size);

#endif
