/*
 * Pairing with RomM (skiff/romm_pairing.h) against the fake transport replaying RomM 5.3.1's
 * answers to the device-code flow (tests/fixtures/romm/device-*.http, recorded by
 * tests/integration/record-pairing-fixtures.sh with every code, token and id made synthetic): the
 * start, pending, slow_down, the approved token, a refused and an expired pairing, and every way an
 * answer can be wrong. Then config.ini's text with the pairing saved in it.
 */
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/romm_pairing.h"
#include "skiff/version.h"

#include "fake_transport.h"
#include "romm_internal.h"
#include "unity.h"

#define BASE_URL "https://romm.test:8443"
#define PATH_INIT "/api/auth/device/init"
#define PATH_TOKEN "/api/auth/device/token"
#define IDENTIFIER "00112233445566778899aabbccddeeff"
/* What the recorded answers hold in place of RomM's codes, token and device id. */
#define USER_CODE "SKIFF234"
#define DEVICE_CODE "skiff-device-code-skiff-device-code-skiff-device-code-skiff-devi"
#define TOKEN "rmm_synthetic_pairing_token_rmm_synthetic_pairing_token_rmm_syntheti"
#define DEVICE_ID "00000000-0000-4000-8000-000000000001"
#define RECORDED_INTERVAL_S 5U
#define RECORDED_EXPIRES_S 600U
#define JSON_201 "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n\r\n"
#define JSON_400 "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n"
#define JSON_200 "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
#define RAW_MAX 1024
/* The scopes RomM grants when the approver keeps what Skiff asked for. */
#define SCOPES "\"scopes\":[\"platforms.read\",\"roms.read\"]"
#define TEXT_BUFFER (SKIFF_CONFIG_TEXT_MAX + 1)

static fake_transport fake;
static skiff_romm_client client;
static skiff_romm_pairing pairing;
static skiff_romm_pairing_result result;

void setUp(void) {
    fake_transport_init(&fake);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_client_init(&client, &fake.base, BASE_URL, NULL));
    memset(&pairing, 0xAB, sizeof pairing);
    memset(&result, 0xAB, sizeof result);
}

void tearDown(void) { skiff_transport_destroy(&fake.base); }

/* Serves path from fixture to POSTs only, for uses requests (0: every request). */
static void serve(const char *path, const char *fixture, int uses) {
    fake_route *route = fake_transport_add_fixture(&fake, path, fixture);
    TEST_ASSERT_NOT_NULL_MESSAGE(route, fixture);
    route->match_method = 1;
    route->method = SKIFF_HTTP_POST;
    route->max_uses = uses;
}

static void serve_raw(const char *path, const char *raw) {
    fake_route *route = fake_transport_add_raw(&fake, path, raw, strlen(raw));
    TEST_ASSERT_NOT_NULL(route);
}

static skiff_err start(void) {
    const skiff_err err = skiff_romm_pairing_start(&client, IDENTIFIER, &pairing);
    TEST_PRINTF("start -> %s: code '%s' at %s, every %u s for %u s", skiff_err_name(err),
                pairing.user_code, pairing.verification_url, (unsigned)pairing.interval_s,
                (unsigned)pairing.expires_in_s);
    return err;
}

static skiff_err poll(void) {
    const skiff_err err = skiff_romm_pairing_poll(&client, &pairing, &result);
    TEST_PRINTF("poll -> %s, state %d, interval now %u s", skiff_err_name(err), (int)result.state,
                (unsigned)pairing.interval_s);
    return err;
}

static void start_recorded(void) {
    serve(PATH_INIT, "romm/device-init.http", 0);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, start());
}

/* ---- The identifier ---- */

static void test_the_identifier_is_the_random_bytes_in_hex(void) {
    static const unsigned char RANDOM[SKIFF_ROMM_DEVICE_IDENTIFIER_BYTES] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    char out[2 * SKIFF_ROMM_DEVICE_IDENTIFIER_BYTES + 1];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_device_identifier(RANDOM, sizeof RANDOM, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING(IDENTIFIER, out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_romm_device_identifier(RANDOM, sizeof RANDOM, out, sizeof out - 1));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_device_identifier(RANDOM, sizeof RANDOM - 1, out, sizeof out));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_device_identifier(NULL, sizeof RANDOM, out, sizeof out));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_device_identifier(RANDOM, sizeof RANDOM, NULL, 0));
}

/* ---- Starting ---- */

static void test_start_asks_for_skiffs_scopes_and_shows_the_code(void) {
    start_recorded();
    TEST_ASSERT_EQUAL_STRING(USER_CODE, pairing.user_code);
    TEST_ASSERT_EQUAL_STRING(DEVICE_CODE, pairing.device_code);
    TEST_ASSERT_EQUAL_STRING(BASE_URL "/pair/device", pairing.verification_url);
    TEST_ASSERT_EQUAL_STRING(BASE_URL "/pair/device?user_code=" USER_CODE,
                             pairing.verification_url_complete);
    TEST_ASSERT_EQUAL_UINT32(RECORDED_INTERVAL_S, pairing.interval_s);
    TEST_ASSERT_EQUAL_UINT32(RECORDED_EXPIRES_S, pairing.expires_in_s);

    const fake_request *sent = &fake.log[0];
    TEST_PRINTF("sent: %s", sent->body);
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_POST, sent->method);
    TEST_ASSERT_EQUAL_STRING("application/json", sent->content_type);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", sent->headers, "pairing sends no token");
    char expected[RAW_MAX];
    snprintf(expected, sizeof expected,
             "{\"client_device_identifier\":\"" IDENTIFIER "\",\"name\":\"Skiff on PSP\","
             "\"client\":\"skiff\",\"platform\":\"psp\",\"client_version\":\"%s\","
             "\"requested_scopes\":[\"platforms.read\",\"roms.read\"]}",
             skiff_version_string());
    TEST_ASSERT_EQUAL_STRING(expected, sent->body);
}

static void test_start_refuses_a_bad_identifier_before_sending(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_pairing_start(&client, "", &pairing));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_pairing_start(&client, "has blank", &pairing));
    char long_identifier[SKIFF_CONFIG_DEVICE_IDENTIFIER_MAX + 1];
    memset(long_identifier, 'a', sizeof long_identifier - 1);
    long_identifier[sizeof long_identifier - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_pairing_start(&client, long_identifier, &pairing));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_pairing_start(&client, NULL, &pairing));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_pairing_start(NULL, IDENTIFIER, &pairing));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_pairing_start(&client, IDENTIFIER, NULL));
    TEST_ASSERT_EQUAL_size_t(0, fake.request_count);
    TEST_ASSERT_EQUAL_STRING("", pairing.device_code);
}

typedef struct bad_start {
    const char *why;
    const char *raw;
    skiff_err expected;
} bad_start;

static void test_start_refuses_what_is_not_a_pairing(void) {
    static const bad_start CASES[] = {
        {"a proxy's login page", "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<html>",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"RomM's validation error", "HTTP/1.1 422 Unprocessable\r\n\r\n{\"detail\":[]}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"a server error", "HTTP/1.1 500 Internal\r\n\r\n{}", SKIFF_ERR_ROMM_SERVER},
        {"no device code",
         JSON_201
         "{\"user_code\":\"A\",\"verification_path\":\"/p\",\"verification_path_complete\":"
         "\"/p\",\"expires_in\":600,\"interval\":5}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"a user code with a blank",
         JSON_201 "{\"device_code\":\"d\",\"user_code\":\"A B\",\"verification_path\":\"/p\","
                  "\"verification_path_complete\":\"/p\",\"expires_in\":600,\"interval\":5}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"a verification page on another site",
         JSON_201 "{\"device_code\":\"d\",\"user_code\":\"A\",\"verification_path\":"
                  "\"https://evil.test/pair\",\"verification_path_complete\":\"/p\","
                  "\"expires_in\":600,\"interval\":5}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"a scheme-relative verification path",
         JSON_201 "{\"device_code\":\"d\",\"user_code\":\"A\",\"verification_path\":"
                  "\"//evil.test/pair\",\"verification_path_complete\":\"/p\","
                  "\"expires_in\":600,\"interval\":5}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"no interval",
         JSON_201 "{\"device_code\":\"d\",\"user_code\":\"A\",\"verification_path\":\"/p\","
                  "\"verification_path_complete\":\"/p\",\"expires_in\":600}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"an interval of zero",
         JSON_201 "{\"device_code\":\"d\",\"user_code\":\"A\",\"verification_path\":\"/p\","
                  "\"verification_path_complete\":\"/p\",\"expires_in\":600,\"interval\":0}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"an interval past the limit",
         JSON_201 "{\"device_code\":\"d\",\"user_code\":\"A\",\"verification_path\":\"/p\","
                  "\"verification_path_complete\":\"/p\",\"expires_in\":600,\"interval\":301}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"a fractional lifetime",
         JSON_201 "{\"device_code\":\"d\",\"user_code\":\"A\",\"verification_path\":\"/p\","
                  "\"verification_path_complete\":\"/p\",\"expires_in\":0.5,\"interval\":5}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        skiff_transport_destroy(&fake.base);
        fake_transport_init(&fake);
        serve_raw(PATH_INIT, CASES[i].raw);
        TEST_PRINTF("%s", CASES[i].why);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(skiff_err_name(CASES[i].expected), skiff_err_name(start()),
                                         CASES[i].why);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", pairing.device_code, CASES[i].why);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", pairing.user_code, CASES[i].why);
    }
}

static void test_start_reports_the_network(void) {
    fake_route *route = fake_transport_add_fixture(&fake, PATH_INIT, "romm/device-init.http");
    TEST_ASSERT_NOT_NULL(route);
    route->fail_before_response = SKIFF_ERR_NET_CONNECT;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECT, start());
}

/* ---- Polling ---- */

static void test_pending_then_slow_down_then_approved(void) {
    start_recorded();
    serve(PATH_TOKEN, "romm/device-pending.http", 1);
    serve(PATH_TOKEN, "romm/device-slow-down.http", 1);
    serve(PATH_TOKEN, "romm/device-token.http", 1);

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, poll());
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_PAIRING_PENDING, result.state);
    TEST_ASSERT_EQUAL_STRING("", result.token);
    TEST_ASSERT_EQUAL_UINT32(RECORDED_INTERVAL_S, pairing.interval_s);

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, poll());
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_PAIRING_SLOW_DOWN, result.state);
    TEST_PRINTF("RFC 8628: slow_down adds 5 s to the interval for good");
    TEST_ASSERT_EQUAL_UINT32(RECORDED_INTERVAL_S + SKIFF_ROMM_PAIRING_SLOW_DOWN_S,
                             pairing.interval_s);

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, poll());
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_PAIRING_APPROVED, result.state);
    TEST_ASSERT_EQUAL_STRING(TOKEN, result.token);
    TEST_ASSERT_EQUAL_STRING(DEVICE_ID, result.device_id);

    TEST_ASSERT_EQUAL_STRING("{\"device_code\":\"" DEVICE_CODE "\"}", fake.log[1].body);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", fake.log[1].headers, "polling sends no token");
}

static void test_a_refused_or_expired_pairing_is_over(void) {
    start_recorded();
    serve(PATH_TOKEN, "romm/device-denied.http", 1);
    serve(PATH_TOKEN, "romm/device-expired.http", 1);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_PAIRING_DENIED, poll());
    TEST_ASSERT_EQUAL_STRING("", result.token);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_PAIRING_EXPIRED, poll());
    TEST_ASSERT_EQUAL_STRING("", result.device_id);
}

static void test_too_many_requests_counts_as_slow_down(void) {
    start_recorded();
    serve_raw(PATH_TOKEN, "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 10\r\n\r\nslow");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, poll());
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_PAIRING_SLOW_DOWN, result.state);
    TEST_ASSERT_EQUAL_UINT32(RECORDED_INTERVAL_S + SKIFF_ROMM_PAIRING_SLOW_DOWN_S,
                             pairing.interval_s);
}

static void test_slow_down_never_grows_past_the_limit(void) {
    start_recorded();
    serve(PATH_TOKEN, "romm/device-slow-down.http", 0);
    pairing.interval_s = SKIFF_ROMM_PAIRING_INTERVAL_MAX_S - 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, poll());
    TEST_ASSERT_EQUAL_UINT32(SKIFF_ROMM_PAIRING_INTERVAL_MAX_S - 1, pairing.interval_s);
}

typedef struct bad_poll {
    const char *why;
    const char *raw;
    skiff_err expected;
} bad_poll;

static void test_a_poll_answer_that_is_neither_is_refused(void) {
    static const bad_poll CASES[] = {
        {"an unknown detail", JSON_400 "{\"detail\":\"invalid_grant\"}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"a 400 without JSON", "HTTP/1.1 400 Bad Request\r\n\r\n<html>bad</html>",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"a detail that is not a string", JSON_400 "{\"detail\":[]}", SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"a token with a blank",
         JSON_200 "{\"access_token\":\"rmm x\",\"device_id\":\"d\"," SCOPES "}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"no device id", JSON_200 "{\"access_token\":\"rmm_x\"," SCOPES "}",
         SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"a proxy page with 200", "HTTP/1.1 200 OK\r\n\r\n<html>", SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"RomM's refusal of the device code", "HTTP/1.1 401 Unauthorized\r\n\r\n{}",
         SKIFF_ERR_ROMM_UNAUTHORIZED},
        {"a redirect", "HTTP/1.1 302 Found\r\n\r\n", SKIFF_ERR_ROMM_BAD_RESPONSE},
        {"a server error", "HTTP/1.1 503 Unavailable\r\n\r\n", SKIFF_ERR_ROMM_SERVER},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        skiff_transport_destroy(&fake.base);
        fake_transport_init(&fake);
        start_recorded();
        serve_raw(PATH_TOKEN, CASES[i].raw);
        TEST_PRINTF("%s", CASES[i].why);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(skiff_err_name(CASES[i].expected), skiff_err_name(poll()),
                                         CASES[i].why);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", result.token, CASES[i].why);
    }
}

static void test_a_token_too_long_for_config_is_refused(void) {
    char raw[RAW_MAX];
    char token[SKIFF_CONFIG_TOKEN_MAX + 1];
    memset(token, 't', sizeof token - 1);
    token[sizeof token - 1] = '\0';
    snprintf(raw, sizeof raw, JSON_200 "{\"access_token\":\"%s\",\"device_id\":\"d\"," SCOPES "}",
             token);
    start_recorded();
    serve_raw(PATH_TOKEN, raw);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, poll());
    token[sizeof token - 2] = '\0';
    snprintf(raw, sizeof raw, JSON_200 "{\"access_token\":\"%s\",\"device_id\":\"d\"," SCOPES "}",
             token);
    skiff_transport_destroy(&fake.base);
    fake_transport_init(&fake);
    start_recorded();
    serve_raw(PATH_TOKEN, raw);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, poll());
    TEST_ASSERT_EQUAL_size_t(SKIFF_CONFIG_TOKEN_MAX - 1, strlen(result.token));
}

static void serve_token_with_scopes(const char *scopes) {
    char raw[RAW_MAX];
    snprintf(raw, sizeof raw,
             JSON_200 "{\"access_token\":\"" TOKEN "\",\"device_id\":\"" DEVICE_ID
                      "\",\"scopes\":%s,\"expires_at\":null}",
             scopes);
    serve_raw(PATH_TOKEN, raw);
}

static void test_a_token_that_cannot_browse_or_download_is_not_kept(void) {
    start_recorded();
    TEST_PRINTF("the approver unticked roms.read: the token could not download");
    serve_token_with_scopes("[\"platforms.read\"]");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_PAIRING_SCOPES, poll());
    TEST_ASSERT_EQUAL_STRING("", result.token);
    TEST_ASSERT_EQUAL_STRING("", result.device_id);
}

static void test_a_token_with_scopes_skiff_did_not_ask_for_is_not_kept(void) {
    start_recorded();
    serve_token_with_scopes("[\"platforms.read\",\"roms.read\",\"roms.write\"]");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, poll());
    TEST_ASSERT_EQUAL_STRING("", result.token);
}

static void test_the_save_sync_scopes_are_not_kept_before_save_sync_asks_for_them(void) {
    start_recorded();
    TEST_PRINTF("a server that grants devices.* or assets.* unasked gets its token refused");
    serve_token_with_scopes("[\"platforms.read\",\"roms.read\",\"devices.write\"]");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, poll());
    TEST_ASSERT_EQUAL_STRING("", result.token);
}

static void test_the_granted_scopes_may_come_in_any_order(void) {
    start_recorded();
    serve_token_with_scopes("[\"roms.read\",\"platforms.read\"]");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, poll());
    TEST_ASSERT_EQUAL_INT(SKIFF_ROMM_PAIRING_APPROVED, result.state);
}

static void test_a_token_without_a_list_of_scopes_is_refused(void) {
    start_recorded();
    serve_raw(PATH_TOKEN, JSON_200 "{\"access_token\":\"" TOKEN "\",\"device_id\":\"" DEVICE_ID
                                   "\",\"scopes\":\"roms.read platforms.read\"}");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, poll());
    TEST_ASSERT_EQUAL_STRING("", result.token);
}

static void test_a_long_secret_answer_grows_its_buffer_and_is_refused(void) {
    enum { FILLER_BYTES = 40 * 1024 };
    char *raw = malloc(FILLER_BYTES + RAW_MAX);
    TEST_ASSERT_NOT_NULL(raw);
    int used = snprintf(raw, RAW_MAX, JSON_400 "{\"detail\":\"");
    memset(raw + used, 'x', FILLER_BYTES);
    snprintf(raw + used + FILLER_BYTES, RAW_MAX, "\"}");
    start_recorded();
    serve_raw(PATH_TOKEN, raw);
    free(raw);
    TEST_PRINTF("a 40 KB answer outgrows the first buffer, which pairing copies and wipes");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, poll());
}

static void test_wiping_a_tree_clears_every_string_in_place(void) {
    cJSON *root = cJSON_Parse("{\"access_token\":\"rmm_secret\",\"count\":3,"
                              "\"nested\":{\"list\":[\"device_code_x\"]}}");
    TEST_ASSERT_NOT_NULL(root);
    const char *token = cJSON_GetObjectItemCaseSensitive(root, "access_token")->valuestring;
    const cJSON *nested = cJSON_GetObjectItemCaseSensitive(root, "nested");
    const char *code =
        cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(nested, "list"), 0)->valuestring;
    skiff_romm_json_wipe_strings(root);
    for (size_t i = 0; i < sizeof "rmm_secret" - 1; i++) {
        TEST_ASSERT_EQUAL_CHAR('\0', token[i]);
    }
    for (size_t i = 0; i < sizeof "device_code_x" - 1; i++) {
        TEST_ASSERT_EQUAL_CHAR('\0', code[i]);
    }
    TEST_PRINTF("numbers and member names stay; NULL is fine");
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetObjectItemCaseSensitive(root, "count")->valueint);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(root, "nested"));
    skiff_romm_json_wipe_strings(NULL);
    cJSON_Delete(root);
}

static void test_a_token_answer_with_junk_after_it_is_refused(void) {
    start_recorded();
    serve_raw(PATH_TOKEN, JSON_200 "{\"access_token\":\"" TOKEN "\",\"device_id\":\"" DEVICE_ID
                                   "\"," SCOPES "} trailing");
    TEST_PRINTF("parsed, then refused for what follows: the tree is wiped before it is freed");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, poll());
    TEST_ASSERT_EQUAL_STRING("", result.token);
}

static void test_poll_needs_a_started_pairing(void) {
    memset(&pairing, 0, sizeof pairing);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, poll());
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_pairing_poll(&client, NULL, &result));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_pairing_poll(&client, &pairing, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_pairing_poll(NULL, &pairing, &result));
    TEST_ASSERT_EQUAL_size_t(0, fake.request_count);
}

static void test_clear_wipes_the_secrets(void) {
    start_recorded();
    serve(PATH_TOKEN, "romm/device-token.http", 0);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, poll());
    skiff_romm_pairing_clear(&pairing, &result);
    TEST_ASSERT_EQUAL_STRING("", pairing.device_code);
    TEST_ASSERT_EQUAL_STRING("", result.token);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(USER_CODE, pairing.user_code, "the code shown is no secret");
    skiff_romm_pairing_clear(NULL, NULL);
}

/* ---- Saving ---- */

static void approved(void) {
    memset(&result, 0, sizeof result);
    result.state = SKIFF_ROMM_PAIRING_APPROVED;
    snprintf(result.token, sizeof result.token, "%s", TOKEN);
    snprintf(result.device_id, sizeof result.device_id, "%s", DEVICE_ID);
}

static void test_the_pairing_lands_in_config_ini_and_parses_back(void) {
    static const char TEXT[] = "# my PSP\n[server]\nurl = https://romm.test:8443\n\n[auth]\n"
                               "token = rmm_old\n";
    static char out[TEXT_BUFFER];
    size_t length = 0;
    approved();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_romm_pairing_config_text(TEXT, sizeof TEXT - 1, IDENTIFIER, &result,
                                                         out, sizeof out, &length, NULL));
    TEST_PRINTF("config.ini after pairing:\n%s", out);
    TEST_ASSERT_EQUAL_size_t(strlen(out), length);
    TEST_ASSERT_EQUAL_STRING("# my PSP\n[server]\nurl = https://romm.test:8443\n\n[auth]\n"
                             "token = " TOKEN "\ndevice_identifier = " IDENTIFIER
                             "\ndevice_id = " DEVICE_ID "\n",
                             out);
    skiff_config config;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_parse(out, length, &config, NULL));
    TEST_ASSERT_EQUAL_STRING(TOKEN, config.token);
    TEST_ASSERT_EQUAL_STRING(IDENTIFIER, config.device_identifier);
    TEST_ASSERT_EQUAL_STRING(DEVICE_ID, config.device_id);
    TEST_ASSERT_EQUAL_STRING("https://romm.test:8443", config.server_url);
}

static void test_saving_refuses_a_pairing_that_is_not_approved_or_does_not_fit(void) {
    static char out[TEXT_BUFFER];
    size_t length = 0;
    skiff_config_issue issue;
    approved();
    result.state = SKIFF_ROMM_PAIRING_PENDING;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_romm_pairing_config_text("", 0, IDENTIFIER, &result, out, sizeof out, &length, NULL));
    approved();
    TEST_PRINTF("a damaged config.ini is reported, not saved over");
    static const char DAMAGED[] = "[server]\nnot a setting\n";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_PARSE, skiff_config_parse(DAMAGED, sizeof DAMAGED - 1,
                                                                     &(skiff_config){0}, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_PARSE, skiff_romm_pairing_config_text(
                                                      DAMAGED, sizeof DAMAGED - 1, IDENTIFIER,
                                                      &result, out, sizeof out, &length, &issue));
    TEST_ASSERT_EQUAL_INT(2, issue.line);
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_PRINTF("an output too small for the result");
    char small[32];
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_romm_pairing_config_text("", 0, IDENTIFIER, &result, small,
                                                         sizeof small, &length, NULL));
    TEST_ASSERT_EQUAL_STRING("", small);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_pairing_config_text(NULL, 1, IDENTIFIER, &result, out,
                                                         sizeof out, &length, NULL));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_romm_pairing_config_text("", 0, NULL, &result, out, sizeof out, &length, NULL));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_romm_pairing_config_text("", 0, IDENTIFIER, &result, out, sizeof out, NULL, NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_identifier_is_the_random_bytes_in_hex);
    RUN_TEST(test_start_asks_for_skiffs_scopes_and_shows_the_code);
    RUN_TEST(test_start_refuses_a_bad_identifier_before_sending);
    RUN_TEST(test_start_refuses_what_is_not_a_pairing);
    RUN_TEST(test_start_reports_the_network);
    RUN_TEST(test_pending_then_slow_down_then_approved);
    RUN_TEST(test_a_refused_or_expired_pairing_is_over);
    RUN_TEST(test_too_many_requests_counts_as_slow_down);
    RUN_TEST(test_slow_down_never_grows_past_the_limit);
    RUN_TEST(test_a_poll_answer_that_is_neither_is_refused);
    RUN_TEST(test_a_token_too_long_for_config_is_refused);
    RUN_TEST(test_a_token_that_cannot_browse_or_download_is_not_kept);
    RUN_TEST(test_a_token_with_scopes_skiff_did_not_ask_for_is_not_kept);
    RUN_TEST(test_the_save_sync_scopes_are_not_kept_before_save_sync_asks_for_them);
    RUN_TEST(test_the_granted_scopes_may_come_in_any_order);
    RUN_TEST(test_a_token_without_a_list_of_scopes_is_refused);
    RUN_TEST(test_a_long_secret_answer_grows_its_buffer_and_is_refused);
    RUN_TEST(test_wiping_a_tree_clears_every_string_in_place);
    RUN_TEST(test_a_token_answer_with_junk_after_it_is_refused);
    RUN_TEST(test_poll_needs_a_started_pairing);
    RUN_TEST(test_clear_wipes_the_secrets);
    RUN_TEST(test_the_pairing_lands_in_config_ini_and_parses_back);
    RUN_TEST(test_saving_refuses_a_pairing_that_is_not_approved_or_does_not_fit);
    return UNITY_END();
}
