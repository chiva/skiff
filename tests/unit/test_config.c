/*
 * config.ini (skiff/config.h): the guide's examples parse as written, every refusal names the line
 * and setting the player has to fix, edits leave the rest of a hand-written file byte for byte, and
 * a save cut short at any step leaves a file the next load can use.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "skiff/config.h"

#include "fake_storage.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define GUIDE_TOKEN_EXAMPLE                                                                        \
    "[server]\n"                                                                                   \
    "url = http://192.168.1.20:8080\n"                                                             \
    "\n"                                                                                           \
    "[auth]\n"                                                                                     \
    "token = rmm_0123456789abcdef\n"
#define GUIDE_HEADERS_EXAMPLE                                                                      \
    "[headers]\n"                                                                                  \
    "CF-Access-Client-Id = 0123abcd.access\n"                                                      \
    "CF-Access-Client-Secret = s3cr#t/with=signs\n"
#define GUIDE_MTLS_EXAMPLE                                                                         \
    "[server]\n"                                                                                   \
    "url = https://romm-devices.example.com\n"                                                     \
    "ca_file = ca.pem\n"                                                                           \
    "\n"                                                                                           \
    "[mtls]\n"                                                                                     \
    "cert_file = psp-fat.crt\n"                                                                    \
    "key_file = psp-fat.key\n"
#define HAND_WRITTEN                                                                               \
    "# My PSP\n"                                                                                   \
    "[server]\n"                                                                                   \
    "url = http://old.example:8080   \n"                                                           \
    "; the CA from my router\n"                                                                    \
    "ca_file=ca.pem\n"                                                                             \
    "future_key = kept\n"                                                                          \
    "\n"                                                                                           \
    "[unknown]\n"                                                                                  \
    "x = y\n"
#define TEXT_BUFFER (SKIFF_CONFIG_TEXT_MAX + 1)

static skiff_config config;
static skiff_config_issue issue;
static char edited[TEXT_BUFFER];
static size_t edited_length;

static char dir[TEMP_DIR_PATH_MAX];
static char path[TEMP_DIR_PATH_MAX];
static char new_path[TEMP_DIR_PATH_MAX + sizeof SKIFF_CONFIG_NEW_SUFFIX];
static char draft_path[TEMP_DIR_PATH_MAX + sizeof SKIFF_CONFIG_DRAFT_SUFFIX];
static skiff_storage *posix;
static fake_storage storage;
static char loaded[TEXT_BUFFER];
static size_t loaded_length;

void setUp(void) {
    memset(&config, 0xAB, sizeof config);
    memset(&issue, 0xAB, sizeof issue);
    memset(edited, 0, sizeof edited);
    edited_length = 0;
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    TEST_ASSERT_TRUE(temp_dir_path(dir, SKIFF_CONFIG_FILE_NAME, path, sizeof path));
    snprintf(new_path, sizeof new_path, "%s" SKIFF_CONFIG_NEW_SUFFIX, path);
    snprintf(draft_path, sizeof draft_path, "%s" SKIFF_CONFIG_DRAFT_SUFFIX, path);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&posix));
    fake_storage_init(&storage, posix);
    memset(loaded, 0, sizeof loaded);
    loaded_length = 0;
}

void tearDown(void) {
    skiff_storage_destroy(&storage.base);
    skiff_storage_destroy(posix);
    temp_dir_remove(dir);
}

static skiff_err parse(const char *text) {
    const skiff_err err = skiff_config_parse(text, strlen(text), &config, &issue);
    TEST_PRINTF("%s at line %d [%s] %s", skiff_err_name(err), issue.line, issue.section, issue.key);
    return err;
}

static void assert_refused(const char *text, skiff_err expected, int line, const char *section,
                           const char *key) {
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(expected), skiff_err_name(parse(text)));
    TEST_ASSERT_EQUAL_INT(line, issue.line);
    TEST_ASSERT_EQUAL_STRING(section, issue.section);
    TEST_ASSERT_EQUAL_STRING(key, issue.key);
    TEST_ASSERT_EQUAL_STRING("", config.server_url);
    TEST_ASSERT_EQUAL_size_t(0, config.header_count);
}

static void set(const char *text, const char *section, const char *key, const char *value) {
    const skiff_err err = skiff_config_set(text, strlen(text), section, key, value, edited,
                                           sizeof edited, &edited_length, &issue);
    TEST_PRINTF("set [%s] %s = '%s' -> %s:\n%s", section, key, value, skiff_err_name(err), edited);
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_OK), skiff_err_name(err));
    TEST_ASSERT_EQUAL_size_t(strlen(edited), edited_length);
}

static void write_file(const char *file, const char *text) {
    FILE *out = fopen(file, "wb");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), out));
    TEST_ASSERT_EQUAL_INT(0, fclose(out));
}

static int file_exists(const char *file) {
    FILE *in = fopen(file, "rb");
    if (in != NULL) {
        fclose(in);
    }
    return in != NULL;
}

/* Appends to a NUL-terminated buffer of size bytes, failing the test when it does not fit. */
static void append(char *text, size_t size, const char *more) {
    const size_t used = strlen(text);
    const int written = snprintf(text + used, size - used, "%s", more);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < size - used);
}

static skiff_err load(void) {
    const skiff_err err =
        skiff_config_load(&storage.base, path, loaded, sizeof loaded, &loaded_length);
    TEST_PRINTF("load -> %s, %zu bytes", skiff_err_name(err), loaded_length);
    return err;
}

/* ---- Parsing ---- */

static void test_the_guides_examples_parse_as_written(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(GUIDE_TOKEN_EXAMPLE));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.20:8080", config.server_url);
    TEST_ASSERT_EQUAL_STRING("rmm_0123456789abcdef", config.token);

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(GUIDE_HEADERS_EXAMPLE));
    TEST_ASSERT_EQUAL_size_t(2, config.header_count);
    TEST_ASSERT_EQUAL_STRING("CF-Access-Client-Id", config.headers[0].name);
    TEST_ASSERT_EQUAL_STRING("0123abcd.access", config.headers[0].value);
    TEST_PRINTF("a '#' and '=' inside a value are part of it, not a comment");
    TEST_ASSERT_EQUAL_STRING("s3cr#t/with=signs", config.headers[1].value);

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(GUIDE_MTLS_EXAMPLE));
    TEST_ASSERT_EQUAL_STRING("https://romm-devices.example.com", config.server_url);
    TEST_ASSERT_EQUAL_STRING("ca.pem", config.ca_file);
    TEST_ASSERT_EQUAL_STRING("psp-fat.crt", config.cert_file);
    TEST_ASSERT_EQUAL_STRING("psp-fat.key", config.key_file);
    TEST_ASSERT_EQUAL_INT(0, config.unknown_count);
}

static void test_an_empty_file_is_an_empty_config(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_parse(NULL, 0, &config, &issue));
    TEST_ASSERT_EQUAL_STRING("", config.server_url);
    TEST_ASSERT_EQUAL_size_t(0, config.header_count);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse("\n# only a comment\n\n"));
}

static void test_a_notepad_file_with_bom_and_crlf_parses(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse("\xEF\xBB\xBF[Server]\r\nURL = https://romm.lan\r\n"
                                          "\r\n[AUTH]\r\n\tToken=rmm_abc\t\r\n"));
    TEST_PRINTF("names ignore case, values lose blanks and the CR");
    TEST_ASSERT_EQUAL_STRING("https://romm.lan", config.server_url);
    TEST_ASSERT_EQUAL_STRING("rmm_abc", config.token);
}

static void test_the_last_line_needs_no_newline(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse("[auth]\ntoken = rmm_last"));
    TEST_ASSERT_EQUAL_STRING("rmm_last", config.token);
}

static void test_version_1_and_no_version_are_accepted(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse("[skiff]\nversion = 1\n"));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse("[skiff]\nversion = 01\n"));
}

static void test_unknown_keys_are_ignored_and_the_first_is_kept(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse("[server]\nurl = https://a.lan\nca-file = ca.pem\n"
                                          "[later]\nfeature = on\norphan_is_fine = 1\n"));
    TEST_ASSERT_EQUAL_INT(3, config.unknown_count);
    TEST_ASSERT_EQUAL_INT(3, config.first_unknown.line);
    TEST_ASSERT_EQUAL_STRING("server", config.first_unknown.section);
    TEST_ASSERT_EQUAL_STRING("ca-file", config.first_unknown.key);
    TEST_ASSERT_EQUAL_STRING("https://a.lan", config.server_url);
    TEST_ASSERT_EQUAL_STRING("", config.ca_file);
}

static void test_a_key_before_any_section_is_unknown(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse("url = https://a.lan\n"));
    TEST_ASSERT_EQUAL_INT(1, config.unknown_count);
    TEST_ASSERT_EQUAL_STRING("", config.first_unknown.section);
    TEST_ASSERT_EQUAL_STRING("", config.server_url);
}

static void test_empty_values_mean_not_set(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse("[server]\nurl =\nca_file = \n[auth]\ntoken=\n"));
    TEST_ASSERT_EQUAL_STRING("", config.server_url);
    TEST_ASSERT_EQUAL_STRING("", config.token);
}

static void test_damaged_lines_are_refused_with_their_line(void) {
    assert_refused("[server]\nurl https://a.lan\n", SKIFF_ERR_CONFIG_PARSE, 2, "server", "");
    assert_refused("\n[server\n", SKIFF_ERR_CONFIG_PARSE, 2, "", "");
    assert_refused("[ ]\n", SKIFF_ERR_CONFIG_PARSE, 1, "", "");
    assert_refused("[]\n", SKIFF_ERR_CONFIG_PARSE, 1, "", "");
    assert_refused("[\n", SKIFF_ERR_CONFIG_PARSE, 1, "", "");
    assert_refused("[auth]\n = rmm_x\n", SKIFF_ERR_CONFIG_PARSE, 2, "auth", "");
}

static void test_a_section_name_too_long_is_damage(void) {
    char text[SKIFF_CONFIG_NAME_MAX + 8];
    memset(text, 's', sizeof text);
    text[0] = '[';
    text[SKIFF_CONFIG_NAME_MAX + 1] = ']';
    text[SKIFF_CONFIG_NAME_MAX + 2] = '\0';
    assert_refused(text, SKIFF_ERR_CONFIG_PARSE, 1, "", "");
    text[SKIFF_CONFIG_NAME_MAX] = ']';
    text[SKIFF_CONFIG_NAME_MAX + 1] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(text));
}

static void test_a_nul_byte_or_an_oversized_file_is_damage(void) {
    static const char with_nul[] = "[auth]\ntoken = rmm\0x\n";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_PARSE,
                          skiff_config_parse(with_nul, sizeof with_nul - 1, &config, &issue));
    TEST_ASSERT_EQUAL_INT(0, issue.line);
    static char big[SKIFF_CONFIG_TEXT_MAX + 1];
    memset(big, '\n', sizeof big);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_PARSE,
                          skiff_config_parse(big, sizeof big, &config, &issue));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_parse(big, sizeof big - 1, &config, &issue));
}

static void test_a_setting_given_twice_is_refused(void) {
    assert_refused("[server]\nurl = https://a.lan\n[auth]\ntoken = x\n[SERVER]\nUrl = https://b\n",
                   SKIFF_ERR_CONFIG_INVALID_VALUE, 6, "SERVER", "Url");
    assert_refused("[headers]\nX-A = 1\nx-a = 2\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 3, "headers",
                   "x-a");
}

static void test_bad_urls_are_refused(void) {
    assert_refused("[server]\nurl = 192.168.1.20:8080\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 2,
                   "server", "url");
    assert_refused("[server]\nurl = ftp://a.lan\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "server",
                   "url");
    assert_refused("[server]\nurl = https://a lan\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "server",
                   "url");
    assert_refused("[server]\nurl = https://caf\xC3\xA9.lan\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 2,
                   "server", "url");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse("[server]\nurl = HTTPS://A.LAN\n"));
}

static void test_values_at_their_limit_pass_and_one_more_is_refused(void) {
    char text[SKIFF_CONFIG_URL_MAX + 64];
    const char *prefix = "[server]\nurl = https://";
    const size_t host = SKIFF_CONFIG_URL_MAX - 1 - strlen("https://");
    snprintf(text, sizeof text, "%s", prefix);
    memset(text + strlen(prefix), 'a', host);
    text[strlen(prefix) + host] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(text));
    TEST_ASSERT_EQUAL_size_t(SKIFF_CONFIG_URL_MAX - 1, strlen(config.server_url));
    append(text, sizeof text, "a");
    assert_refused(text, SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "server", "url");

    char token[SKIFF_CONFIG_TOKEN_MAX + 32];
    snprintf(token, sizeof token, "[auth]\ntoken = ");
    const size_t start = strlen(token);
    memset(token + start, 't', SKIFF_CONFIG_TOKEN_MAX);
    token[start + SKIFF_CONFIG_TOKEN_MAX] = '\0';
    assert_refused(token, SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "auth", "token");
    token[start + SKIFF_CONFIG_TOKEN_MAX - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(token));
}

static void test_tokens_with_blanks_or_controls_are_refused(void) {
    assert_refused("[auth]\ntoken = rmm abc\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "auth", "token");
    assert_refused("[auth]\ntoken = rmm\x01\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "auth", "token");
}

static void test_file_names_must_stay_in_the_skiff_folder(void) {
    const char *refused[] = {"certs/ca.pem", "..\\ca.pem", "ms0:/ca.pem", "..", "."};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        char text[96];
        snprintf(text, sizeof text, "[server]\nca_file = %s\n", refused[i]);
        assert_refused(text, SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "server", "ca_file");
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse("[server]\nca_file = my..ca file.pem\n"));
    TEST_ASSERT_EQUAL_STRING("my..ca file.pem", config.ca_file);
}

static void test_a_client_certificate_needs_its_key(void) {
    assert_refused("[mtls]\ncert_file = psp.crt\n", SKIFF_ERR_CONFIG_MISSING_KEY, 0, "mtls",
                   "key_file");
    assert_refused("[mtls]\nkey_file = psp.key\n", SKIFF_ERR_CONFIG_MISSING_KEY, 0, "mtls",
                   "cert_file");
}

static void test_newer_schema_versions_are_refused(void) {
    assert_refused("[skiff]\nversion = 2\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "skiff", "version");
    assert_refused("[skiff]\nversion = one\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "skiff",
                   "version");
    assert_refused("[skiff]\nversion =\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "skiff", "version");
    assert_refused("[skiff]\nversion = 99999999999999999999999\n", SKIFF_ERR_CONFIG_INVALID_VALUE,
                   2, "skiff", "version");
}

static void test_headers_skiff_sets_itself_are_refused(void) {
    const char *refused[] = {"Authorization", "host", "Range", "IF-RANGE"};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        char text[96];
        snprintf(text, sizeof text, "[headers]\n%s = x\n", refused[i]);
        assert_refused(text, SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "headers", refused[i]);
    }
}

static void test_bad_header_names_and_values_are_refused(void) {
    assert_refused("[headers]\nX Bad = 1\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "headers", "X Bad");
    assert_refused("[headers]\nX-Empty =\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "headers",
                   "X-Empty");
    assert_refused("[headers]\nX-Ctl = a\x01\n", SKIFF_ERR_CONFIG_INVALID_VALUE, 2, "headers",
                   "X-Ctl");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse("[headers]\nX-Tab = a\tb \xC3\xA9\n"));
    TEST_ASSERT_EQUAL_STRING("a\tb \xC3\xA9", config.headers[0].value);
}

static void test_at_most_eight_headers(void) {
    char text[512] = "[headers]\n";
    for (int i = 0; i < SKIFF_CONFIG_HEADERS_MAX; i++) {
        char line[32];
        snprintf(line, sizeof line, "X-H%d = v%d\n", i, i);
        append(text, sizeof text, line);
    }
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(text));
    TEST_ASSERT_EQUAL_size_t(SKIFF_CONFIG_HEADERS_MAX, config.header_count);
    append(text, sizeof text, "X-One-Too-Many = v\n");
    assert_refused(text, SKIFF_ERR_CONFIG_INVALID_VALUE, SKIFF_CONFIG_HEADERS_MAX + 2, "headers",
                   "X-One-Too-Many");
}

static void test_header_views_feed_the_transport(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(GUIDE_HEADERS_EXAMPLE));
    skiff_http_header views[SKIFF_CONFIG_HEADERS_MAX];
    TEST_ASSERT_EQUAL_size_t(2, skiff_config_headers(&config, views, SKIFF_CONFIG_HEADERS_MAX));
    TEST_ASSERT_TRUE(skiff_http_headers_valid(views, 2));
    TEST_ASSERT_EQUAL_PTR(config.headers[1].value, views[1].value);
    TEST_ASSERT_EQUAL_size_t(1, skiff_config_headers(&config, views, 1));
    TEST_ASSERT_EQUAL_size_t(0, skiff_config_headers(NULL, views, 1));
    TEST_ASSERT_EQUAL_size_t(0, skiff_config_headers(&config, NULL, 1));
}

static void test_null_arguments_are_refused(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_config_parse("x", 1, NULL, &issue));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_config_parse(NULL, 1, &config, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_parse("[a]\n", 4, &config, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_PARSE, skiff_config_parse("x\n", 2, &config, NULL));
}

/* ---- Editing ---- */

static void test_setting_an_existing_key_changes_only_its_value(void) {
    set(HAND_WRITTEN, "server", "url", "https://new.example");
    TEST_ASSERT_EQUAL_STRING("# My PSP\n"
                             "[server]\n"
                             "url = https://new.example   \n"
                             "; the CA from my router\n"
                             "ca_file=ca.pem\n"
                             "future_key = kept\n"
                             "\n"
                             "[unknown]\n"
                             "x = y\n",
                             edited);
    TEST_PRINTF("a key written without spaces keeps that style");
    set(HAND_WRITTEN, "SERVER", "CA_FILE", "mine.pem");
    TEST_ASSERT_NOT_NULL(strstr(edited, "\nca_file=mine.pem\nfuture_key = kept\n"));
}

static void test_clearing_a_value_leaves_an_unset_key(void) {
    set(GUIDE_TOKEN_EXAMPLE, "auth", "token", "");
    TEST_ASSERT_NOT_NULL(strstr(edited, "[auth]\ntoken =\n"));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(edited));
    TEST_ASSERT_EQUAL_STRING("", config.token);
    set("[auth]\ntoken =\n", "auth", "token", "rmm_new");
    TEST_ASSERT_EQUAL_STRING("[auth]\ntoken = rmm_new\n", edited);
}

static void test_a_missing_key_joins_its_section(void) {
    set(HAND_WRITTEN, "server", "token_hint", "v");
    TEST_PRINTF("added after the section's last setting, before the blank line");
    TEST_ASSERT_NOT_NULL(strstr(edited, "future_key = kept\ntoken_hint = v\n\n[unknown]\n"));
    set("[auth]\n# nothing yet\n\n[server]\nurl = https://a\n", "auth", "token", "rmm_t");
    TEST_ASSERT_EQUAL_STRING("[auth]\ntoken = rmm_t\n# nothing yet\n\n[server]\nurl = https://a\n",
                             edited);
}

static void test_a_missing_section_goes_at_the_end(void) {
    set(GUIDE_MTLS_EXAMPLE, "auth", "token", "rmm_paired");
    TEST_ASSERT_EQUAL_INT(0, strncmp(edited, GUIDE_MTLS_EXAMPLE, strlen(GUIDE_MTLS_EXAMPLE)));
    TEST_ASSERT_EQUAL_STRING("\n[auth]\ntoken = rmm_paired\n", edited + strlen(GUIDE_MTLS_EXAMPLE));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(edited));
    TEST_ASSERT_EQUAL_STRING("rmm_paired", config.token);
    TEST_ASSERT_EQUAL_STRING("psp-fat.key", config.key_file);
}

static void test_editing_an_empty_or_unterminated_file(void) {
    set("", "server", "url", "https://first.lan");
    TEST_ASSERT_EQUAL_STRING("[server]\nurl = https://first.lan\n", edited);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_set(NULL, 0, "auth", "token", "t", edited,
                                                     sizeof edited, &edited_length, NULL));
    TEST_ASSERT_EQUAL_STRING("[auth]\ntoken = t\n", edited);
    set("[server]\nurl = https://a", "auth", "token", "t");
    TEST_ASSERT_EQUAL_STRING("[server]\nurl = https://a\n\n[auth]\ntoken = t\n", edited);
    set("[server]\nurl = https://a", "server", "ca_file", "ca.pem");
    TEST_ASSERT_EQUAL_STRING("[server]\nurl = https://a\nca_file = ca.pem\n", edited);
    set("[auth]", "auth", "token", "t");
    TEST_ASSERT_EQUAL_STRING("[auth]\ntoken = t\n", edited);
}

static void test_a_notepad_file_is_edited_past_its_bom(void) {
    set("\xEF\xBB\xBF[server]\r\nurl = https://a\r\n", "server", "url", "https://b");
    TEST_ASSERT_EQUAL_STRING("\xEF\xBB\xBF[server]\r\nurl = https://b\r\n", edited);
    set("\xEF\xBB\xBF[server]\r\n", "server", "url", "https://b");
    TEST_ASSERT_EQUAL_STRING("\xEF\xBB\xBF[server]\r\nurl = https://b\r\n", edited);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(edited));
}

static void test_a_windows_file_keeps_crlf(void) {
    set("\xEF\xBB\xBF[server]\r\nurl = https://a\r\n", "auth", "token", "t");
    TEST_ASSERT_EQUAL_STRING(
        "\xEF\xBB\xBF[server]\r\nurl = https://a\r\n\r\n[auth]\r\ntoken = t\r\n", edited);
    set("[server]\r\nurl = https://a\r\n", "server", "url", "https://b");
    TEST_ASSERT_EQUAL_STRING("[server]\r\nurl = https://b\r\n", edited);
}

static void test_only_the_first_instance_of_a_section_grows(void) {
    set("[server]\nurl = https://a\n[auth]\ntoken = t\n[server]\nlater = 1\n", "server", "ca_file",
        "ca.pem");
    TEST_ASSERT_EQUAL_STRING(
        "[server]\nurl = https://a\nca_file = ca.pem\n[auth]\ntoken = t\n[server]\nlater = 1\n",
        edited);
    set("[server]\nurl = https://a\n[auth]\n[server]\nca_file = x\n", "server", "ca_file", "y");
    TEST_ASSERT_EQUAL_STRING("[server]\nurl = https://a\n[auth]\n[server]\nca_file = y\n", edited);
}

static void test_names_and_values_that_would_not_parse_back_are_refused(void) {
    const char *bad_names[] = {"", " url", "url ", "a=b", "[x", "x]", "#c", ";c", "a\nb"};
    for (size_t i = 0; i < sizeof bad_names / sizeof bad_names[0]; i++) {
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                              skiff_config_set("", 0, "server", bad_names[i], "v", edited,
                                               sizeof edited, &edited_length, NULL));
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                              skiff_config_set("", 0, bad_names[i], "url", "v", edited,
                                               sizeof edited, &edited_length, NULL));
    }
    const char *bad_values[] = {"a\nb", "a\rb", " a", "a\t"};
    for (size_t i = 0; i < sizeof bad_values / sizeof bad_values[0]; i++) {
        TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                              skiff_config_set("", 0, "auth", "token", bad_values[i], edited,
                                               sizeof edited, &edited_length, NULL));
    }
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_config_set(NULL, 1, "a", "b", "c", edited, sizeof edited, &edited_length, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_config_set("", 0, "a", "b", "c", edited, 0, &edited_length, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_config_set("", 0, "a", "b", NULL, edited, 8, &edited_length, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_config_set("", 0, "a", "b", "c", edited, 8, NULL, NULL));
}

/* Calls skiff_config_set() expecting a refusal from the parser, which names the setting. */
static void assert_set_refused(const char *text, const char *section, const char *key,
                               const char *value, skiff_err expected, int line) {
    const skiff_err err = skiff_config_set(text, strlen(text), section, key, value, edited,
                                           sizeof edited, &edited_length, &issue);
    TEST_PRINTF("set [%s] %s = '%s' -> %s at line %d [%s] %s", section, key, value,
                skiff_err_name(err), issue.line, issue.section, issue.key);
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(expected), skiff_err_name(err));
    TEST_ASSERT_EQUAL_INT(line, issue.line);
    TEST_ASSERT_EQUAL_STRING("", edited);
}

static void test_an_edit_that_would_not_load_is_refused(void) {
    TEST_PRINTF("values the parser refuses never reach a saved file");
    assert_set_refused(GUIDE_TOKEN_EXAMPLE, "server", "url", "ftp://host",
                       SKIFF_ERR_CONFIG_INVALID_VALUE, 2);
    TEST_ASSERT_EQUAL_STRING("url", issue.key);
    assert_set_refused(GUIDE_TOKEN_EXAMPLE, "auth", "token", "rmm two words",
                       SKIFF_ERR_CONFIG_INVALID_VALUE, 5);
    assert_set_refused("", "headers", "X Bad", "v", SKIFF_ERR_CONFIG_INVALID_VALUE, 2);
    assert_set_refused("", "headers", "Authorization", "Bearer x", SKIFF_ERR_CONFIG_INVALID_VALUE,
                       2);
    assert_set_refused("", "mtls", "cert_file", "psp.crt", SKIFF_ERR_CONFIG_MISSING_KEY, 0);
    TEST_ASSERT_EQUAL_STRING("key_file", issue.key);
    TEST_PRINTF("a damaged line elsewhere is reported, not saved again");
    assert_set_refused("[server]\nnot a setting\n", "auth", "token", "t", SKIFF_ERR_CONFIG_PARSE,
                       2);
    TEST_PRINTF("and an edit that repairs the file is accepted");
    set("[server]\nurl = 192.168.1.20\n", "server", "url", "http://192.168.1.20");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse(edited));
}

static void test_an_edit_into_its_own_buffer_is_refused(void) {
    char text[64] = "[auth]\ntoken = old\n";
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_config_set(text, strlen(text), "auth", "token", "a-longer-value",
                                           text, sizeof text, &length, NULL));
    TEST_ASSERT_EQUAL_STRING("[auth]\ntoken = old\n", text);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_config_set(text + 8, 4, "auth", "token", "v",
                                                                  text, 10, &length, NULL));
    TEST_PRINTF(
        "nor may the value, key or section live in out: out is cleared before they are read");
    char out[64] = "https://new.example";
    const char *base = "[server]\nurl = https://a\n";
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_INVALID_ARG,
        skiff_config_set(base, strlen(base), "server", "url", out, out, sizeof out, &length, NULL));
    snprintf(out, sizeof out, "url");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_config_set(base, strlen(base), "server", out, "https://b", out,
                                           sizeof out, &length, NULL));
    snprintf(out, sizeof out, "server");
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_config_set(base, strlen(base), out, "url", "https://b", out,
                                           sizeof out, &length, NULL));
    TEST_ASSERT_EQUAL_STRING("server", out);
}

static void test_an_edit_that_does_not_fit_is_refused(void) {
    const char *text = "[auth]\ntoken = old\n";
    const char *expected = "[auth]\ntoken = new\n";
    char small[32];
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_config_set(text, strlen(text), "auth", "token", "new", small,
                                           strlen(expected), &length, NULL));
    TEST_ASSERT_EQUAL_STRING("", small);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_set(text, strlen(text), "auth", "token", "new",
                                                     small, strlen(expected) + 1, &length, NULL));
    TEST_ASSERT_EQUAL_STRING(expected, small);
}

/* ---- Loading and saving ---- */

static void test_no_file_loads_as_empty(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_size_t(0, loaded_length);
    TEST_ASSERT_EQUAL_STRING("", loaded);
}

static void test_save_then_load_round_trips(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_save(&storage.base, path, GUIDE_MTLS_EXAMPLE,
                                                      strlen(GUIDE_MTLS_EXAMPLE)));
    TEST_PRINTF("synced: the draft's bytes, then the device after each rename");
    TEST_ASSERT_EQUAL_INT(3, storage.syncs);
    TEST_ASSERT_FALSE(file_exists(new_path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_STRING(GUIDE_MTLS_EXAMPLE, loaded);
    TEST_PRINTF("saving again replaces the file (FAT: remove, then rename)");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_save(&storage.base, path, GUIDE_TOKEN_EXAMPLE,
                                                      strlen(GUIDE_TOKEN_EXAMPLE)));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_STRING(GUIDE_TOKEN_EXAMPLE, loaded);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_save(&storage.base, path, NULL, 0));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_size_t(0, loaded_length);
}

static void test_a_failed_write_or_sync_leaves_the_old_file(void) {
    write_file(path, GUIDE_TOKEN_EXAMPLE);
    storage.fail_suffix = SKIFF_CONFIG_DRAFT_SUFFIX;
    storage.write_budget = 4;
    storage.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_STORAGE_NO_SPACE,
        skiff_config_save(&storage.base, path, GUIDE_MTLS_EXAMPLE, strlen(GUIDE_MTLS_EXAMPLE)));
    TEST_ASSERT_FALSE(file_exists(draft_path));
    TEST_ASSERT_FALSE(file_exists(new_path));
    storage.write_budget = 0;
    storage.write_error = SKIFF_OK;
    storage.sync_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_STORAGE_IO,
        skiff_config_save(&storage.base, path, GUIDE_MTLS_EXAMPLE, strlen(GUIDE_MTLS_EXAMPLE)));
    TEST_ASSERT_FALSE(file_exists(draft_path));
    storage.sync_error = SKIFF_OK;
    storage.rename_error = SKIFF_ERR_STORAGE_IO;
    TEST_PRINTF("a draft that never became .new is dropped");
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_STORAGE_IO,
        skiff_config_save(&storage.base, path, GUIDE_MTLS_EXAMPLE, strlen(GUIDE_MTLS_EXAMPLE)));
    TEST_ASSERT_FALSE(file_exists(draft_path));
    TEST_ASSERT_FALSE(file_exists(new_path));
    storage.rename_error = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_STRING(GUIDE_TOKEN_EXAMPLE, loaded);
}

static void test_a_save_cut_before_the_last_rename_is_finished_by_load(void) {
    write_file(path, GUIDE_TOKEN_EXAMPLE);
    storage.fail_suffix = SKIFF_CONFIG_NEW_SUFFIX;
    storage.rename_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_STORAGE_IO,
        skiff_config_save(&storage.base, path, GUIDE_MTLS_EXAMPLE, strlen(GUIDE_MTLS_EXAMPLE)));
    TEST_PRINTF("as after a power cut: config.ini gone, the complete .new file left");
    TEST_ASSERT_FALSE(file_exists(path));
    TEST_ASSERT_TRUE(file_exists(new_path));
    storage.rename_error = SKIFF_OK;
    const int syncs = storage.syncs;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_STRING(GUIDE_MTLS_EXAMPLE, loaded);
    TEST_ASSERT_FALSE(file_exists(new_path));
    TEST_PRINTF("the recovery rename is synced to the device too");
    TEST_ASSERT_EQUAL_INT(syncs + 1, storage.syncs);
}

static void test_config_ini_stays_until_the_new_name_is_flushed(void) {
    write_file(path, GUIDE_TOKEN_EXAMPLE);
    storage.fail_suffix = SKIFF_CONFIG_NEW_SUFFIX;
    storage.sync_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_STORAGE_IO,
        skiff_config_save(&storage.base, path, GUIDE_MTLS_EXAMPLE, strlen(GUIDE_MTLS_EXAMPLE)));
    storage.sync_error = SKIFF_OK;
    TEST_ASSERT_TRUE(file_exists(path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_STRING(GUIDE_TOKEN_EXAMPLE, loaded);
}

static void test_a_save_finishes_an_earlier_cut_save_first(void) {
    TEST_PRINTF("only a complete .new file holds the settings; the next save must not lose it");
    write_file(new_path, GUIDE_MTLS_EXAMPLE);
    storage.fail_suffix = SKIFF_CONFIG_DRAFT_SUFFIX;
    storage.sync_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_STORAGE_IO,
        skiff_config_save(&storage.base, path, GUIDE_TOKEN_EXAMPLE, strlen(GUIDE_TOKEN_EXAMPLE)));
    storage.sync_error = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_STRING(GUIDE_MTLS_EXAMPLE, loaded);
}

static void test_a_cut_draft_is_never_used(void) {
    TEST_PRINTF("first save cut mid-write: no config.ini, only a partial draft");
    write_file(draft_path, "[server]\nurl = https://half");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_size_t(0, loaded_length);
    TEST_ASSERT_FALSE(file_exists(draft_path));
    TEST_ASSERT_FALSE(file_exists(path));
}

static void test_a_failed_remove_keeps_the_complete_new_file(void) {
    write_file(path, GUIDE_TOKEN_EXAMPLE);
    TEST_PRINTF("config.ini is gone although its remove reported an error");
    storage.fail_suffix = SKIFF_CONFIG_FILE_NAME;
    storage.remove_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_STORAGE_IO,
        skiff_config_save(&storage.base, path, GUIDE_MTLS_EXAMPLE, strlen(GUIDE_MTLS_EXAMPLE)));
    storage.remove_error = SKIFF_OK;
    TEST_ASSERT_FALSE(file_exists(path));
    TEST_ASSERT_TRUE(file_exists(new_path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_STRING(GUIDE_MTLS_EXAMPLE, loaded);
}

static void test_a_leftover_new_file_beside_the_old_one_is_dropped(void) {
    write_file(path, GUIDE_TOKEN_EXAMPLE);
    write_file(new_path, "[server]\nurl = https://other");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_STRING(GUIDE_TOKEN_EXAMPLE, loaded);
    TEST_ASSERT_FALSE(file_exists(new_path));
    TEST_PRINTF("and a stale .new or .tmp file never blocks the next save");
    write_file(new_path, "stale");
    write_file(draft_path, "stale");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_save(&storage.base, path, GUIDE_MTLS_EXAMPLE,
                                                      strlen(GUIDE_MTLS_EXAMPLE)));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_STRING(GUIDE_MTLS_EXAMPLE, loaded);
}

static void test_an_undeletable_new_file_does_not_block_loading(void) {
    write_file(path, GUIDE_TOKEN_EXAMPLE);
    TEST_PRINTF("a folder named config.ini.new cannot be removed as a file");
    TEST_ASSERT_EQUAL_INT(0, mkdir(new_path, S_IRWXU));
    const skiff_err err = load();
    TEST_ASSERT_EQUAL_INT(0, rmdir(new_path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, err);
    TEST_ASSERT_EQUAL_STRING(GUIDE_TOKEN_EXAMPLE, loaded);
}

static void test_a_file_too_large_is_damage(void) {
    static char big[SKIFF_CONFIG_TEXT_MAX + 2];
    memset(big, '#', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    write_file(path, big);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_PARSE, load());
    TEST_ASSERT_EQUAL_STRING("", loaded);
    big[SKIFF_CONFIG_TEXT_MAX] = '\0';
    write_file(path, big);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, load());
    TEST_ASSERT_EQUAL_size_t(SKIFF_CONFIG_TEXT_MAX, loaded_length);
}

static void test_a_buffer_too_small_for_the_file_reports_damage(void) {
    write_file(path, GUIDE_TOKEN_EXAMPLE);
    char tiny[4];
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_CONFIG_PARSE,
                          skiff_config_load(&storage.base, path, tiny, sizeof tiny, &length));
    TEST_ASSERT_EQUAL_STRING("", tiny);
    TEST_ASSERT_EQUAL_size_t(0, length);
}

static void test_load_and_save_refuse_bad_arguments(void) {
    char long_path[SKIFF_CONFIG_PATH_MAX];
    memset(long_path, 'p', sizeof long_path - 1);
    long_path[sizeof long_path - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_config_load(&storage.base, long_path, loaded,
                                                                   sizeof loaded, &loaded_length));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_config_load(NULL, path, loaded, sizeof loaded, &loaded_length));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_config_load(&storage.base, path, loaded, 0, &loaded_length));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_config_load(&storage.base, path, loaded, sizeof loaded, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_config_save(&storage.base, long_path, "x", 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_config_save(NULL, path, "x", 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_config_save(&storage.base, path, NULL, 1));
    TEST_PRINTF("a text the next load would refuse is never written");
    static char big[SKIFF_CONFIG_TEXT_MAX + 1];
    memset(big, '#', sizeof big);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_config_save(&storage.base, path, big, sizeof big));
    TEST_ASSERT_FALSE(file_exists(path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_config_save(&storage.base, path, big, sizeof big - 1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_guides_examples_parse_as_written);
    RUN_TEST(test_an_empty_file_is_an_empty_config);
    RUN_TEST(test_a_notepad_file_with_bom_and_crlf_parses);
    RUN_TEST(test_the_last_line_needs_no_newline);
    RUN_TEST(test_version_1_and_no_version_are_accepted);
    RUN_TEST(test_unknown_keys_are_ignored_and_the_first_is_kept);
    RUN_TEST(test_a_key_before_any_section_is_unknown);
    RUN_TEST(test_empty_values_mean_not_set);
    RUN_TEST(test_damaged_lines_are_refused_with_their_line);
    RUN_TEST(test_a_section_name_too_long_is_damage);
    RUN_TEST(test_a_nul_byte_or_an_oversized_file_is_damage);
    RUN_TEST(test_a_setting_given_twice_is_refused);
    RUN_TEST(test_bad_urls_are_refused);
    RUN_TEST(test_values_at_their_limit_pass_and_one_more_is_refused);
    RUN_TEST(test_tokens_with_blanks_or_controls_are_refused);
    RUN_TEST(test_file_names_must_stay_in_the_skiff_folder);
    RUN_TEST(test_a_client_certificate_needs_its_key);
    RUN_TEST(test_newer_schema_versions_are_refused);
    RUN_TEST(test_headers_skiff_sets_itself_are_refused);
    RUN_TEST(test_bad_header_names_and_values_are_refused);
    RUN_TEST(test_at_most_eight_headers);
    RUN_TEST(test_header_views_feed_the_transport);
    RUN_TEST(test_null_arguments_are_refused);
    RUN_TEST(test_setting_an_existing_key_changes_only_its_value);
    RUN_TEST(test_clearing_a_value_leaves_an_unset_key);
    RUN_TEST(test_a_missing_key_joins_its_section);
    RUN_TEST(test_a_missing_section_goes_at_the_end);
    RUN_TEST(test_editing_an_empty_or_unterminated_file);
    RUN_TEST(test_a_notepad_file_is_edited_past_its_bom);
    RUN_TEST(test_a_windows_file_keeps_crlf);
    RUN_TEST(test_only_the_first_instance_of_a_section_grows);
    RUN_TEST(test_names_and_values_that_would_not_parse_back_are_refused);
    RUN_TEST(test_an_edit_that_would_not_load_is_refused);
    RUN_TEST(test_an_edit_into_its_own_buffer_is_refused);
    RUN_TEST(test_an_edit_that_does_not_fit_is_refused);
    RUN_TEST(test_no_file_loads_as_empty);
    RUN_TEST(test_save_then_load_round_trips);
    RUN_TEST(test_a_failed_write_or_sync_leaves_the_old_file);
    RUN_TEST(test_a_save_cut_before_the_last_rename_is_finished_by_load);
    RUN_TEST(test_config_ini_stays_until_the_new_name_is_flushed);
    RUN_TEST(test_a_save_finishes_an_earlier_cut_save_first);
    RUN_TEST(test_a_cut_draft_is_never_used);
    RUN_TEST(test_a_failed_remove_keeps_the_complete_new_file);
    RUN_TEST(test_a_leftover_new_file_beside_the_old_one_is_dropped);
    RUN_TEST(test_an_undeletable_new_file_does_not_block_loading);
    RUN_TEST(test_a_file_too_large_is_damage);
    RUN_TEST(test_a_buffer_too_small_for_the_file_reports_damage);
    RUN_TEST(test_load_and_save_refuse_bad_arguments);
    return UNITY_END();
}
