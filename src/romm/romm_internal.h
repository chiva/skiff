#ifndef SKIFF_ROMM_INTERNAL_H
#define SKIFF_ROMM_INTERNAL_H

/*
 * What src/romm/ files share beyond skiff/romm.h: requests whose JSON body is read whatever the
 * status, for the pairing calls (src/romm/pairing.c), whose answers come as 400s with a JSON
 * "detail"; requests with the token and any body, for the save sync calls (src/romm/saves.c);
 * reads that tell a body too large for the caps from a broken one (src/romm/collections.c); and the
 * checks every response and field gets.
 */

#include <cjson/cJSON.h>

#include "skiff/romm.h"
#include "skiff/transport.h"

/* The longest path and query a request sends: a save's upload names its emulator, slot and device,
 * any byte of which may be percent-encoded. */
#define SKIFF_ROMM_REQUEST_PATH_MAX 1536
#define SKIFF_ROMM_REQUEST_URL_MAX (SKIFF_CONFIG_URL_MAX + SKIFF_ROMM_REQUEST_PATH_MAX)

/* What a request sends after its headers: JSON text (labelled application/json), or size bytes
 * streamed from read (skiff_http_read_fn) under content_type. */
typedef struct skiff_romm_body {
    const char *json;
    skiff_http_read_fn read;
    void *read_ctx;
    size_t size;
    const char *content_type;
} skiff_romm_body;

/*
 * Sends method to the client's base URL + path with the token and body (NULL for none), and parses
 * a successful (2xx) response's body as JSON into *out: NULL when it is empty or not JSON (free it
 * with cJSON_Delete()); any other status's body is not kept. *status is the HTTP status, 0 when
 * none arrived: the caller judges it. Returns the transport's error (a body reader's included),
 * SKIFF_ERR_ROMM_BAD_RESPONSE for a body over SKIFF_ROMM_BODY_MAX, SKIFF_ERR_NO_MEMORY, or
 * SKIFF_ERR_INVALID_ARG for a NULL argument or a path longer than SKIFF_ROMM_REQUEST_PATH_MAX.
 */
skiff_err skiff_romm_request_json(skiff_romm_client *client, skiff_http_method method,
                                  const char *path, const skiff_romm_body *body, long *status,
                                  cJSON **out);

/* A body that passes the checks every response gets (skiff/romm.h) as a tree, NULL otherwise; free
 * it with cJSON_Delete(). */
cJSON *skiff_romm_parse_json(const char *json, size_t length);

/* The same, with *too_large set when the body was refused for its size (over SKIFF_ROMM_BODY_MAX)
 * or its number of values (over SKIFF_ROMM_JSON_NODES_MAX) rather than for what it holds, and
 * cleared otherwise. */
cJSON *skiff_romm_parse_json_capped(const char *json, size_t length, int *too_large);

/*
 * GETs the client's base URL + path with the token and parses the body as JSON into *out (free it
 * with cJSON_Delete()), with skiff/romm.h's errors: the transport's, the RomM error for an HTTP
 * status, SKIFF_ERR_ROMM_BAD_RESPONSE for a body that is not JSON or is refused by the caps, and
 * SKIFF_ERR_INVALID_ARG for a NULL argument or a path longer than SKIFF_ROMM_REQUEST_PATH_MAX. On a
 * refusal by the caps, *too_large is 1 (0 otherwise), so the caller can name it.
 */
skiff_err skiff_romm_get_json(skiff_romm_client *client, const char *path, cJSON **out,
                              int *too_large);

/* Field readers under skiff/romm.h's rules; each returns 0 for a field that breaks them. A whole,
 * non-negative number below 2^53: */
int skiff_romm_read_count(const cJSON *object, const char *name, uint64_t *out);
/* A string copied whole: 0 when absent, not a string, too long for out or holding a control
 * character. */
int skiff_romm_read_text(const cJSON *object, const char *name, char *out, size_t out_size);
/* The same, but null or absent leaves out empty. */
int skiff_romm_read_optional_text(const cJSON *object, const char *name, char *out,
                                  size_t out_size);
/* A name that is only shown, in display form (skiff_romm_name_status: control characters replaced,
 * a long one cut); null or absent leaves out empty, and only a value that is not a string is 0. */
int skiff_romm_read_display_name(const cJSON *object, const char *name, char *out, size_t out_size);

/* 1 if text holds no control character (C0, DEL or C1). */
int skiff_romm_is_printable_text(const char *text);

/* Appends text to the string in out with every byte but letters, digits and "-._~"
 * percent-encoded. SKIFF_ERR_BUFFER_TOO_SMALL (out emptied) when it does not fit. */
skiff_err skiff_romm_append_encoded(char *out, size_t out_size, const char *text);

/*
 * POSTs json_body (sent as application/json, without the token: RomM's pairing endpoints are open)
 * to the client's base URL + path, and parses the response body, of any status, as JSON into *out:
 * NULL when it is not JSON (a proxy's page, an empty body), the same checks as every RomM response
 * (SKIFF_ROMM_BODY_MAX, SKIFF_ROMM_JSON_NODES_MAX). *status is the HTTP status, 0 when none
 * arrived. Returns the transport's error (1xx), SKIFF_ERR_ROMM_BAD_RESPONSE for a body over the
 * cap, SKIFF_ERR_NO_MEMORY, or SKIFF_ERR_INVALID_ARG for a NULL argument; the caller judges the
 * status. The raw body is wiped before its memory is freed, since pairing's answers hold
 * credentials, and so is a body refused after parsing (junk after the JSON); the caller wipes *out
 * with skiff_romm_json_wipe_strings() before cJSON_Delete().
 */
skiff_err skiff_romm_post_json(skiff_romm_client *client, const char *path, const char *json_body,
                               long *status, cJSON **out);

/* Wipes every string value in item, its siblings and everything under them (not the member names):
 * cJSON frees a credential without clearing it. Call it before cJSON_Delete() on a tree from
 * skiff_romm_post_json(). Does nothing for NULL. */
void skiff_romm_json_wipe_strings(cJSON *item);

#endif
