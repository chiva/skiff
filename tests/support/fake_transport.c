#include "fake_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_END "\r\n"
#define SCHEME_SEPARATOR "://"

enum {
    HTTP_STATUS_OK = 200,
    HTTP_STATUS_PARTIAL = 206,
    HTTP_STATUS_NOT_FOUND = 404,
    HTTP_STATUS_RANGE_NOT_SATISFIABLE = 416
};

static skiff_err fake_perform(skiff_transport *base, const skiff_http_request *request,
                              skiff_http_response *response);
static void fake_destroy(skiff_transport *base);

static const skiff_transport_ops FAKE_TRANSPORT_OPS = {fake_perform, fake_destroy};

void fake_transport_init(fake_transport *fake) {
    memset(fake, 0, sizeof *fake);
    fake->base.ops = &FAKE_TRANSPORT_OPS;
}

fake_route *fake_transport_add_raw(fake_transport *fake, const char *path, const char *raw,
                                   size_t raw_size) {
    if (fake->route_count == FAKE_TRANSPORT_MAX_ROUTES || strlen(path) >= FAKE_TRANSPORT_URL_MAX) {
        return NULL;
    }
    char *copy = malloc(raw_size);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, raw, raw_size);
    fake_route *route = &fake->routes[fake->route_count++];
    memset(route, 0, sizeof *route);
    snprintf(route->path, sizeof route->path, "%s", path);
    route->raw = copy;
    route->raw_size = raw_size;
    return route;
}

fake_route *fake_transport_add_fixture(fake_transport *fake, const char *path,
                                       const char *fixture_file) {
    char file_path[FAKE_TRANSPORT_URL_MAX];
    snprintf(file_path, sizeof file_path, "%s/%s", SKIFF_FIXTURES_DIR, fixture_file);
    FILE *file = fopen(file_path, "rb");
    if (file == NULL) {
        return NULL;
    }
    fake_route *route = NULL;
    long size = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        size = ftell(file);
    }
    char *raw = size > 0 ? malloc((size_t)size) : NULL;
    if (raw != NULL && fseek(file, 0, SEEK_SET) == 0 &&
        fread(raw, 1, (size_t)size, file) == (size_t)size) {
        route = fake_transport_add_raw(fake, path, raw, (size_t)size);
    }
    free(raw);
    fclose(file);
    return route;
}

/* "https://host:port/api/x?y" -> "/api/x?y". */
static const char *path_of(const char *url) {
    const char *scheme_end = strstr(url, SCHEME_SEPARATOR);
    const char *host = scheme_end != NULL ? scheme_end + strlen(SCHEME_SEPARATOR) : url;
    const char *path = strchr(host, '/');
    return path != NULL ? path : "/";
}

static fake_route *find_route(fake_transport *fake, const skiff_http_request *request) {
    const char *path = path_of(request->url);
    for (size_t i = 0; i < fake->route_count; i++) {
        fake_route *route = &fake->routes[i];
        if (strcmp(route->path, path) == 0 &&
            (!route->match_method || route->method == request->method) &&
            (route->max_uses == 0 || route->uses < route->max_uses)) {
            route->uses++;
            return route;
        }
    }
    return NULL;
}

static void log_request(fake_transport *fake, const skiff_http_request *request) {
    fake->request_count++;
    if (fake->log_count == FAKE_TRANSPORT_MAX_LOG) {
        return;
    }
    fake_request *entry = &fake->log[fake->log_count++];
    memset(entry, 0, sizeof *entry);
    entry->method = request->method;
    snprintf(entry->url, sizeof entry->url, "%s", request->url);
    if (request->body != NULL) {
        const size_t kept = request->body_size < sizeof entry->body - 1 ? request->body_size
                                                                        : sizeof entry->body - 1;
        memcpy(entry->body, request->body, kept);
        entry->body[kept] = '\0';
    }
    entry->body_size = request->body_size;
    if (request->content_type != NULL) {
        snprintf(entry->content_type, sizeof entry->content_type, "%s", request->content_type);
    }
    entry->has_range = request->has_range;
    entry->range_start = request->range_start;
    if (request->if_range != NULL) {
        snprintf(entry->if_range, sizeof entry->if_range, "%s", request->if_range);
    }
    size_t used = 0;
    for (size_t i = 0; i < request->header_count && used < sizeof entry->headers; i++) {
        const int written =
            snprintf(entry->headers + used, sizeof entry->headers - used, "%s: %s\n",
                     request->headers[i].name, request->headers[i].value);
        used += written > 0 ? (size_t)written : 0;
    }
}

/* Feeds each header line of raw to the parser; returns the body's offset, or raw_size when the
 * response has no blank line. */
static size_t parse_headers(const fake_route *route, skiff_http_response *response) {
    const char *raw = route->raw;
    const char *end = raw + route->raw_size;
    const char *line = raw;
    while (line < end) {
        const char *line_end = line;
        while (line_end + 1 < end && !(line_end[0] == '\r' && line_end[1] == '\n')) {
            line_end++;
        }
        if (line_end + 1 >= end) {
            break;
        }
        if (line_end == line) {
            return (size_t)(line_end + strlen(LINE_END) - raw);
        }
        skiff_http_response_parse_header(response, line, (size_t)(line_end - line));
        line = line_end + strlen(LINE_END);
    }
    return route->raw_size;
}

/* Answers a Range request from a recorded 200 body as an HTTP server would; adjusts *body/size. */
static void apply_range(const fake_route *route, const skiff_http_request *request,
                        skiff_http_response *response, const char **body, size_t *size) {
    if (!request->has_range || route->ignore_range || response->status != HTTP_STATUS_OK) {
        return;
    }
    /* The transport refuses a range without If-Range before the fake sees it. */
    const int etag_matches =
        response->etag[0] != '\0' && strcmp(request->if_range, response->etag) == 0;
    if (!etag_matches) {
        return;
    }
    const uint64_t total = *size;
    if (request->range_start >= total) {
        response->status = HTTP_STATUS_RANGE_NOT_SATISFIABLE;
        response->has_content_length = 1;
        response->content_length = 0;
        *size = 0;
        return;
    }
    response->status = HTTP_STATUS_PARTIAL;
    response->has_content_range = 1;
    response->range_start = request->range_start;
    response->range_end = total - 1;
    response->range_total = total;
    *body += request->range_start;
    *size -= (size_t)request->range_start;
    response->has_content_length = 1;
    response->content_length = *size;
}

/* The request's stop hook, as the curl transport polls it; SKIFF_OK without one. */
static skiff_err stop_requested(const skiff_http_request *request) {
    return request->should_stop != NULL ? request->should_stop(request->stop_ctx) : SKIFF_OK;
}

/* Passes the body to the callback in chunks, asking the stop hook before each; with a mid-body
 * failure, stops after fail_after_bytes (or the whole body, if shorter) and reports it. */
static skiff_err deliver_body(fake_transport *fake, const fake_route *route,
                              const skiff_http_request *request, skiff_http_response *response,
                              const char *body, size_t size) {
    const int fails = route->fail_mid_body != SKIFF_OK;
    const size_t limit =
        fails && route->fail_after_bytes < size ? (size_t)route->fail_after_bytes : size;
    for (size_t delivered = 0; delivered < limit;) {
        const size_t left = limit - delivered;
        const size_t chunk = left < FAKE_TRANSPORT_CHUNK_BYTES ? left : FAKE_TRANSPORT_CHUNK_BYTES;
        const skiff_err stop = stop_requested(request);
        if (stop != SKIFF_OK) {
            fake->connected = 0;
            return stop;
        }
        if (request->on_body != NULL) {
            const skiff_err err =
                request->on_body(request->body_ctx, (const unsigned char *)body + delivered, chunk);
            if (err != SKIFF_OK) {
                fake->connected = 0;
                return err;
            }
        }
        delivered += chunk;
        response->body_bytes += chunk;
    }
    if (fails) {
        fake->connected = 0;
        return route->fail_mid_body;
    }
    return SKIFF_OK;
}

static skiff_err fake_perform(skiff_transport *base, const skiff_http_request *request,
                              skiff_http_response *response) {
    fake_transport *fake = (fake_transport *)base;
    log_request(fake, request);
    response->new_connections = fake->connected ? 0 : 1;
    fake->connected = 1;
    const fake_route *route = find_route(fake, request);
    if (route == NULL) {
        response->status = HTTP_STATUS_NOT_FOUND;
        return SKIFF_OK;
    }
    const skiff_err before = route->fail_before_response != SKIFF_OK ? route->fail_before_response
                                                                     : stop_requested(request);
    if (before != SKIFF_OK) {
        fake->connected = 0;
        return before;
    }
    const size_t body_offset = parse_headers(route, response);
    if (route->current_etag != NULL) {
        snprintf(response->etag, sizeof response->etag, "%s", route->current_etag);
    }
    const char *body = route->raw + body_offset;
    size_t size = route->raw_size - body_offset;
    apply_range(route, request, response, &body, &size);
    return deliver_body(fake, route, request, response, body, size);
}

static void fake_destroy(skiff_transport *base) {
    fake_transport *fake = (fake_transport *)base;
    for (size_t i = 0; i < fake->route_count; i++) {
        free(fake->routes[i].raw);
        fake->routes[i].raw = NULL;
    }
    fake->route_count = 0;
}
