#ifndef SKIFF_HTTP_H
#define SKIFF_HTTP_H

/*
 * What Skiff keeps from an HTTP response: the status and the few headers downloads depend on
 * (ETag for If-Range, Content-Length, Content-Range for resuming). Every transport fills it through
 * skiff_http_response_parse_header(), one header line at a time, so the real (curl) and fake
 * transports cannot read the same response differently. Bodies never land here: they stream
 * through the request's body callback (skiff/transport.h).
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"

/* Room for an ETag and its terminator. A longer ETag is treated as absent, never truncated: a cut
 * ETag sent in If-Range would never match, and a resumed download would silently restart. */
#define SKIFF_HTTP_ETAG_MAX 128

typedef struct skiff_http_response {
    long status;
    char etag[SKIFF_HTTP_ETAG_MAX];
    int has_content_length;
    uint64_t content_length;
    /* Content-Range: bytes <range_start>-<range_end>/<range_total>; an unknown total ("*") or any
     * malformed value leaves has_content_range at 0. */
    int has_content_range;
    uint64_t range_start;
    uint64_t range_end;
    uint64_t range_total;
    /* Body bytes passed to the body callback. */
    uint64_t body_bytes;
    /* Connections the transport opened for this request: 0 when it reused a kept-alive one. */
    long new_connections;
} skiff_http_response;

/* Clears every field. Does nothing for NULL. */
void skiff_http_response_reset(skiff_http_response *response);

/*
 * Reads one raw header line (with or without its CRLF, not NUL-terminated). A status line
 * ("HTTP/1.1 206 Partial Content") sets status and clears the header fields, because curl passes
 * the headers of every response it reads, interim ones included. Names are case-insensitive; lines
 * Skiff does not use, and malformed values, are ignored. Returns SKIFF_ERR_INVALID_ARG for a NULL
 * response, or a NULL line with a non-zero length.
 */
skiff_err skiff_http_response_parse_header(skiff_http_response *response, const char *line,
                                           size_t length);

/*
 * The error a RomM request gets for an HTTP status: SKIFF_OK for 2xx; 401, 403 and 404 as RomM
 * refusals; 408 and 504 as timeouts; 502 as an unreachable server behind a proxy; other 5xx as a
 * RomM server error; anything else as a response Skiff does not understand.
 */
skiff_err skiff_http_status_error(long status);

#endif
