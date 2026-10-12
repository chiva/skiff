#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

#include "skiff/http.h"
#include "skiff/romm_installs.h"

#include "romm_internal.h"

#define PATH_DEVICES "/api/devices/"
#define PATH_CLAIM "/installs/claim"
#define PATH_INSTALLS "/installs/"
#define FIELD_CAPABILITIES "capabilities"
#define FIELD_REMOTE_INSTALL "remote_install"
#define FIELD_STATUS "status"
#define FIELD_REASON "reason"
#define FIELD_ID "id"
#define FIELD_DEVICE_ID "device_id"
#define FIELD_ROM_ID "rom_id"
#define FIELD_FILE_IDS "file_ids"
/* The only status a claim returns. */
#define STATUS_TAKEN "taken"
#define HEX_DIGITS_LOWER "0123456789abcdef"
#define UUID_HYPHEN '-'
/* UTF-8 continuation bytes are 10xxxxxx. */
#define UTF8_CONTINUATION_MASK 0xC0U
#define UTF8_CONTINUATION 0x80U
/* 2^53: ids arrive as JSON numbers, which cJSON reads as doubles. */
#define JSON_INTEGER_LIMIT 9007199254740992.0

enum {
    HTTP_STATUS_FORBIDDEN = 403,
    HTTP_STATUS_CONFLICT = 409,
};

/* Where str(uuid4()) puts its hyphens. */
static const size_t UUID_HYPHENS[] = {8, 13, 18, 23};

/* What RomM calls each status a device reports. */
static const char *const STATUS_NAMES[] = {
    [SKIFF_ROMM_INSTALL_DONE] = "done",
    [SKIFF_ROMM_INSTALL_ALREADY_INSTALLED] = "already_installed",
    [SKIFF_ROMM_INSTALL_FAILED] = "failed",
};

static int client_ready(const skiff_romm_client *client) {
    return client != NULL && client->transport != NULL && client->base_url[0] != '\0';
}

static int is_uuid_hyphen_at(size_t index) {
    for (size_t i = 0; i < sizeof UUID_HYPHENS / sizeof UUID_HYPHENS[0]; i++) {
        if (UUID_HYPHENS[i] == index) {
            return 1;
        }
    }
    return 0;
}

int skiff_romm_install_request_id_valid(const char *id) {
    if (id == NULL || strlen(id) != SKIFF_ROMM_INSTALL_REQUEST_ID_LENGTH) {
        return 0;
    }
    for (size_t i = 0; i < SKIFF_ROMM_INSTALL_REQUEST_ID_LENGTH; i++) {
        const int hyphen = is_uuid_hyphen_at(i);
        if ((hyphen && id[i] != UUID_HYPHEN) ||
            (!hyphen && (id[i] == '\0' || strchr(HEX_DIGITS_LOWER, id[i]) == NULL))) {
            return 0;
        }
    }
    return 1;
}

static int starts_character(char c) {
    return ((unsigned char)c & UTF8_CONTINUATION_MASK) != UTF8_CONTINUATION;
}

size_t skiff_romm_install_reason_length(const char *reason) {
    if (reason == NULL) {
        return 0;
    }
    size_t length = strlen(reason);
    if (length <= SKIFF_ROMM_INSTALL_REASON_MAX_BYTES) {
        return length;
    }
    /* reason[length] is the first byte left out: it must start a character. */
    length = SKIFF_ROMM_INSTALL_REASON_MAX_BYTES;
    while (length > 0 && !starts_character(reason[length])) {
        length--;
    }
    return length;
}

/* ---- Paths ---- */

/* "/api/devices/<device_id><suffix>", the id percent-encoded; 0 when device_id is not one or the
 * path does not fit. */
static int device_path(const char *device_id, const char *suffix, char *out, size_t out_size) {
    if (!skiff_romm_device_id_valid(device_id)) {
        return 0;
    }
    snprintf(out, out_size, "%s", PATH_DEVICES);
    if (skiff_romm_append_encoded(out, out_size, device_id) != SKIFF_OK) {
        return 0;
    }
    const size_t used = strlen(out);
    const int written = snprintf(out + used, out_size - used, "%s", suffix);
    return written >= 0 && (size_t)written < out_size - used;
}

/* ---- Statuses ---- */

static skiff_err status_error(long status) {
    return status == HTTP_STATUS_FORBIDDEN ? SKIFF_ERR_ROMM_SENDS_FORBIDDEN
                                           : skiff_http_status_error(status);
}

static skiff_err claim_status_error(long status) {
    return status == HTTP_STATUS_CONFLICT ? SKIFF_ERR_ROMM_SERVER : status_error(status);
}

static skiff_err report_status_error(long status) {
    return status == HTTP_STATUS_CONFLICT ? SKIFF_ERR_ROMM_NOT_FOUND : status_error(status);
}

/* Sends method to path with json (freed here with cJSON_free(); NULL means out of memory, unless
 * no_body) and maps the status with map; a 2xx answer's JSON goes to *out (free it with
 * cJSON_Delete()), and a 2xx that is not JSON (a proxy's page) is SKIFF_ERR_ROMM_BAD_RESPONSE: it
 * confirms nothing. */
static skiff_err exchange(skiff_romm_client *client, skiff_http_method method, const char *path,
                          char *json, int no_body, skiff_err (*map)(long), cJSON **out) {
    *out = NULL;
    if (json == NULL && !no_body) {
        return SKIFF_ERR_NO_MEMORY;
    }
    const skiff_romm_body body = {.json = json};
    long status = 0;
    cJSON *root = NULL;
    skiff_err err =
        skiff_romm_request_json(client, method, path, no_body ? NULL : &body, &status, &root);
    cJSON_free(json);
    if (err == SKIFF_OK) {
        err = map(status);
    }
    if (err == SKIFF_OK && root == NULL) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    if (err == SKIFF_OK) {
        *out = root;
        root = NULL;
    }
    cJSON_Delete(root);
    return err;
}

/* ---- Register ---- */

static char *capabilities_body(int on) {
    cJSON *body = cJSON_CreateObject();
    cJSON *capabilities = cJSON_AddObjectToObject(body, FIELD_CAPABILITIES);
    char *printed = NULL;
    if (capabilities != NULL &&
        cJSON_AddBoolToObject(capabilities, FIELD_REMOTE_INSTALL, on ? 1 : 0) != NULL) {
        printed = cJSON_PrintUnformatted(body);
    }
    cJSON_Delete(body);
    return printed;
}

/* RomM's answer to a register: this device, holding remote_install as it was sent. */
static int registered(const cJSON *root, const char *device_id, int on) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, FIELD_ID);
    const cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(root, FIELD_CAPABILITIES);
    const cJSON *remote = cJSON_GetObjectItemCaseSensitive(capabilities, FIELD_REMOTE_INSTALL);
    return cJSON_IsObject(root) && cJSON_IsString(id) && id->valuestring != NULL &&
           strcmp(id->valuestring, device_id) == 0 && cJSON_IsObject(capabilities) &&
           cJSON_IsBool(remote) && cJSON_IsTrue(remote) == (on ? 1 : 0);
}

skiff_err skiff_romm_installs_register(skiff_romm_client *client, const char *device_id, int on) {
    char path[SKIFF_ROMM_URL_MAX];
    if (!client_ready(client) || !device_path(device_id, "", path, sizeof path)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    cJSON *root = NULL;
    skiff_err err =
        exchange(client, SKIFF_HTTP_PUT, path, capabilities_body(on), 0, status_error, &root);
    if (err == SKIFF_OK && !registered(root, device_id, on)) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    cJSON_Delete(root);
    return err;
}

/* ---- Claim ---- */

/* An id RomM gives out: from 1, below 2^53. */
static int read_id(const cJSON *object, const char *name, uint64_t *out) {
    return skiff_romm_read_count(object, name, out) && *out > 0;
}

/* file_ids: an array, its first SKIFF_ROMM_INSTALL_FILE_IDS_READ_MAX entries ids, counted. */
static int read_file_ids(const cJSON *item, size_t *count) {
    const cJSON *ids = cJSON_GetObjectItemCaseSensitive(item, FIELD_FILE_IDS);
    if (!cJSON_IsArray(ids)) {
        return 0;
    }
    size_t index = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, ids) {
        if (index < SKIFF_ROMM_INSTALL_FILE_IDS_READ_MAX) {
            if (!cJSON_IsNumber(entry) ||
                !(entry->valuedouble >= 1.0 && entry->valuedouble < JSON_INTEGER_LIMIT) ||
                entry->valuedouble != (double)(uint64_t)entry->valuedouble) {
                return 0;
            }
        }
        index++;
    }
    *count = index;
    return 1;
}

static int is_string(const cJSON *object, const char *name, const char *expected) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != NULL &&
           strcmp(item->valuestring, expected) == 0;
}

/* One claimed request: 1 when it parses (*keep says whether its id is one Skiff uses), 0 for one
 * RomM would not send. */
static int parse_request(const cJSON *item, const char *device_id, skiff_romm_install_request *out,
                         int *keep) {
    memset(out, 0, sizeof *out);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, FIELD_ID);
    if (!cJSON_IsObject(item) || !cJSON_IsString(id) || id->valuestring == NULL ||
        !is_string(item, FIELD_STATUS, STATUS_TAKEN) ||
        !is_string(item, FIELD_DEVICE_ID, device_id) ||
        !read_id(item, FIELD_ROM_ID, &out->rom_id) || !read_file_ids(item, &out->file_count)) {
        return 0;
    }
    *keep = skiff_romm_install_request_id_valid(id->valuestring);
    if (*keep) {
        memcpy(out->id, id->valuestring, SKIFF_ROMM_INSTALL_REQUEST_ID_MAX);
    }
    return 1;
}

static int fill_claim(const cJSON *root, const char *device_id, skiff_romm_install_claim *out) {
    if (!cJSON_IsArray(root)) {
        return 0;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        skiff_romm_install_request request;
        int keep = 0;
        if (!parse_request(item, device_id, &request, &keep)) {
            return 0;
        }
        out->total++;
        if (!keep) {
            out->dropped++;
        } else if (out->count < SKIFF_ROMM_INSTALL_CLAIM_MAX) {
            out->requests[out->count++] = request;
        }
    }
    return 1;
}

skiff_err skiff_romm_parse_install_claim(const char *json, size_t length, const char *device_id,
                                         skiff_romm_install_claim *out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (!skiff_romm_device_id_valid(device_id)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    cJSON *root = skiff_romm_parse_json(json, length);
    const int ok = root != NULL && fill_claim(root, device_id, out);
    cJSON_Delete(root);
    if (!ok) {
        memset(out, 0, sizeof *out);
        return SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    return SKIFF_OK;
}

skiff_err skiff_romm_installs_claim(skiff_romm_client *client, const char *device_id,
                                    skiff_romm_install_claim *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    char path[SKIFF_ROMM_URL_MAX];
    if (!client_ready(client) || out == NULL ||
        !device_path(device_id, PATH_CLAIM, path, sizeof path)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    cJSON *root = NULL;
    skiff_err err = exchange(client, SKIFF_HTTP_POST, path, NULL, 1, claim_status_error, &root);
    if (err == SKIFF_OK && !fill_claim(root, device_id, out)) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    cJSON_Delete(root);
    if (err != SKIFF_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}

/* ---- Report ---- */

static int status_valid(skiff_romm_install_status status) {
    return status == SKIFF_ROMM_INSTALL_DONE || status == SKIFF_ROMM_INSTALL_ALREADY_INSTALLED ||
           status == SKIFF_ROMM_INSTALL_FAILED;
}

/* {"status":...,"reason":...}, the reason cut and left out when empty; NULL when out of memory. */
static char *report_body(skiff_romm_install_status status, const char *reason) {
    char cut[SKIFF_ROMM_INSTALL_REASON_MAX_BYTES + 1];
    const size_t length = skiff_romm_install_reason_length(reason);
    if (length > 0) {
        memcpy(cut, reason, length);
    }
    cut[length] = '\0';
    cJSON *body = cJSON_CreateObject();
    char *printed = NULL;
    if (cJSON_AddStringToObject(body, FIELD_STATUS, STATUS_NAMES[status]) != NULL &&
        (length == 0 || cJSON_AddStringToObject(body, FIELD_REASON, cut) != NULL)) {
        printed = cJSON_PrintUnformatted(body);
    }
    cJSON_Delete(body);
    return printed;
}

skiff_err skiff_romm_installs_report(skiff_romm_client *client, const char *device_id,
                                     const char *request_id, skiff_romm_install_status status,
                                     const char *reason) {
    char path[SKIFF_ROMM_URL_MAX];
    char suffix[sizeof PATH_INSTALLS + SKIFF_ROMM_INSTALL_REQUEST_ID_LENGTH];
    if (!client_ready(client) || !skiff_romm_install_request_id_valid(request_id) ||
        !status_valid(status) || (reason != NULL && !skiff_romm_is_printable_text(reason))) {
        return SKIFF_ERR_INVALID_ARG;
    }
    snprintf(suffix, sizeof suffix, PATH_INSTALLS "%s", request_id);
    if (!device_path(device_id, suffix, path, sizeof path)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    cJSON *root = NULL;
    skiff_err err = exchange(client, SKIFF_HTTP_PUT, path, report_body(status, reason), 0,
                             report_status_error, &root);
    /* RomM answers with the request as it ended. */
    if (err == SKIFF_OK && (!cJSON_IsObject(root) || !is_string(root, FIELD_ID, request_id) ||
                            !is_string(root, FIELD_STATUS, STATUS_NAMES[status]))) {
        err = SKIFF_ERR_ROMM_BAD_RESPONSE;
    }
    cJSON_Delete(root);
    return err;
}
