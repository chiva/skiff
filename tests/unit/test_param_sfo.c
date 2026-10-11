/*
 * The PARAM.SFO reader (skiff/param_sfo.h) on files built here byte by byte in the PSP's layout:
 * a save's (SAVEDATA_PARAMS and SAVEDATA_FILE_LIST), a game-data install's (neither), text and
 * number values, and files that lie: cut short, wrong magic or version, counts and offsets that
 * point outside the file, keys without a terminator, values past their table.
 */
#include <stdint.h>
#include <string.h>

#include "skiff/param_sfo.h"

#include "unity.h"

#define SFO_CAPACITY 1024
#define PARAMS_BYTES 128
#define FILE_LIST_BYTES 64
#define TITLE "Skiff Test Game"
#define DISC_ID "SKIF00001"
#define PARENTAL_LEVEL 5U
#define TABLE_ALIGNMENT 4
/* Offsets inside the header and the first index entry. */
#define HEADER_MAGIC 0
#define HEADER_VERSION 4
#define HEADER_KEY_TABLE 8
#define HEADER_DATA_TABLE 12
#define HEADER_COUNT 16
#define ENTRY_KEY_OFFSET 0
#define ENTRY_FORMAT 2
#define ENTRY_LENGTH 4
#define ENTRY_RESERVED 8
#define ENTRY_DATA_OFFSET 12

typedef struct sfo_field {
    const char *key;
    uint16_t format;
    const void *value;
    uint32_t length;
    uint32_t reserved;
} sfo_field;

typedef struct sfo_file {
    unsigned char bytes[SFO_CAPACITY];
    size_t size;
    uint32_t key_table;
    uint32_t data_table;
} sfo_file;

static sfo_file file;
static skiff_sfo sfo;

static void put16(unsigned char *at, uint32_t value) {
    at[0] = (unsigned char)(value & 0xFFU);
    at[1] = (unsigned char)((value >> 8) & 0xFFU);
}

static void put32(unsigned char *at, uint32_t value) {
    put16(at, value & 0xFFFFU);
    put16(at + 2, value >> 16);
}

static size_t aligned(size_t value) {
    return (value + TABLE_ALIGNMENT - 1) / TABLE_ALIGNMENT * TABLE_ALIGNMENT;
}

/* Lays fields out as the PSP's tools do: header, index, keys (padded to 4), data. */
static void build(const sfo_field *fields, size_t count) {
    memset(&file, 0, sizeof file);
    size_t keys = 0;
    for (size_t i = 0; i < count; i++) {
        keys += strlen(fields[i].key) + 1;
    }
    file.key_table = (uint32_t)(SKIFF_SFO_HEADER_BYTES + count * SKIFF_SFO_INDEX_ENTRY_BYTES);
    file.data_table = (uint32_t)aligned(file.key_table + keys);
    memcpy(file.bytes + HEADER_MAGIC, SKIFF_SFO_MAGIC, SKIFF_SFO_MAGIC_BYTES);
    put32(file.bytes + HEADER_VERSION, SKIFF_SFO_VERSION_1_1);
    put32(file.bytes + HEADER_KEY_TABLE, file.key_table);
    put32(file.bytes + HEADER_DATA_TABLE, file.data_table);
    put32(file.bytes + HEADER_COUNT, (uint32_t)count);
    uint32_t key_at = 0;
    uint32_t data_at = 0;
    for (size_t i = 0; i < count; i++) {
        unsigned char *entry =
            file.bytes + SKIFF_SFO_HEADER_BYTES + i * SKIFF_SFO_INDEX_ENTRY_BYTES;
        put16(entry + ENTRY_KEY_OFFSET, key_at);
        put16(entry + ENTRY_FORMAT, fields[i].format);
        put32(entry + ENTRY_LENGTH, fields[i].length);
        put32(entry + ENTRY_RESERVED, fields[i].reserved);
        put32(entry + ENTRY_DATA_OFFSET, data_at);
        memcpy(file.bytes + file.key_table + key_at, fields[i].key, strlen(fields[i].key) + 1);
        memcpy(file.bytes + file.data_table + data_at, fields[i].value, fields[i].length);
        key_at += (uint32_t)strlen(fields[i].key) + 1;
        data_at += fields[i].reserved;
    }
    file.size = file.data_table + data_at;
    TEST_PRINTF("built a PARAM.SFO of %zu bytes: %zu keys, key table at %u, data table at %u",
                file.size, count, file.key_table, file.data_table);
}

static unsigned char params[PARAMS_BYTES];
static unsigned char file_list[FILE_LIST_BYTES];

/* A save's PARAM.SFO as the PSP's savedata utility writes it (synthetic values). */
static void build_save(void) {
    memset(params, 0x5A, sizeof params);
    memset(file_list, 0xA5, sizeof file_list);
    unsigned char level[4];
    put32(level, PARENTAL_LEVEL);
    const sfo_field fields[] = {
        {"CATEGORY", SKIFF_SFO_FORMAT_TEXT, "MS", 3, 4},
        {"PARENTAL_LEVEL", SKIFF_SFO_FORMAT_NUMBER, level, 4, 4},
        {"SAVEDATA_DIRECTORY", SKIFF_SFO_FORMAT_TEXT, DISC_ID "DATA00", 16, 64},
        {SKIFF_SFO_KEY_SAVEDATA_FILE_LIST, SKIFF_SFO_FORMAT_BYTES, file_list, FILE_LIST_BYTES,
         FILE_LIST_BYTES},
        {SKIFF_SFO_KEY_SAVEDATA_PARAMS, SKIFF_SFO_FORMAT_BYTES, params, PARAMS_BYTES, PARAMS_BYTES},
        {"TITLE", SKIFF_SFO_FORMAT_TEXT, TITLE, sizeof TITLE, 128},
    };
    build(fields, sizeof fields / sizeof fields[0]);
}

/* What a game's data installer writes: no save keys. */
static void build_install(void) {
    const sfo_field fields[] = {
        {"CATEGORY", SKIFF_SFO_FORMAT_TEXT, "MS", 3, 4},
        {"SAVEDATA_DIRECTORY", SKIFF_SFO_FORMAT_TEXT, DISC_ID "INSTALL", 17, 64},
        {"TITLE", SKIFF_SFO_FORMAT_TEXT, TITLE, sizeof TITLE, 128},
    };
    build(fields, sizeof fields / sizeof fields[0]);
}

static unsigned char *first_entry(void) { return file.bytes + SKIFF_SFO_HEADER_BYTES; }

static skiff_err parse(void) {
    const skiff_err err = skiff_sfo_parse(file.bytes, file.size, &sfo);
    TEST_PRINTF("parse %zu bytes: %s", file.size, skiff_err_name(err));
    return err;
}

static void assert_damaged(const char *why) {
    TEST_PRINTF("%s", why);
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_STORAGE_SAVE_DAMAGED),
                             skiff_err_name(parse()));
    TEST_ASSERT_NULL(sfo.data);
    TEST_ASSERT_EQUAL_UINT32(0, sfo.count);
}

void setUp(void) { memset(&sfo, 0xAB, sizeof sfo); }

void tearDown(void) {}

static void test_a_save_reads_its_text_number_and_save_keys(void) {
    build_save();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse());
    char text[64];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_sfo_text(&sfo, "TITLE", text, sizeof text));
    TEST_PRINTF("TITLE = \"%s\"", text);
    TEST_ASSERT_EQUAL_STRING(TITLE, text);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_sfo_text(&sfo, "SAVEDATA_DIRECTORY", text, sizeof text));
    TEST_ASSERT_EQUAL_STRING(DISC_ID "DATA00", text);
    uint32_t number = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_sfo_number(&sfo, "PARENTAL_LEVEL", &number));
    TEST_PRINTF("PARENTAL_LEVEL = %u", number);
    TEST_ASSERT_EQUAL_UINT32(PARENTAL_LEVEL, number);
    skiff_sfo_value value;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_sfo_find(&sfo, SKIFF_SFO_KEY_SAVEDATA_PARAMS, &value));
    TEST_ASSERT_EQUAL_INT(SKIFF_SFO_FORMAT_BYTES, value.format);
    TEST_ASSERT_EQUAL_size_t(PARAMS_BYTES, value.length);
    TEST_ASSERT_EQUAL_MEMORY(params, value.bytes, PARAMS_BYTES);
    TEST_ASSERT_TRUE(skiff_sfo_is_save(&sfo));
}

static void test_an_install_has_neither_save_key(void) {
    build_install();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse());
    TEST_ASSERT_FALSE(skiff_sfo_is_save(&sfo));
    skiff_sfo_value value;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND,
                          skiff_sfo_find(&sfo, SKIFF_SFO_KEY_SAVEDATA_PARAMS, &value));
    TEST_ASSERT_NULL(value.bytes);
}

static void test_one_save_key_is_enough(void) {
    memset(params, 0x5A, sizeof params);
    const sfo_field fields[] = {
        {SKIFF_SFO_KEY_SAVEDATA_PARAMS, SKIFF_SFO_FORMAT_BYTES, params, PARAMS_BYTES, PARAMS_BYTES},
    };
    build(fields, 1);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse());
    TEST_ASSERT_TRUE(skiff_sfo_is_save(&sfo));
}

static void test_keys_match_exactly(void) {
    build_save();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse());
    skiff_sfo_value value;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_sfo_find(&sfo, "TITL", &value));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_sfo_find(&sfo, "title", &value));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_sfo_find(&sfo, "TITLE_", &value));
}

static void test_a_value_of_the_wrong_kind_or_too_long_is_refused(void) {
    build_save();
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse());
    char text[sizeof TITLE];
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_SAVE_DAMAGED,
                          skiff_sfo_text(&sfo, "PARENTAL_LEVEL", text, sizeof text));
    TEST_ASSERT_EQUAL_STRING("", text);
    uint32_t number = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_SAVE_DAMAGED, skiff_sfo_number(&sfo, "TITLE", &number));
    TEST_ASSERT_EQUAL_UINT32(0, number);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_BUFFER_TOO_SMALL,
                          skiff_sfo_text(&sfo, "TITLE", text, sizeof text - 1));
    TEST_ASSERT_EQUAL_STRING("", text);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_sfo_text(&sfo, "TITLE", text, sizeof text));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NOT_FOUND, skiff_sfo_number(&sfo, "MISSING", &number));
}

static void test_text_without_a_terminator_reads_its_used_bytes(void) {
    const sfo_field fields[] = {{"DISC_ID", SKIFF_SFO_FORMAT_BYTES, DISC_ID, 9, 16}};
    build(fields, 1);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse());
    char text[16];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_sfo_text(&sfo, "DISC_ID", text, sizeof text));
    TEST_ASSERT_EQUAL_STRING(DISC_ID, text);
}

static void test_a_cut_file_is_damaged_at_every_length(void) {
    build_save();
    const size_t whole = file.size;
    for (size_t size = 0; size < whole; size++) {
        if (skiff_sfo_parse(file.bytes, size, &sfo) != SKIFF_ERR_STORAGE_SAVE_DAMAGED) {
            TEST_PRINTF("a file cut to %zu of %zu bytes parsed", size, whole);
            TEST_FAIL();
        }
    }
    TEST_PRINTF("every cut from 0 to %zu bytes was refused", whole - 1);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_sfo_parse(file.bytes, whole, &sfo));
}

static void test_wrong_magic_or_version(void) {
    build_save();
    file.bytes[1] = 'X';
    assert_damaged("magic \\0XSF");
    build_save();
    put32(file.bytes + HEADER_VERSION, 0x00000102U);
    assert_damaged("version 1.2");
}

static void test_lying_counts_and_table_offsets(void) {
    build_save();
    put32(file.bytes + HEADER_COUNT, SKIFF_SFO_ENTRIES_MAX + 1);
    assert_damaged("more entries than the cap");
    build_save();
    put32(file.bytes + HEADER_COUNT, 7);
    assert_damaged("one entry more than fits before the key table");
    build_save();
    put32(file.bytes + HEADER_DATA_TABLE, file.key_table - 4);
    assert_damaged("the data table before the key table");
    build_save();
    put32(file.bytes + HEADER_DATA_TABLE, (uint32_t)file.size + 1);
    assert_damaged("the data table past the end");
    build_save();
    put32(file.bytes + HEADER_KEY_TABLE, 0xFFFFFFF0U);
    assert_damaged("the key table far past the end");
}

static void test_lying_entries(void) {
    build_save();
    put16(first_entry() + ENTRY_KEY_OFFSET, file.data_table - file.key_table);
    assert_damaged("a key starting at the data table");
    build_save();
    put16(first_entry() + ENTRY_FORMAT, 0x0104);
    assert_damaged("an unknown format");
    build_save();
    put32(first_entry() + ENTRY_LENGTH, 5);
    assert_damaged("a value using more than it reserves");
    build_save();
    put32(first_entry() + ENTRY_DATA_OFFSET, (uint32_t)(file.size - file.data_table) - 2);
    assert_damaged("a value running past the end");
    build_save();
    put32(first_entry() + ENTRY_DATA_OFFSET, 0xFFFFFFFCU);
    assert_damaged("a value offset that wraps around");
    build_save();
    unsigned char *number_entry = first_entry() + SKIFF_SFO_INDEX_ENTRY_BYTES;
    put32(number_entry + ENTRY_LENGTH, 2);
    assert_damaged("a number of 2 bytes");
}

static void test_keys_must_end_inside_the_key_table(void) {
    const sfo_field fields[] = {{"TITLE", SKIFF_SFO_FORMAT_TEXT, TITLE, sizeof TITLE, 32}};
    build(fields, 1);
    /* "TITLE\0" fills the key table up to its padding: overwrite the terminator and padding. */
    memset(file.bytes + file.key_table, 'K', file.data_table - file.key_table);
    assert_damaged("a key with no NUL before the data table");
    build(fields, 1);
    file.bytes[file.key_table] = '\0';
    assert_damaged("an empty key");
}

static void test_a_file_larger_than_the_cap_is_refused(void) {
    build_save();
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_SAVE_DAMAGED,
                          skiff_sfo_parse(file.bytes, SKIFF_SFO_BYTES_MAX + 1, &sfo));
}

static void test_null_arguments(void) {
    build_save();
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_sfo_parse(NULL, file.size, &sfo));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_sfo_parse(file.bytes, file.size, NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, parse());
    skiff_sfo_value value;
    char text[8];
    uint32_t number = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_sfo_find(NULL, "TITLE", &value));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_sfo_find(&sfo, NULL, &value));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_sfo_find(&sfo, "TITLE", NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_sfo_text(&sfo, "TITLE", NULL, 8));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_sfo_text(&sfo, "TITLE", text, 0));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_sfo_number(&sfo, "TITLE", NULL));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_sfo_number(NULL, "TITLE", &number));
    TEST_ASSERT_FALSE(skiff_sfo_is_save(NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_save_reads_its_text_number_and_save_keys);
    RUN_TEST(test_an_install_has_neither_save_key);
    RUN_TEST(test_one_save_key_is_enough);
    RUN_TEST(test_keys_match_exactly);
    RUN_TEST(test_a_value_of_the_wrong_kind_or_too_long_is_refused);
    RUN_TEST(test_text_without_a_terminator_reads_its_used_bytes);
    RUN_TEST(test_a_cut_file_is_damaged_at_every_length);
    RUN_TEST(test_wrong_magic_or_version);
    RUN_TEST(test_lying_counts_and_table_offsets);
    RUN_TEST(test_lying_entries);
    RUN_TEST(test_keys_must_end_inside_the_key_table);
    RUN_TEST(test_a_file_larger_than_the_cap_is_refused);
    RUN_TEST(test_null_arguments);
    return UNITY_END();
}
