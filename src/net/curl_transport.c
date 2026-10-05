#include "skiff/curl_transport.h"

#include <curl/curl.h>
#include <mbedtls/platform_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Below one byte per second for the stall timeout counts as stalled. */
#define CURL_STALL_BYTES_PER_SECOND 1L
#define CURL_ALLOWED_PROTOCOLS "http,https"
#define HTTPS_PREFIX "https://"
/* "Range: bytes=" plus a 64-bit offset, "-" and the terminator. */
#define RANGE_HEADER_MAX 48
#define HEADER_SEPARATOR ": "

typedef struct curl_transport {
    skiff_transport base; /* first, so a skiff_transport * is a curl_transport * */
    CURL *curl;
    struct curl_slist *default_headers;
    int client_cert_configured;
} curl_transport;

/* State of one request, shared with curl's callbacks. */
typedef struct transfer {
    const skiff_http_request *request;
    skiff_http_response *response;
    skiff_err body_error;
} transfer;

skiff_err skiff_net_global_init(void) {
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? SKIFF_OK : SKIFF_ERR_NET_ENTROPY;
}

void skiff_net_global_cleanup(void) { curl_global_cleanup(); }

/* curl's callback signature takes a non-const buffer. */
// cppcheck-suppress constParameterCallback
static size_t on_header(char *buffer, size_t size, size_t count, void *userdata) {
    transfer *current = userdata;
    const size_t length = size * count;
    skiff_http_response_parse_header(current->response, buffer, length);
    return length;
}

static size_t on_body(char *buffer, size_t size, size_t count, void *userdata) {
    transfer *current = userdata;
    const size_t length = size * count;
    if (current->request->on_body != NULL) {
        const skiff_err err =
            current->request->on_body(current->request->body_ctx, (unsigned char *)buffer, length);
        if (err != SKIFF_OK) {
            current->body_error = err;
            return 0; /* anything short of length makes curl stop with CURLE_WRITE_ERROR */
        }
    }
    current->response->body_bytes += length;
    return length;
}

/* Appends "<name>: <value>" to *list; returns 0 when out of memory (the list stays valid). */
static int append_header(struct curl_slist **list, const char *name, const char *value) {
    const size_t line_size = strlen(name) + sizeof HEADER_SEPARATOR - 1 + strlen(value) + 1;
    char *line = malloc(line_size);
    if (line == NULL) {
        return 0;
    }
    snprintf(line, line_size, "%s" HEADER_SEPARATOR "%s", name, value);
    struct curl_slist *grown = curl_slist_append(*list, line);
    free(line);
    if (grown == NULL) {
        return 0;
    }
    *list = grown;
    return 1;
}

/* curl_slist_append() copies each line, so the result owns its strings. */
static int append_line(struct curl_slist **list, const char *line) {
    struct curl_slist *grown = curl_slist_append(*list, line);
    if (grown == NULL) {
        return 0;
    }
    *list = grown;
    return 1;
}

/* The headers for one request: the transport's defaults, the request's own, then Range/If-Range. */
static skiff_err build_headers(const curl_transport *transport, const skiff_http_request *request,
                               struct curl_slist **out) {
    struct curl_slist *list = NULL;
    int ok = 1;
    for (const struct curl_slist *item = transport->default_headers; ok && item != NULL;
         item = item->next) {
        ok = append_line(&list, item->data);
    }
    for (size_t i = 0; ok && i < request->header_count; i++) {
        ok = append_header(&list, request->headers[i].name, request->headers[i].value);
    }
    if (ok && request->has_range) {
        char range[RANGE_HEADER_MAX];
        snprintf(range, sizeof range, "Range: bytes=%llu-",
                 (unsigned long long)request->range_start);
        ok = append_line(&list, range);
    }
    if (ok && request->if_range != NULL) {
        ok = append_header(&list, "If-Range", request->if_range);
    }
    if (!ok) {
        curl_slist_free_all(list);
        return SKIFF_ERR_NO_MEMORY;
    }
    *out = list;
    return SKIFF_OK;
}

static skiff_err describe_failure(curl_transport *transport, const skiff_http_request *request,
                                  const transfer *current, CURLcode code) {
    curl_off_t tls_done = 0;
    curl_easy_getinfo(transport->curl, CURLINFO_APPCONNECT_TIME_T, &tls_done);
    const skiff_net_failure failure = {
        .curl_code = (int)code,
        .clock_now = (int64_t)mbedtls_time(NULL),
        .uses_tls = curl_strnequal(request->url, HTTPS_PREFIX, sizeof HTTPS_PREFIX - 1),
        .tls_established = tls_done > 0,
        .reused_connection = current->response->new_connections == 0,
        .got_response = current->response->status != 0,
        .client_cert_configured = transport->client_cert_configured,
        .body_error = current->body_error,
    };
    return skiff_net_error_from_curl(&failure);
}

static skiff_err curl_perform(skiff_transport *base, const skiff_http_request *request,
                              skiff_http_response *response) {
    curl_transport *transport = (curl_transport *)base;
    transfer current = {request, response, SKIFF_OK};
    struct curl_slist *headers = NULL;
    skiff_err err = build_headers(transport, request, &headers);
    if (err != SKIFF_OK) {
        return err;
    }
    CURLcode code = CURLE_OK;
    if (curl_easy_setopt(transport->curl, CURLOPT_URL, request->url) != CURLE_OK ||
        curl_easy_setopt(transport->curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(transport->curl, CURLOPT_HEADERDATA, &current) != CURLE_OK ||
        curl_easy_setopt(transport->curl, CURLOPT_WRITEDATA, &current) != CURLE_OK) {
        code = CURLE_OUT_OF_MEMORY;
    } else {
        code = curl_easy_perform(transport->curl);
    }
    long connections = 0;
    curl_easy_getinfo(transport->curl, CURLINFO_NUM_CONNECTS, &connections);
    response->new_connections = connections;
    err = code == CURLE_OK ? SKIFF_OK : describe_failure(transport, request, &current, code);
    /* The list and the callback state die with this call; curl must not keep pointers to them. */
    curl_easy_setopt(transport->curl, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(transport->curl, CURLOPT_HEADERDATA, NULL);
    curl_easy_setopt(transport->curl, CURLOPT_WRITEDATA, NULL);
    curl_slist_free_all(headers);
    return err;
}

static void curl_destroy(skiff_transport *base) {
    curl_transport *transport = (curl_transport *)base;
    curl_easy_cleanup(transport->curl);
    curl_slist_free_all(transport->default_headers);
    free(transport);
}

static const skiff_transport_ops CURL_TRANSPORT_OPS = {curl_perform, curl_destroy};

static int set_optional_string(CURL *curl, CURLoption option, const char *value) {
    return value == NULL || curl_easy_setopt(curl, option, value) == CURLE_OK;
}

/* Options that hold for every request; curl copies the strings. */
static int configure(curl_transport *transport, const skiff_curl_config *config) {
    CURL *curl = transport->curl;
    const long connect_timeout =
        config->connect_timeout_s > 0 ? config->connect_timeout_s : SKIFF_CURL_CONNECT_TIMEOUT_S;
    const long stall_timeout =
        config->stall_timeout_s > 0 ? config->stall_timeout_s : SKIFF_CURL_STALL_TIMEOUT_S;
    return curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, CURL_ALLOWED_PROTOCOLS) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, CURL_STALL_BYTES_PER_SECOND) ==
               CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, stall_timeout) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body) == CURLE_OK &&
           set_optional_string(curl, CURLOPT_CAINFO, config->ca_file) &&
           set_optional_string(curl, CURLOPT_SSLCERT, config->client_cert) &&
           set_optional_string(curl, CURLOPT_SSLKEY, config->client_key);
}

static int copy_default_headers(curl_transport *transport, const skiff_curl_config *config) {
    for (size_t i = 0; i < config->default_header_count; i++) {
        if (!append_header(&transport->default_headers, config->default_headers[i].name,
                           config->default_headers[i].value)) {
            return 0;
        }
    }
    return 1;
}

skiff_err skiff_curl_transport_create(const skiff_curl_config *config, skiff_transport **out) {
    static const skiff_curl_config DEFAULTS = {0};
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (config == NULL) {
        config = &DEFAULTS;
    }
    if ((config->client_key != NULL && config->client_cert == NULL) ||
        config->connect_timeout_s < 0 || config->stall_timeout_s < 0 ||
        !skiff_http_headers_valid(config->default_headers, config->default_header_count)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    curl_transport *transport = calloc(1, sizeof *transport);
    if (transport == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    transport->base.ops = &CURL_TRANSPORT_OPS;
    transport->client_cert_configured = config->client_cert != NULL;
    transport->curl = curl_easy_init();
    if (transport->curl == NULL || !configure(transport, config) ||
        !copy_default_headers(transport, config)) {
        curl_destroy(&transport->base);
        return SKIFF_ERR_NO_MEMORY;
    }
    *out = &transport->base;
    return SKIFF_OK;
}
