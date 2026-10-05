#include "skiff/transport.h"

#include <string.h>

static int has_line_break(const char *text) { return strpbrk(text, "\r\n") != NULL; }

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
    return transport->ops->perform(transport, request, response);
}

void skiff_transport_destroy(skiff_transport *transport) {
    if (transport != NULL && transport->ops != NULL && transport->ops->destroy != NULL) {
        transport->ops->destroy(transport);
    }
}
