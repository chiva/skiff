#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/http.h"
#include "skiff/romm_pairing.h"
#include "skiff/version.h"

#include "romm_internal.h"

#define PATH_DEVICE_INIT "/api/auth/device/init"
#define PATH_DEVICE_TOKEN "/api/auth/device/token"
/* RomM's answers to a poll that is not yet a token (RFC 8628 §3.5). */
#define DETAIL_PENDING "authorization_pending"
#define DETAIL_SLOW_DOWN "slow_down"
#define DETAIL_DENIED "access_denied"
#define DETAIL_EXPIRED "expired_token"
#define HEX_DIGITS_LOWER "0123456789abcdef"
#define ASCII_DELETE 0x7F

enum {
    HTTP_STATUS_OK = 200,
    HTTP_STATUS_CREATED = 201,
    HTTP_STATUS_BAD_REQUEST = 400,
    HTTP_STATUS_TOO_MANY_REQUESTS = 429,
};

/*
 * What Skiff asks RomM for: browsing the platforms and ROMs and downloading them (roms.read covers
 * a file's content), nothing more, so the token on a removable Memory Stick can only read. Save
 * sync (Phase 5) needs devices.* and, in RomM 5.3.1, assets.* too: it will ask the player to pair
 * again for them. RomM's approval page lists the scopes and the player may grant fewer; Skiff
 * cannot work without any of these, so a token missing one is refused rather than saved.
 */
static const char *const REQUESTED_SCOPES[] = {
    "platforms.read",
    "roms.read",
};
enum { REQUESTED_SCOPE_COUNT = sizeof REQUESTED_SCOPES / sizeof REQUESTED_SCOPES[0] };

/* Printable ASCII without blanks: codes, tokens and ids. */
static int is_visible_ascii(const char *text) {
    for (const char *c = text; *c != '\0'; c++) {
        const unsigned char byte = (unsigned char)*c;
        if (byte <= (unsigned char)' ' || byte >= ASCII_DELETE) {
            return 0;
        }
    }
    return 1;
}

/* A non-empty string of visible ASCII copied whole into out; 0 otherwise. */
static int read_code(const cJSON *object, const char *name, char *out, size_t out_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return 0;
    }
    const size_t length = strlen(item->valuestring);
    if (length == 0 || length >= out_size || !is_visible_ascii(item->valuestring)) {
        return 0;
    }
    memcpy(out, item->valuestring, length + 1);
    return 1;
}

/* A whole number of seconds from 1 to max. */
static int read_seconds(const cJSON *object, const char *name, uint32_t max, uint32_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) {
        return 0;
    }
    const double value = item->valuedouble;
    if (!(value >= 1.0 && value <= (double)max) || value != (double)(uint32_t)value) {
        return 0;
    }
    *out = (uint32_t)value;
    return 1;
}

/* Wipes a secret so the compiler cannot drop the stores. */
static void wipe(char *secret, size_t size) {
    volatile char *bytes = secret;
    for (size_t i = 0; i < size; i++) {
        bytes[i] = '\0';
    }
}

skiff_err skiff_romm_device_identifier(const unsigned char *random, size_t size, char *out,
                                       size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (random == NULL || out == NULL || size != SKIFF_ROMM_DEVICE_IDENTIFIER_BYTES) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (out_size < 2 * size + 1) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    for (size_t i = 0; i < size; i++) {
        out[2 * i] = HEX_DIGITS_LOWER[random[i] >> 4];
        out[2 * i + 1] = HEX_DIGITS_LOWER[random[i] & 0x0F];
    }
    out[2 * size] = '\0';
    return SKIFF_OK;
}

/* The init request's body; NULL when out of memory. Free it with cJSON_free(). */
static char *init_body(const char *device_identifier) {
    cJSON *body = cJSON_CreateObject();
    cJSON *scopes = cJSON_CreateStringArray(REQUESTED_SCOPES, (int)REQUESTED_SCOPE_COUNT);
    char *printed = NULL;
    if (body != NULL && scopes != NULL &&
        cJSON_AddStringToObject(body, "client_device_identifier", device_identifier) != NULL &&
        cJSON_AddStringToObject(body, "name", SKIFF_ROMM_PAIRING_NAME) != NULL &&
        cJSON_AddStringToObject(body, "client", SKIFF_ROMM_PAIRING_CLIENT) != NULL &&
        cJSON_AddStringToObject(body, "platform", SKIFF_ROMM_PAIRING_PLATFORM) != NULL &&
        cJSON_AddStringToObject(body, "client_version", skiff_version_string()) != NULL &&
        cJSON_AddItemToObject(body, "requested_scopes", scopes)) {
        scopes = NULL; /* owned by body now */
        printed = cJSON_PrintUnformatted(body);
    }
    cJSON_Delete(scopes);
    cJSON_Delete(body);
    return printed;
}

/* base + a verification path ("/pair/device"), which must be a path on the server: a full URL in
 * its place would send the player to another site to enter the code. */
static int join_url(const skiff_romm_client *client, const char *path, char *out, size_t out_size) {
    if (path[0] != '/' || path[1] == '/' || !is_visible_ascii(path)) {
        return 0;
    }
    const size_t base = strlen(client->base_url);
    const size_t length = strlen(path);
    if (base + length >= out_size) {
        return 0;
    }
    memcpy(out, client->base_url, base);
    memcpy(out + base, path, length + 1);
    return 1;
}

static int fill_pairing(const skiff_romm_client *client, const cJSON *root,
                        skiff_romm_pairing *out) {
    char path[SKIFF_ROMM_URL_MAX];
    char path_complete[SKIFF_ROMM_URL_MAX];
    return cJSON_IsObject(root) &&
           read_code(root, "device_code", out->device_code, sizeof out->device_code) &&
           read_code(root, "user_code", out->user_code, sizeof out->user_code) &&
           read_code(root, "verification_path", path, sizeof path) &&
           read_code(root, "verification_path_complete", path_complete, sizeof path_complete) &&
           join_url(client, path, out->verification_url, sizeof out->verification_url) &&
           join_url(client, path_complete, out->verification_url_complete,
                    sizeof out->verification_url_complete) &&
           read_seconds(root, "expires_in", SKIFF_ROMM_PAIRING_EXPIRES_MAX_S, &out->expires_in_s) &&
           read_seconds(root, "interval", SKIFF_ROMM_PAIRING_INTERVAL_MAX_S, &out->interval_s);
}

static int client_ready(const skiff_romm_client *client) {
    return client != NULL && client->transport != NULL && client->base_url[0] != '\0';
}

/* The same rule config.ini puts on [auth] device_identifier. */
static int identifier_valid(const char *identifier) {
    const size_t length = strlen(identifier);
    return length > 0 && length < SKIFF_CONFIG_DEVICE_IDENTIFIER_MAX &&
           is_visible_ascii(identifier);
}

skiff_err skiff_romm_pairing_start(skiff_romm_client *client, const char *device_identifier,
                                   skiff_romm_pairing *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (!client_ready(client) || device_identifier == NULL || out == NULL ||
        !identifier_valid(device_identifier)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    char *body = init_body(device_identifier);
    if (body == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    long status = 0;
    cJSON *root = NULL;
    skiff_err err = skiff_romm_post_json(client, PATH_DEVICE_INIT, body, &status, &root);
    cJSON_free(body);
    if (err == SKIFF_OK) {
        err = skiff_http_status_error(status);
    }
    if (err == SKIFF_OK && status != HTTP_STATUS_CREATED && status != HTTP_STATUS_OK) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    if (err == SKIFF_OK && !fill_pairing(client, root, out)) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    skiff_romm_json_wipe_strings(root);
    cJSON_Delete(root);
    if (err != SKIFF_OK) {
        skiff_romm_pairing_clear(out, NULL);
        memset(out, 0, sizeof *out);
    }
    return err;
}

/* RFC 8628's slow_down: wait longer from now on, up to the most RomM may ask for. */
static void slow_down(skiff_romm_pairing *pairing, skiff_romm_pairing_result *result) {
    result->state = SKIFF_ROMM_PAIRING_SLOW_DOWN;
    if (pairing->interval_s <= SKIFF_ROMM_PAIRING_INTERVAL_MAX_S - SKIFF_ROMM_PAIRING_SLOW_DOWN_S) {
        pairing->interval_s += SKIFF_ROMM_PAIRING_SLOW_DOWN_S;
    }
}

/* The state a 400 names in its "detail", or the error that ends the pairing. */
static skiff_err read_refusal(const cJSON *root, skiff_romm_pairing *pairing,
                              skiff_romm_pairing_result *result) {
    const cJSON *detail = cJSON_GetObjectItemCaseSensitive(root, "detail");
    const char *text = cJSON_IsString(detail) ? detail->valuestring : NULL;
    if (text == NULL) {
        return SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    if (strcmp(text, DETAIL_PENDING) == 0) {
        result->state = SKIFF_ROMM_PAIRING_PENDING;
        return SKIFF_OK;
    }
    if (strcmp(text, DETAIL_SLOW_DOWN) == 0) {
        slow_down(pairing, result);
        return SKIFF_OK;
    }
    if (strcmp(text, DETAIL_DENIED) == 0) {
        return SKIFF_ERR_ROMM_PAIRING_DENIED;
    }
    if (strcmp(text, DETAIL_EXPIRED) == 0) {
        return SKIFF_ERR_ROMM_PAIRING_EXPIRED;
    }
    return SKIFF_ERR_ROMM_BAD_RESPONSE;
}

/* 1 if the scopes RomM granted (a JSON array of strings) hold every one Skiff requires. */
static int has_required_scopes(const cJSON *granted) {
    for (size_t i = 0; i < REQUESTED_SCOPE_COUNT; i++) {
        int found = 0;
        const cJSON *scope = NULL;
        cJSON_ArrayForEach(scope, granted) {
            found = found || (cJSON_IsString(scope) && scope->valuestring != NULL &&
                              strcmp(scope->valuestring, REQUESTED_SCOPES[i]) == 0);
        }
        if (!found) {
            return 0;
        }
    }
    return 1;
}

/* 1 if every scope RomM granted is one Skiff asked for: a token that can do more than Skiff
 * needs is not kept on a removable Memory Stick. */
static int only_requested_scopes(const cJSON *granted) {
    const cJSON *scope = NULL;
    cJSON_ArrayForEach(scope, granted) {
        int requested = 0;
        for (size_t i = 0; i < REQUESTED_SCOPE_COUNT; i++) {
            requested = requested || (cJSON_IsString(scope) && scope->valuestring != NULL &&
                                      strcmp(scope->valuestring, REQUESTED_SCOPES[i]) == 0);
        }
        if (!requested) {
            return 0;
        }
    }
    return 1;
}

/* The approved answer: the token and device id, provided RomM granted what Skiff needs. */
static skiff_err fill_token(const cJSON *root, skiff_romm_pairing_result *result) {
    const cJSON *scopes = cJSON_GetObjectItemCaseSensitive(root, "scopes");
    if (!cJSON_IsObject(root) || !cJSON_IsArray(scopes) || !only_requested_scopes(scopes) ||
        !read_code(root, "access_token", result->token, sizeof result->token) ||
        !read_code(root, "device_id", result->device_id, sizeof result->device_id)) {
        return SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    if (!has_required_scopes(scopes)) {
        return SKIFF_ERR_ROMM_PAIRING_SCOPES;
    }
    result->state = SKIFF_ROMM_PAIRING_APPROVED;
    return SKIFF_OK;
}

skiff_err skiff_romm_pairing_poll(skiff_romm_client *client, skiff_romm_pairing *pairing,
                                  skiff_romm_pairing_result *result) {
    if (result != NULL) {
        memset(result, 0, sizeof *result);
    }
    if (!client_ready(client) || pairing == NULL || result == NULL ||
        pairing->device_code[0] == '\0') {
        return SKIFF_ERR_INVALID_ARG;
    }
    cJSON *request = cJSON_CreateObject();
    char *body = NULL;
    if (request != NULL &&
        cJSON_AddStringToObject(request, "device_code", pairing->device_code) != NULL) {
        body = cJSON_PrintUnformatted(request);
    }
    skiff_romm_json_wipe_strings(request);
    cJSON_Delete(request);
    if (body == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    long status = 0;
    cJSON *root = NULL;
    skiff_err err = skiff_romm_post_json(client, PATH_DEVICE_TOKEN, body, &status, &root);
    /* The body held the device code. */
    wipe(body, strlen(body));
    cJSON_free(body);
    if (err == SKIFF_OK) {
        if (status == HTTP_STATUS_OK) {
            err = fill_token(root, result);
        } else if (status == HTTP_STATUS_BAD_REQUEST) {
            err = read_refusal(root, pairing, result);
        } else if (status == HTTP_STATUS_TOO_MANY_REQUESTS) {
            /* RomM limits polls per address and per code: treat it as RFC 8628's slow_down. */
            slow_down(pairing, result);
        } else {
            err = skiff_http_status_error(status);
            if (err == SKIFF_OK) {
                err = SKIFF_ERR_ROMM_BAD_RESPONSE;
            }
        }
    }
    /* The approved answer held the token; cJSON frees without wiping. */
    skiff_romm_json_wipe_strings(root);
    cJSON_Delete(root);
    if (err != SKIFF_OK) {
        skiff_romm_pairing_clear(NULL, result);
        memset(result, 0, sizeof *result);
    }
    return err;
}

skiff_err skiff_romm_pairing_config_text(const char *text, size_t length,
                                         const char *device_identifier,
                                         const skiff_romm_pairing_result *result, char *out,
                                         size_t out_size, size_t *out_length,
                                         skiff_config_issue *issue) {
    if (issue != NULL) {
        memset(issue, 0, sizeof *issue);
    }
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if ((text == NULL && length > 0) || device_identifier == NULL || result == NULL ||
        out == NULL || out_size == 0 || out_length == NULL ||
        result->state != SKIFF_ROMM_PAIRING_APPROVED) {
        return SKIFF_ERR_INVALID_ARG;
    }
    /* skiff_config_set() needs its output apart from its input: edit through a scratch copy. */
    char *scratch = malloc(out_size);
    if (scratch == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    size_t scratch_length = 0;
    skiff_err err = skiff_config_set(text, length, SKIFF_CONFIG_SECTION_AUTH,
                                     SKIFF_CONFIG_KEY_DEVICE_IDENTIFIER, device_identifier, scratch,
                                     out_size, &scratch_length, issue);
    if (err == SKIFF_OK) {
        err = skiff_config_set(scratch, scratch_length, SKIFF_CONFIG_SECTION_AUTH,
                               SKIFF_CONFIG_KEY_DEVICE_ID, result->device_id, out, out_size,
                               out_length, issue);
    }
    if (err == SKIFF_OK) {
        err = skiff_config_set(out, *out_length, SKIFF_CONFIG_SECTION_AUTH, SKIFF_CONFIG_KEY_TOKEN,
                               result->token, scratch, out_size, &scratch_length, issue);
    }
    if (err == SKIFF_OK) {
        memcpy(out, scratch, scratch_length + 1);
        *out_length = scratch_length;
    } else {
        out[0] = '\0';
        *out_length = 0;
    }
    /* The scratch copy held the token. */
    wipe(scratch, out_size);
    free(scratch);
    return err;
}

void skiff_romm_pairing_clear(skiff_romm_pairing *pairing, skiff_romm_pairing_result *result) {
    if (pairing != NULL) {
        wipe(pairing->device_code, sizeof pairing->device_code);
    }
    if (result != NULL) {
        wipe(result->token, sizeof result->token);
    }
}
