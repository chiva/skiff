#ifndef SKIFF_ROMM_INTERNAL_H
#define SKIFF_ROMM_INTERNAL_H

/*
 * What src/romm/ files share beyond skiff/romm.h: requests whose JSON body is read whatever the
 * status, for the pairing calls (src/romm/pairing.c), whose answers come as 400s with a JSON
 * "detail".
 */

#include <cjson/cJSON.h>

#include "skiff/romm.h"

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
