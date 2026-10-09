#include "skiff/curl_transport.h"

#include <curl/curl.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/ssl.h>
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
/* An empty header removes curl's own: no "Expect: 100-continue" before a POST body. */
#define NO_EXPECT_HEADER "Expect:"
/* What a CURLOPT_RESOLVE entry adds to a host name: '-' or the ':' before the port, the port, and
 * the ':' before the address. */
#define RESOLVE_PORT_MAX 16

typedef struct curl_transport {
    skiff_transport base; /* first, so a skiff_transport * is a curl_transport * */
    CURL *curl;
    struct curl_slist *default_headers;
    int client_cert_configured;
    skiff_curl_resolve_fn resolve;
    void *resolve_ctx;
    /* The address the resolve hook gave for host:port, pinned in curl's name cache through
     * CURLOPT_RESOLVE (whose list curl reads at every request); port 0 when none is. */
    char resolved_host[SKIFF_CURL_HOST_MAX];
    long resolved_port;
    struct curl_slist *resolve_list;
} curl_transport;

/* State of one request, shared with curl's callbacks. */
typedef struct transfer {
    CURL *curl;
    const skiff_http_request *request;
    skiff_http_response *response;
    skiff_err callback_error;
} transfer;

skiff_err skiff_net_global_init(void) {
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? SKIFF_OK : SKIFF_ERR_NET_ENTROPY;
}

void skiff_net_global_cleanup(void) { curl_global_cleanup(); }

/* The connection's TLS version and cipher suite, once per request. curl exposes the session only
 * while a transfer runs, so this is read from the header callback. */
static void read_tls_session(const transfer *current) {
    skiff_http_response *response = current->response;
    const struct curl_tlssessioninfo *session = NULL;
    if (response->tls_cipher[0] != '\0' ||
        curl_easy_getinfo(current->curl, CURLINFO_TLS_SSL_PTR, &session) != CURLE_OK ||
        session == NULL || session->backend != CURLSSLBACKEND_MBEDTLS ||
        session->internals == NULL) {
        return;
    }
    const mbedtls_ssl_context *ssl = session->internals;
    const char *version = mbedtls_ssl_get_version(ssl);
    const char *cipher = mbedtls_ssl_get_ciphersuite(ssl);
    snprintf(response->tls_version, sizeof response->tls_version, "%s",
             version != NULL ? version : "");
    snprintf(response->tls_cipher, sizeof response->tls_cipher, "%s", cipher != NULL ? cipher : "");
}

/* curl's callback signature takes a non-const buffer. */
// cppcheck-suppress constParameterCallback
static size_t on_header(char *buffer, size_t size, size_t count, void *userdata) {
    transfer *current = userdata;
    const size_t length = size * count;
    read_tls_session(current);
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
            current->callback_error = err;
            return 0; /* anything short of length makes curl stop with CURLE_WRITE_ERROR */
        }
    }
    current->response->body_bytes += length;
    return length;
}

/* curl calls this about once a second, and after every chunk, whether or not bytes arrive. */
static int on_progress(void *userdata, curl_off_t download_total, curl_off_t downloaded,
                       curl_off_t upload_total, curl_off_t uploaded) {
    (void)download_total;
    (void)downloaded;
    (void)upload_total;
    (void)uploaded;
    transfer *current = userdata;
    if (current->request->should_stop == NULL) {
        return 0;
    }
    const skiff_err err = current->request->should_stop(current->request->stop_ctx);
    if (err != SKIFF_OK) {
        current->callback_error = err;
        return 1; /* non-zero makes curl stop with CURLE_ABORTED_BY_CALLBACK */
    }
    return 0;
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
    if (ok && request->content_type != NULL) {
        ok = append_header(&list, "Content-Type", request->content_type);
    }
    if (ok && request->method == SKIFF_HTTP_POST) {
        ok = append_line(&list, NO_EXPECT_HEADER);
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
        .callback_error = current->callback_error,
    };
    return skiff_net_error_from_curl(&failure);
}

/* The handle is reused, so every request sets its method: a GET after a POST must not resend the
 * body. curl reads the body from request->body during the transfer and copies nothing. */
static int set_method(CURL *curl, const skiff_http_request *request) {
    if (request->method == SKIFF_HTTP_GET) {
        return curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L) == CURLE_OK;
    }
    return curl_easy_setopt(curl, CURLOPT_POST, 1L) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request->body_size) ==
               CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body != NULL ? request->body : "") ==
               CURLE_OK;
}

/* The host and port (the scheme's default when the URL names none) of url. */
static skiff_err url_host(const char *url, char *host, size_t host_size, long *port) {
    CURLU *parsed = curl_url();
    if (parsed == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    char *name = NULL;
    char *number = NULL;
    skiff_err err = SKIFF_ERR_CONFIG_INVALID_VALUE;
    if (curl_url_set(parsed, CURLUPART_URL, url, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_HOST, &name, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_PORT, &number, CURLU_DEFAULT_PORT) == CURLUE_OK &&
        strlen(name) < host_size) {
        snprintf(host, host_size, "%s", name);
        *port = strtol(number, NULL, 10);
        err = SKIFF_OK;
    }
    curl_free(name);
    curl_free(number);
    curl_url_cleanup(parsed);
    return err;
}

/* Looks the request's host up through the resolve hook, unless the address is pinned already, and
 * pins the answer: curl then connects to it without a lookup of its own. An earlier entry for the
 * same host and port is dropped first ('-'), in case the address changed. */
static skiff_err pin_address(curl_transport *transport, const char *url) {
    if (transport->resolve == NULL) {
        return SKIFF_OK;
    }
    char host[SKIFF_CURL_HOST_MAX];
    long port = 0;
    skiff_err err = url_host(url, host, sizeof host, &port);
    if (err != SKIFF_OK) {
        return err;
    }
    if (port == transport->resolved_port && strcmp(host, transport->resolved_host) == 0) {
        return SKIFF_OK;
    }
    char address[SKIFF_CURL_ADDRESS_MAX];
    err = transport->resolve(transport->resolve_ctx, host, address, sizeof address);
    if (err != SKIFF_OK) {
        return err;
    }
    char drop[SKIFF_CURL_HOST_MAX + RESOLVE_PORT_MAX];
    char entry[SKIFF_CURL_HOST_MAX + RESOLVE_PORT_MAX + SKIFF_CURL_ADDRESS_MAX];
    snprintf(drop, sizeof drop, "-%s:%ld", host, port);
    snprintf(entry, sizeof entry, "%s:%ld:%s", host, port, address);
    struct curl_slist *list = NULL;
    if (!append_line(&list, drop) || !append_line(&list, entry) ||
        curl_easy_setopt(transport->curl, CURLOPT_RESOLVE, list) != CURLE_OK) {
        curl_slist_free_all(list);
        return SKIFF_ERR_NO_MEMORY;
    }
    curl_slist_free_all(transport->resolve_list);
    transport->resolve_list = list;
    snprintf(transport->resolved_host, sizeof transport->resolved_host, "%s", host);
    transport->resolved_port = port;
    return SKIFF_OK;
}

static skiff_err curl_perform(skiff_transport *base, const skiff_http_request *request,
                              skiff_http_response *response) {
    curl_transport *transport = (curl_transport *)base;
    transfer current = {transport->curl, request, response, SKIFF_OK};
    skiff_err err = pin_address(transport, request->url);
    if (err != SKIFF_OK) {
        return err;
    }
    struct curl_slist *headers = NULL;
    err = build_headers(transport, request, &headers);
    if (err != SKIFF_OK) {
        return err;
    }
    CURLcode code = CURLE_OK;
    if (!set_method(transport->curl, request) ||
        curl_easy_setopt(transport->curl, CURLOPT_URL, request->url) != CURLE_OK ||
        curl_easy_setopt(transport->curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(transport->curl, CURLOPT_HEADERDATA, &current) != CURLE_OK ||
        curl_easy_setopt(transport->curl, CURLOPT_WRITEDATA, &current) != CURLE_OK ||
        curl_easy_setopt(transport->curl, CURLOPT_XFERINFODATA, &current) != CURLE_OK) {
        code = CURLE_OUT_OF_MEMORY;
    } else {
        code = curl_easy_perform(transport->curl);
    }
    long connections = 0;
    curl_easy_getinfo(transport->curl, CURLINFO_NUM_CONNECTS, &connections);
    response->new_connections = connections;
    err = code == CURLE_OK ? SKIFF_OK : describe_failure(transport, request, &current, code);
    if (err != SKIFF_OK) {
        /* The server may have moved: the next request looks its name up again. */
        transport->resolved_port = 0;
    }
    /* The list and the callback state die with this call; curl must not keep pointers to them. */
    curl_easy_setopt(transport->curl, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(transport->curl, CURLOPT_HEADERDATA, NULL);
    curl_easy_setopt(transport->curl, CURLOPT_WRITEDATA, NULL);
    curl_easy_setopt(transport->curl, CURLOPT_XFERINFODATA, NULL);
    curl_easy_setopt(transport->curl, CURLOPT_POSTFIELDS, NULL);
    curl_slist_free_all(headers);
    return err;
}

static void curl_destroy(skiff_transport *base) {
    curl_transport *transport = (curl_transport *)base;
    curl_easy_cleanup(transport->curl);
    curl_slist_free_all(transport->default_headers);
    curl_slist_free_all(transport->resolve_list);
    free(transport);
}

static const skiff_transport_ops CURL_TRANSPORT_OPS = {curl_perform, curl_destroy};

static int set_optional_string(CURL *curl, CURLoption option, const char *value) {
    return value == NULL || curl_easy_setopt(curl, option, value) == CURLE_OK;
}

/*
 * Options that hold for every request; curl copies the strings. No cipher list: Mbed TLS's default
 * order offers ChaCha20-Poly1305 first, which a PSP decrypts eight times faster than AES-GCM, and
 * keeps every other suite for servers without it (pinned by tests/unit/test_host_tls.c).
 */
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
           curl_easy_setopt(curl, CURLOPT_TIMEOUT, config->total_timeout_s) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
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
        config->total_timeout_s < 0 ||
        !skiff_http_headers_valid(config->default_headers, config->default_header_count)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    curl_transport *transport = calloc(1, sizeof *transport);
    if (transport == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    transport->base.ops = &CURL_TRANSPORT_OPS;
    transport->client_cert_configured = config->client_cert != NULL;
    transport->resolve = config->resolve;
    transport->resolve_ctx = config->resolve_ctx;
    transport->curl = curl_easy_init();
    if (transport->curl == NULL || !configure(transport, config) ||
        !copy_default_headers(transport, config)) {
        curl_destroy(&transport->base);
        return SKIFF_ERR_NO_MEMORY;
    }
    *out = &transport->base;
    return SKIFF_OK;
}
