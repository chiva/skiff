/*
 * RomM's install calls (skiff/romm_installs.h) against the fake transport replaying RomM 5.4.0's
 * answers (tests/fixtures/romm/installs-*.http, recorded by
 * tests/integration/record-installs-fixtures.sh with synthetic device ids): remote_install
 * registered and turned off, a claim of three sends, the same claim again and an empty one, reports
 * of done, already_installed and failed, a repeat (404) and a send not claimed yet (409), and a
 * token that only browses refused both (403). Requests are checked byte for byte, statuses mapped
 * as the app reads them, and every way an answer can be wrong refused; claims RomM never recorded
 * (more than 64 sends, ids Skiff will not put in a URL, wrong types) are built here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiff/romm_installs.h"

#include "fake_transport.h"
#include "unity.h"

#define BASE_URL "https://romm.test"
#define TOKEN "rmm_0123456789abcdef0123456789abcdef"
/* What the recorded answers hold (record-installs-fixtures.sh). */
#define DEVICE_D "00000000-0000-4000-8000-00000000000d"
#define PAYLOAD_ROM_ID 1
#define EXTRA_ROM_ID 2
#define FOREIGN_ROM_ID 3
#define REQUEST_PAYLOAD "fbff8d79-b3df-4492-a9a2-fe8d935b6513"
#define REQUEST_EXTRA "e006a065-f327-434b-ba2b-5d8a9b8acf20"
#define REQUEST_FOREIGN "b133deee-8d1d-4ef8-b526-80a31aba4324"
/* The recorder's send it had not claimed yet: RomM's 409 does not name it. */
#define REQUEST_PENDING "5d6e7f80-91a2-4b3c-8d4e-5f60718293a4"
#define NOT_PSP_REASON "Ese juego no es de PSP [219]"
#define PATH_DEVICE_D "/api/devices/" DEVICE_D
#define PATH_CLAIM_D PATH_DEVICE_D "/installs/claim"
#define PATH_REPORT(request) PATH_DEVICE_D "/installs/" request
#define JSON_200 "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
#define STATUS_ONLY(line) "HTTP/1.1 " line "\r\nContent-Type: application/json\r\n\r\n{}"
#define REPORT_BODY_START "{\"status\":\"failed\",\"reason\":\""
/* One claimed request as RomM writes it, with id, ROM and file ids filled in. */
#define CLAIMED_FORMAT                                                                             \
    "{\"id\":\"%s\",\"user_id\":1,\"device_id\":\"" DEVICE_D "\",\"rom_id\":%u,\"file_ids\":[%s]," \
    "\"status\":\"taken\",\"reason\":null,\"created_at\":\"2026-10-12T10:00:00Z\","                \
    "\"updated_at\":\"2026-10-12T10:00:01Z\"}"
#define CLAIMED_MAX 512
#define RAW_MAX 2048
/* Past what a claim keeps, so the rest wait for the next one. */
#define FLOOD_REQUESTS (SKIFF_ROMM_INSTALL_CLAIM_MAX + 6)
#define FLOOD_RAW_MAX ((size_t)FLOOD_REQUESTS * (CLAIMED_MAX + 1) + sizeof JSON_200 + 2)
/* More file ids than are read, the last of them not an id: only counted, never refused. */
#define MANY_FILES (SKIFF_ROMM_INSTALL_FILE_IDS_READ_MAX + 2)
#define FILE_IDS_MAX (MANY_FILES * 8)
#define REASON_MAX (SKIFF_ROMM_INSTALL_REASON_MAX_BYTES + 100)

static fake_transport fake;
static skiff_romm_client client;
static skiff_romm_install_claim claim;

void setUp(void) {
    fake_transport_init(&fake);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_client_init(&client, &fake.base, BASE_URL, TOKEN));
    memset(&claim, 0xAB, sizeof claim);
}

void tearDown(void) { skiff_transport_destroy(&fake.base); }

/* Drops every route and the log, for a test that serves another answer. */
static void reset_fake(void) {
    skiff_transport_destroy(&fake.base);
    fake_transport_init(&fake);
}

static void serve(const char *path, const char *fixture) {
    TEST_ASSERT_NOT_NULL_MESSAGE(fake_transport_add_fixture(&fake, path, fixture), fixture);
}

static void serve_raw(const char *path, const char *raw) {
    TEST_ASSERT_NOT_NULL(fake_transport_add_raw(&fake, path, raw, strlen(raw)));
}

/* Every request carried the token in its Authorization header and nowhere else. */
static void assert_token_only_in_headers(void) {
    for (size_t i = 0; i < fake.log_count; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(fake.log[i].headers, "Authorization: Bearer " TOKEN),
                                     fake.log[i].url);
        TEST_ASSERT_NULL_MESSAGE(strstr(fake.log[i].url, TOKEN), "no token in a URL");
        TEST_ASSERT_NULL_MESSAGE(strstr(fake.log[i].body, TOKEN), "no token in a body");
    }
}

static void assert_claim_empty(void) {
    TEST_ASSERT_EQUAL_size_t(0, claim.total);
    TEST_ASSERT_EQUAL_size_t(0, claim.dropped);
    TEST_ASSERT_EQUAL_size_t(0, claim.count);
    TEST_ASSERT_EQUAL_STRING("", claim.requests[0].id);
}

static skiff_err claim_d(void) {
    const skiff_err err = skiff_romm_installs_claim(&client, DEVICE_D, &claim);
    TEST_PRINTF("claim -> %s: %zu kept of %zu, %zu dropped", skiff_err_name(err), claim.count,
                claim.total, claim.dropped);
    for (size_t i = 0; i < claim.count && i < SKIFF_ROMM_INSTALL_CLAIM_MAX; i++) {
        TEST_PRINTF("  %s: ROM %llu, %zu file(s)", claim.requests[i].id,
                    (unsigned long long)claim.requests[i].rom_id, claim.requests[i].file_count);
    }
    return err;
}

/* Parses a claim body built here. */
static skiff_err parse(const char *json) {
    const skiff_err err = skiff_romm_parse_install_claim(json, strlen(json), DEVICE_D, &claim);
    TEST_PRINTF("parse -> %s: %zu kept of %zu, %zu dropped", skiff_err_name(err), claim.count,
                claim.total, claim.dropped);
    return err;
}

static void claimed(char *out, size_t out_size, const char *id, unsigned rom_id,
                    const char *file_ids) {
    const int written = snprintf(out, out_size, CLAIMED_FORMAT, id, rom_id, file_ids);
    TEST_ASSERT_TRUE(written > 0 && (size_t)written < out_size);
}

static skiff_err report_d(const char *request_id, skiff_romm_install_status status,
                          const char *reason) {
    const skiff_err err = skiff_romm_installs_report(&client, DEVICE_D, request_id, status, reason);
    TEST_PRINTF("report %s as %d -> %s, sent '%s'", request_id != NULL ? request_id : "(null)",
                (int)status, skiff_err_name(err),
                fake.log_count > 0 ? fake.log[fake.log_count - 1].body : "nothing");
    return err;
}

/* ---- Register ---- */

static void test_register_sends_only_remote_install_and_reads_it_back(void) {
    serve(PATH_DEVICE_D, "romm/installs-register.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_installs_register(&client, DEVICE_D, 1));
    TEST_PRINTF("sent: PUT %s %s", fake.log[0].url, fake.log[0].body);
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_PUT, fake.log[0].method);
    TEST_ASSERT_EQUAL_STRING(BASE_URL PATH_DEVICE_D, fake.log[0].url);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("{\"capabilities\":{\"remote_install\":true}}",
                                     fake.log[0].body,
                                     "RomM replaces the whole capabilities with what is sent");
    TEST_ASSERT_EQUAL_STRING("application/json", fake.log[0].content_type);
    TEST_ASSERT_FALSE(fake.log[0].long_transfer);
    assert_token_only_in_headers();
}

static void test_turning_it_off_sends_false_and_reads_it_back(void) {
    serve(PATH_DEVICE_D, "romm/installs-register-off.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_installs_register(&client, DEVICE_D, 0));
    TEST_ASSERT_EQUAL_STRING("{\"capabilities\":{\"remote_install\":false}}", fake.log[0].body);
    TEST_PRINTF("any non-zero on is on");
    reset_fake();
    serve(PATH_DEVICE_D, "romm/installs-register.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_installs_register(&client, DEVICE_D, 2));
    TEST_ASSERT_EQUAL_STRING("{\"capabilities\":{\"remote_install\":true}}", fake.log[0].body);
}

static void test_a_register_answer_that_is_not_what_was_sent_confirms_nothing(void) {
    static const char *const ANSWERS[] = {
        "{\"id\":\"" DEVICE_D "\",\"capabilities\":{\"remote_install\":false}}",
        "{\"id\":\"00000000-0000-4000-8000-00000000000e\",\"capabilities\":"
        "{\"remote_install\":true}}",
        "{\"id\":\"" DEVICE_D "\",\"capabilities\":{\"remote_install\":\"true\"}}",
        "{\"id\":\"" DEVICE_D "\",\"capabilities\":{\"remote_install\":1}}",
        "{\"id\":\"" DEVICE_D "\",\"capabilities\":{}}",
        "{\"id\":\"" DEVICE_D "\",\"capabilities\":null}",
        "{\"id\":\"" DEVICE_D "\"}",
        "{\"capabilities\":{\"remote_install\":true}}",
        "[{\"id\":\"" DEVICE_D "\",\"capabilities\":{\"remote_install\":true}}]",
        "<html>Sign in</html>",
        "",
    };
    for (size_t i = 0; i < sizeof ANSWERS / sizeof ANSWERS[0]; i++) {
        char raw[RAW_MAX];
        snprintf(raw, sizeof raw, JSON_200 "%s", ANSWERS[i]);
        reset_fake();
        serve_raw(PATH_DEVICE_D, raw);
        const skiff_err err = skiff_romm_installs_register(&client, DEVICE_D, 1);
        TEST_PRINTF("answer '%s' -> %s", ANSWERS[i], skiff_err_name(err));
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, err);
    }
}

static void test_register_maps_romms_refusals(void) {
    serve(PATH_DEVICE_D, "romm/installs-register-forbidden.http");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_ROMM_SENDS_FORBIDDEN,
                                  skiff_romm_installs_register(&client, DEVICE_D, 1),
                                  "a token without devices.write (the recorded browse-only one)");
    static const struct {
        const char *raw;
        skiff_err expected;
    } CASES[] = {
        {STATUS_ONLY("401 Unauthorized"), SKIFF_ERR_ROMM_UNAUTHORIZED},
        {STATUS_ONLY("404 Not Found"), SKIFF_ERR_ROMM_NOT_FOUND},
        {STATUS_ONLY("422 Unprocessable Content"), SKIFF_ERR_ROMM_BAD_RESPONSE},
        {STATUS_ONLY("500 Internal Server Error"), SKIFF_ERR_ROMM_SERVER},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        reset_fake();
        serve_raw(PATH_DEVICE_D, CASES[i].raw);
        const skiff_err err = skiff_romm_installs_register(&client, DEVICE_D, 1);
        TEST_PRINTF("case %zu -> %s", i, skiff_err_name(err));
        TEST_ASSERT_EQUAL_INT(CASES[i].expected, err);
    }
}

static void test_a_device_id_is_percent_encoded_in_the_path(void) {
    serve_raw("/api/devices/a%2F..%3Fb%23",
              JSON_200 "{\"id\":\"a/..?b#\",\"capabilities\":{\"remote_install\":true}}");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_romm_installs_register(&client, "a/..?b#", 1));
    TEST_PRINTF("sent to %s", fake.log[0].url);
    TEST_ASSERT_EQUAL_STRING(BASE_URL "/api/devices/a%2F..%3Fb%23", fake.log[0].url);
}

static void test_values_romm_would_refuse_send_nothing(void) {
    char long_id[SKIFF_CONFIG_DEVICE_ID_MAX + 1];
    memset(long_id, 'a', sizeof long_id - 1);
    long_id[sizeof long_id - 1] = '\0';
    const char *const DEVICES[] = {NULL, "", "a b", "a\x01", "caf\xC3\xA9", long_id};
    for (size_t i = 0; i < sizeof DEVICES / sizeof DEVICES[0]; i++) {
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                              skiff_romm_installs_register(&client, DEVICES[i], 1));
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                              skiff_romm_installs_claim(&client, DEVICES[i], &claim));
        assert_claim_empty();
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                              skiff_romm_installs_report(&client, DEVICES[i], REQUEST_PAYLOAD,
                                                         SKIFF_ROMM_INSTALL_DONE, NULL));
    }
    skiff_romm_client empty;
    memset(&empty, 0, sizeof empty);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_installs_register(NULL, DEVICE_D, 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_romm_installs_register(&empty, DEVICE_D, 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_installs_claim(&empty, DEVICE_D, &claim));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_installs_claim(&client, DEVICE_D, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_installs_report(&empty, DEVICE_D, REQUEST_PAYLOAD,
                                                     SKIFF_ROMM_INSTALL_DONE, NULL));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, fake.log_count, "nothing was sent");
}

/* ---- Claim ---- */

static void assert_the_recorded_three(void) {
    TEST_ASSERT_EQUAL_size_t(3, claim.total);
    TEST_ASSERT_EQUAL_size_t(3, claim.count);
    TEST_ASSERT_EQUAL_size_t(0, claim.dropped);
    static const char *const IDS[] = {REQUEST_PAYLOAD, REQUEST_EXTRA, REQUEST_FOREIGN};
    static const uint64_t ROMS[] = {PAYLOAD_ROM_ID, EXTRA_ROM_ID, FOREIGN_ROM_ID};
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_STRING(IDS[i], claim.requests[i].id);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(ROMS[i], claim.requests[i].rom_id, "oldest first");
        TEST_ASSERT_EQUAL_size_t(1, claim.requests[i].file_count);
    }
}

static void test_a_claim_takes_the_three_sends_oldest_first(void) {
    serve(PATH_CLAIM_D, "romm/installs-claim.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, claim_d());
    TEST_PRINTF("sent: POST %s with %zu body bytes", fake.log[0].url, fake.log[0].body_size);
    TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_POST, fake.log[0].method);
    TEST_ASSERT_EQUAL_STRING(BASE_URL PATH_CLAIM_D, fake.log[0].url);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, fake.log[0].body_size, "a claim has no body");
    TEST_ASSERT_EQUAL_STRING("", fake.log[0].content_type);
    TEST_ASSERT_FALSE(fake.log[0].long_transfer);
    assert_token_only_in_headers();
    assert_the_recorded_three();
}

static void test_an_unreported_send_comes_back_on_every_claim(void) {
    serve(PATH_CLAIM_D, "romm/installs-claim-again.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, claim_d());
    assert_the_recorded_three();
}

static void test_a_claim_with_nothing_sent_is_empty(void) {
    serve(PATH_CLAIM_D, "romm/installs-claim-empty.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, claim_d());
    assert_claim_empty();
}

static void test_a_claim_maps_romms_refusals(void) {
    serve(PATH_CLAIM_D, "romm/installs-claim-forbidden.http");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_ROMM_SENDS_FORBIDDEN, claim_d(),
                                  "the recorded browse-only token");
    assert_claim_empty();
    static const struct {
        const char *raw;
        skiff_err expected;
    } CASES[] = {
        /* The device was deleted in RomM, or DEVICE_INSTALL_ENABLED is off. */
        {STATUS_ONLY("404 Not Found"), SKIFF_ERR_ROMM_NOT_FOUND},
        /* RomM's requests kept changing under the claim: try again later. */
        {STATUS_ONLY("409 Conflict"), SKIFF_ERR_ROMM_SERVER},
        {STATUS_ONLY("401 Unauthorized"), SKIFF_ERR_ROMM_UNAUTHORIZED},
        {STATUS_ONLY("503 Service Unavailable"), SKIFF_ERR_ROMM_SERVER},
        {JSON_200 "<html>Sign in</html>", SKIFF_ERR_ROMM_BAD_RESPONSE},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        reset_fake();
        serve_raw(PATH_CLAIM_D, CASES[i].raw);
        memset(&claim, 0xAB, sizeof claim);
        TEST_ASSERT_EQUAL_INT(CASES[i].expected, claim_d());
        assert_claim_empty();
    }
}

static void test_a_claim_keeps_the_first_64_and_counts_the_rest(void) {
    char *raw = malloc(FLOOD_RAW_MAX);
    TEST_ASSERT_NOT_NULL(raw);
    size_t used = (size_t)snprintf(raw, FLOOD_RAW_MAX, JSON_200 "[");
    for (unsigned i = 0; i < FLOOD_REQUESTS; i++) {
        char id[SKIFF_ROMM_INSTALL_REQUEST_ID_MAX];
        char item[CLAIMED_MAX];
        snprintf(id, sizeof id, "00000000-0000-4000-8000-%012x", i);
        claimed(item, sizeof item, id, i + 1, "1");
        used += (size_t)snprintf(raw + used, FLOOD_RAW_MAX - used, "%s%s", i > 0 ? "," : "", item);
    }
    snprintf(raw + used, FLOOD_RAW_MAX - used, "]");
    serve_raw(PATH_CLAIM_D, raw);
    free(raw);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, claim_d());
    TEST_ASSERT_EQUAL_size_t(FLOOD_REQUESTS, claim.total);
    TEST_ASSERT_EQUAL_size_t(SKIFF_ROMM_INSTALL_CLAIM_MAX, claim.count);
    TEST_ASSERT_EQUAL_size_t(0, claim.dropped);
    for (size_t i = 0; i < claim.count; i++) {
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(i + 1, claim.requests[i].rom_id, "in RomM's order");
    }
    TEST_ASSERT_EQUAL_STRING("00000000-0000-4000-8000-00000000003f", claim.requests[63].id);
}

static void test_an_id_skiff_would_not_put_in_a_url_is_dropped(void) {
    static const char *const BAD_IDS[] = {
        "FBFF8D79-B3DF-4492-A9A2-FE8D935B6513",
        "fbff8d79-b3df-4492-a9a2-fe8d935b651",
        "fbff8d79-b3df-4492-a9a2-fe8d935b65130",
        "fbff8d79b3df-4492-a9a2-fe8d935b65135",
        "fbff8d79-b3df-4492-a9a2-fe8d935b651g",
        "../../../api/heartbeat?00000000000000",
        "fbff8d79-b3df-4492-a9a2/fe8d935b6513",
        "{fbff8d79-b3df-4492-a9a2-fe8d935b6}",
        "",
    };
    for (size_t i = 0; i < sizeof BAD_IDS / sizeof BAD_IDS[0]; i++) {
        char bad[CLAIMED_MAX];
        char good[CLAIMED_MAX];
        char json[2 * CLAIMED_MAX + 4];
        claimed(bad, sizeof bad, BAD_IDS[i], 7, "7");
        claimed(good, sizeof good, REQUEST_EXTRA, 8, "8");
        snprintf(json, sizeof json, "[%s,%s]", bad, good);
        TEST_PRINTF("id '%s'", BAD_IDS[i]);
        TEST_ASSERT_FALSE(skiff_romm_install_request_id_valid(BAD_IDS[i]));
        TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(json));
        TEST_ASSERT_EQUAL_size_t(2, claim.total);
        TEST_ASSERT_EQUAL_size_t(1, claim.dropped);
        TEST_ASSERT_EQUAL_size_t(1, claim.count);
        TEST_ASSERT_EQUAL_STRING(REQUEST_EXTRA, claim.requests[0].id);
        TEST_ASSERT_EQUAL_UINT64(8, claim.requests[0].rom_id);
    }
    TEST_ASSERT_FALSE(skiff_romm_install_request_id_valid(NULL));
    TEST_ASSERT_TRUE(skiff_romm_install_request_id_valid(REQUEST_PAYLOAD));
}

static void test_fields_skiff_does_not_read_are_ignored(void) {
    static const char JSON[] =
        "[{\"id\":\"" REQUEST_PAYLOAD "\",\"device_id\":\"" DEVICE_D "\",\"rom_id\":1,"
        "\"file_ids\":[1,2],\"status\":\"taken\",\"reason\":\"anything\",\"user_id\":\"x\","
        "\"created_at\":null,\"new_in_romm_6\":{\"a\":[1]}}]";
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(JSON));
    TEST_ASSERT_EQUAL_size_t(1, claim.count);
    TEST_ASSERT_EQUAL_size_t(2, claim.requests[0].file_count);
}

static void test_file_ids_past_the_first_64_are_only_counted(void) {
    char ids[FILE_IDS_MAX] = "";
    for (int i = 1; i < MANY_FILES; i++) {
        snprintf(ids + strlen(ids), sizeof ids - strlen(ids), "%d,", i);
    }
    snprintf(ids + strlen(ids), sizeof ids - strlen(ids), "\"x\"");
    char item[CLAIMED_MAX];
    char json[CLAIMED_MAX + 2];
    claimed(item, sizeof item, REQUEST_PAYLOAD, 1, ids);
    snprintf(json, sizeof json, "[%s]", item);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(json));
    TEST_ASSERT_EQUAL_size_t(MANY_FILES, claim.requests[0].file_count);
}

static void test_a_claim_of_the_wrong_shape_is_refused_whole(void) {
    static const struct {
        const char *what;
        const char *id;
        const char *rest;
    } CASES[] = {
        {"an id that is not a string", "", "\"id\":7,"},
        {"no id", "", ""},
        {"a pending request", REQUEST_PAYLOAD, "\"status\":\"pending\","},
        {"another device's", REQUEST_PAYLOAD,
         "\"device_id\":\"00000000-0000-4000-8000-00000000000e\","},
        {"a ROM id of 0", REQUEST_PAYLOAD, "\"rom_id\":0,"},
        {"a ROM id that is a string", REQUEST_PAYLOAD, "\"rom_id\":\"1\","},
        {"a ROM id with a fraction", REQUEST_PAYLOAD, "\"rom_id\":1.5,"},
        {"a ROM id past 2^53", REQUEST_PAYLOAD, "\"rom_id\":9007199254740992,"},
        {"a negative ROM id", REQUEST_PAYLOAD, "\"rom_id\":-1,"},
        {"file ids that are not an array", REQUEST_PAYLOAD, "\"file_ids\":1,"},
        {"a file id that is a string", REQUEST_PAYLOAD, "\"file_ids\":[\"1\"],"},
        {"a file id of 0", REQUEST_PAYLOAD, "\"file_ids\":[0],"},
        {"a file id past 2^53", REQUEST_PAYLOAD, "\"file_ids\":[9007199254740992],"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        /* The first of duplicate keys is the one read: each case's field comes first. */
        char json[CLAIMED_MAX];
        snprintf(json, sizeof json,
                 "[{%s%s%s%s\"device_id\":\"" DEVICE_D "\",\"rom_id\":1,\"file_ids\":[1],"
                 "\"status\":\"taken\"}]",
                 CASES[i].rest, CASES[i].id[0] != '\0' ? "\"id\":\"" : "", CASES[i].id,
                 CASES[i].id[0] != '\0' ? "\"," : "");
        TEST_PRINTF("%s: %s", CASES[i].what, json);
        memset(&claim, 0xAB, sizeof claim);
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, parse(json));
        assert_claim_empty();
    }
    static const char *const BODIES[] = {"{}", "[1]", "[null]", "[[]]", "null", "\"taken\""};
    for (size_t i = 0; i < sizeof BODIES / sizeof BODIES[0]; i++) {
        TEST_PRINTF("body %s", BODIES[i]);
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, parse(BODIES[i]));
        assert_claim_empty();
    }
    TEST_PRINTF("one bad request refuses the claim, even after good ones");
    char good[CLAIMED_MAX];
    char json[CLAIMED_MAX + 16];
    claimed(good, sizeof good, REQUEST_PAYLOAD, 1, "1");
    snprintf(json, sizeof json, "[%s,7]", good);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, parse(json));
    assert_claim_empty();
}

static void test_parsing_a_claim_needs_somewhere_to_put_it_and_a_device(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_parse_install_claim("[]", 2, DEVICE_D, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_romm_parse_install_claim("[]", 2, "a b", &claim));
    assert_claim_empty();
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          skiff_romm_parse_install_claim(NULL, 0, DEVICE_D, &claim));
}

/* ---- Report ---- */

static void test_reports_end_each_send_as_the_recorder_did(void) {
    static const struct {
        const char *request;
        skiff_romm_install_status status;
        const char *reason;
        const char *fixture;
        const char *body;
    } CASES[] = {
        {REQUEST_PAYLOAD, SKIFF_ROMM_INSTALL_DONE, NULL, "romm/installs-report-done.http",
         "{\"status\":\"done\"}"},
        {REQUEST_EXTRA, SKIFF_ROMM_INSTALL_ALREADY_INSTALLED, "",
         "romm/installs-report-already-installed.http", "{\"status\":\"already_installed\"}"},
        {REQUEST_FOREIGN, SKIFF_ROMM_INSTALL_FAILED, NOT_PSP_REASON,
         "romm/installs-report-failed.http",
         "{\"status\":\"failed\",\"reason\":\"" NOT_PSP_REASON "\"}"},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char path[FAKE_TRANSPORT_URL_MAX];
        snprintf(path, sizeof path, PATH_DEVICE_D "/installs/%s", CASES[i].request);
        reset_fake();
        serve(path, CASES[i].fixture);
        TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                              report_d(CASES[i].request, CASES[i].status, CASES[i].reason));
        TEST_ASSERT_EQUAL_INT(SKIFF_HTTP_PUT, fake.log[0].method);
        TEST_ASSERT_EQUAL_STRING(path, fake.log[0].url + strlen(BASE_URL));
        TEST_ASSERT_EQUAL_STRING(CASES[i].body, fake.log[0].body);
        TEST_ASSERT_EQUAL_STRING("application/json", fake.log[0].content_type);
        assert_token_only_in_headers();
    }
}

static void test_a_report_with_nothing_left_to_end_is_not_found(void) {
    serve(PATH_REPORT(REQUEST_PAYLOAD), "romm/installs-report-repeat.http");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_ROMM_NOT_FOUND,
                                  report_d(REQUEST_PAYLOAD, SKIFF_ROMM_INSTALL_DONE, NULL),
                                  "already reported, or cancelled in the web UI");
    reset_fake();
    serve(PATH_REPORT(REQUEST_PENDING), "romm/installs-report-pending.http");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_ROMM_NOT_FOUND,
                                  report_d(REQUEST_PENDING, SKIFF_ROMM_INSTALL_DONE, NULL),
                                  "a send not claimed yet (409)");
    reset_fake();
    serve(PATH_REPORT(REQUEST_PAYLOAD), "romm/installs-claim-forbidden.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_SENDS_FORBIDDEN,
                          report_d(REQUEST_PAYLOAD, SKIFF_ROMM_INSTALL_DONE, NULL));
    reset_fake();
    serve_raw(PATH_REPORT(REQUEST_PAYLOAD), STATUS_ONLY("500 Internal Server Error"));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_SERVER,
                          report_d(REQUEST_PAYLOAD, SKIFF_ROMM_INSTALL_DONE, NULL));
}

static void test_a_report_answer_for_another_request_or_status_confirms_nothing(void) {
    serve(PATH_REPORT(REQUEST_EXTRA), "romm/installs-report-done.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          report_d(REQUEST_EXTRA, SKIFF_ROMM_INSTALL_DONE, NULL));
    reset_fake();
    serve(PATH_REPORT(REQUEST_PAYLOAD), "romm/installs-report-done.http");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          report_d(REQUEST_PAYLOAD, SKIFF_ROMM_INSTALL_FAILED, "x"));
    static const char *const ANSWERS[] = {
        "[]", "<html>Sign in</html>", "{\"id\":\"" REQUEST_PAYLOAD "\"}", "{\"status\":\"done\"}"};
    for (size_t i = 0; i < sizeof ANSWERS / sizeof ANSWERS[0]; i++) {
        char raw[RAW_MAX];
        snprintf(raw, sizeof raw, JSON_200 "%s", ANSWERS[i]);
        reset_fake();
        serve_raw(PATH_REPORT(REQUEST_PAYLOAD), raw);
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                              report_d(REQUEST_PAYLOAD, SKIFF_ROMM_INSTALL_DONE, NULL));
    }
}

static void test_a_report_romm_would_refuse_is_not_sent(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, report_d("FBFF8D79-B3DF-4492-A9A2-FE8D935B6513",
                                                          SKIFF_ROMM_INSTALL_DONE, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, report_d(NULL, SKIFF_ROMM_INSTALL_DONE, NULL));
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          report_d(REQUEST_PAYLOAD, (skiff_romm_install_status)7, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          report_d(REQUEST_PAYLOAD, SKIFF_ROMM_INSTALL_FAILED, "a\nb"));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          report_d(REQUEST_PAYLOAD, SKIFF_ROMM_INSTALL_FAILED, "a\xC2\x85"));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, fake.log_count, "nothing was sent");
}

/* ---- The reason's length ---- */

/* A reason of count 'a's followed by tail. */
static const char *reason_of(size_t count, const char *tail) {
    static char reason[REASON_MAX];
    memset(reason, 'a', count);
    snprintf(reason + count, sizeof reason - count, "%s", tail);
    return reason;
}

static void test_a_reason_is_cut_at_500_bytes_between_characters(void) {
    static const struct {
        size_t count;
        const char *tail;
        size_t kept;
    } CASES[] = {
        {0, "", 0},
        {SKIFF_ROMM_INSTALL_REASON_MAX_BYTES, "", SKIFF_ROMM_INSTALL_REASON_MAX_BYTES},
        {SKIFF_ROMM_INSTALL_REASON_MAX_BYTES + 1, "", SKIFF_ROMM_INSTALL_REASON_MAX_BYTES},
        /* A two-byte character ending exactly at the limit is kept... */
        {498, "\xC3\xA9", SKIFF_ROMM_INSTALL_REASON_MAX_BYTES},
        /* ...and one that would cross it is left out whole. */
        {499, "\xC3\xA9", 499},
        {499, "\xE6\x97\xA5", 499},
        {498, "\xE6\x97\xA5", 498},
        {497, "\xF0\x9F\x8E\xAE", 497},
        {498, "\xF0\x9F\x8E\xAEz", 498},
        {496, "\xF0\x9F\x8E\xAE", SKIFF_ROMM_INSTALL_REASON_MAX_BYTES},
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const char *reason = reason_of(CASES[i].count, CASES[i].tail);
        const size_t kept = skiff_romm_install_reason_length(reason);
        TEST_PRINTF("%zu 'a's and %zu more bytes: %zu kept", CASES[i].count, strlen(CASES[i].tail),
                    kept);
        TEST_ASSERT_EQUAL_size_t(CASES[i].kept, kept);
    }
    TEST_ASSERT_EQUAL_size_t(0, skiff_romm_install_reason_length(NULL));
}

static void test_a_long_reason_is_sent_cut(void) {
    serve_raw(PATH_REPORT(REQUEST_FOREIGN),
              JSON_200 "{\"id\":\"" REQUEST_FOREIGN "\",\"status\":\"failed\"}");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, report_d(REQUEST_FOREIGN, SKIFF_ROMM_INSTALL_FAILED,
                                             reason_of(499, "\xC3\xA9 and more")));
    const size_t expected = sizeof REPORT_BODY_START - 1 + 499 + sizeof "\"}" - 1;
    TEST_PRINTF("sent %zu bytes, expected %zu", fake.log[0].body_received, expected);
    TEST_ASSERT_EQUAL_size_t(expected, fake.log[0].body_received);
    TEST_ASSERT_EQUAL_INT(
        0, strncmp(fake.log[0].body, REPORT_BODY_START "aaa", sizeof REPORT_BODY_START + 2));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_register_sends_only_remote_install_and_reads_it_back);
    RUN_TEST(test_turning_it_off_sends_false_and_reads_it_back);
    RUN_TEST(test_a_register_answer_that_is_not_what_was_sent_confirms_nothing);
    RUN_TEST(test_register_maps_romms_refusals);
    RUN_TEST(test_a_device_id_is_percent_encoded_in_the_path);
    RUN_TEST(test_values_romm_would_refuse_send_nothing);
    RUN_TEST(test_a_claim_takes_the_three_sends_oldest_first);
    RUN_TEST(test_an_unreported_send_comes_back_on_every_claim);
    RUN_TEST(test_a_claim_with_nothing_sent_is_empty);
    RUN_TEST(test_a_claim_maps_romms_refusals);
    RUN_TEST(test_a_claim_keeps_the_first_64_and_counts_the_rest);
    RUN_TEST(test_an_id_skiff_would_not_put_in_a_url_is_dropped);
    RUN_TEST(test_fields_skiff_does_not_read_are_ignored);
    RUN_TEST(test_file_ids_past_the_first_64_are_only_counted);
    RUN_TEST(test_a_claim_of_the_wrong_shape_is_refused_whole);
    RUN_TEST(test_parsing_a_claim_needs_somewhere_to_put_it_and_a_device);
    RUN_TEST(test_reports_end_each_send_as_the_recorder_did);
    RUN_TEST(test_a_report_with_nothing_left_to_end_is_not_found);
    RUN_TEST(test_a_report_answer_for_another_request_or_status_confirms_nothing);
    RUN_TEST(test_a_report_romm_would_refuse_is_not_sent);
    RUN_TEST(test_a_reason_is_cut_at_500_bytes_between_characters);
    RUN_TEST(test_a_long_reason_is_sent_cut);
    return UNITY_END();
}
