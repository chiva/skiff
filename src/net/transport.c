#include "skiff/transport.h"

#include <string.h>

static const char *const ALLOWED_SCHEMES[] = {"http://", "https://"};

static int has_line_break(const char *text) { return strpbrk(text, "\r\n") != NULL; }

static int starts_with_ignoring_case(const char *text, const char *prefix) {
    for (; *prefix != '\0'; text++, prefix++) {
        char lower = *text;
        if (lower >= 'A' && lower <= 'Z') {
            lower = (char)(lower - 'A' + 'a');
        }
        if (lower != *prefix) {
            return 0;
        }
    }
    return 1;
}

/* Only an explicit http:// or https://: curl guesses a scheme for anything else, falling back to
 * plain HTTP, which would send the token and custom headers in the clear. */
static int has_allowed_scheme(const char *url) {
    for (size_t i = 0; i < sizeof ALLOWED_SCHEMES / sizeof ALLOWED_SCHEMES[0]; i++) {
        if (starts_with_ignoring_case(url, ALLOWED_SCHEMES[i])) {
            return 1;
        }
    }
    return 0;
}

int skiff_http_headers_valid(const skiff_http_header *headers, size_t count) {
    if (count > 0 && headers == NULL) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        const char *name = headers[i].name;
        const char *value = headers[i].value;
        if (name == NULL || name[0] == '\0' || value == NULL || strchr(name, ':') != NULL ||
            has_line_break(name) || has_line_break(value)) {
            return 0;
        }
    }
    return 1;
}

skiff_err skiff_transport_perform(skiff_transport *transport, const skiff_http_request *request,
                                  skiff_http_response *response) {
    if (response == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_http_response_reset(response);
    if (transport == NULL || transport->ops == NULL || transport->ops->perform == NULL ||
        request == NULL || request->url == NULL || request->url[0] == '\0' ||
        !skiff_http_headers_valid(request->headers, request->header_count) ||
        (request->if_range != NULL && (!request->has_range || has_line_break(request->if_range)))) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (!has_allowed_scheme(request->url)) {
        return SKIFF_ERR_CONFIG_INVALID_VALUE;
    }
    return transport->ops->perform(transport, request, response);
}

void skiff_transport_destroy(skiff_transport *transport) {
    if (transport != NULL && transport->ops != NULL && transport->ops->destroy != NULL) {
        transport->ops->destroy(transport);
    }
}
