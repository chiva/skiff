/*
 * Units Skiff must refuse whole (skiff_saves_read_unit(), design §7), each crafted here byte by
 * byte from a valid synthetic unit with one thing wrong: names that would leave the staging folder
 * or the game's folders, duplicates, encryption, ZIP64, several disks, other methods, caps, sizes
 * and CRC-32s that lie, damaged deflate data, and layouts with a comment, junk, gaps, overlaps or a
 * central directory that disagrees with the local headers. Each refusal must name its problem and
 * leave nothing in the staging folder. Also the variants other writers produce that must pass:
 * folder entries, data descriptors with and without a signature, extra fields and comments.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "skiff/saves.h"

#include "fake_storage.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define GAME "SKIF00001"
#define DATA00 GAME "DATA00"
#define DATA01 GAME "DATA01"
#define UNIT_PATH SKIFF_STORAGE_ROOT_APP "/unit.zip"
#define STAGING SKIFF_STORAGE_ROOT_APP "/stage"
#define ENTRIES_MAX 400
#define NAME_BYTES 160
#define ZIP_CAPACITY ((size_t)256 * 1024)
#define DEFLATE_CAPACITY 4096
#define DATA_BYTES 2048
/* Record layouts (APPNOTE.TXT), as the reader expects them. */
#define LOCAL_BYTES 30
#define CENTRAL_BYTES 46
#define END_BYTES 22
#define LOCAL_SIGNATURE 0x04034b50U
#define CENTRAL_SIGNATURE 0x02014b50U
#define END_SIGNATURE 0x06054b50U
#define DESCRIPTOR_SIGNATURE 0x08074b50U
#define LOCAL_VERSION 4
#define LOCAL_FLAGS 6
#define LOCAL_METHOD 8
#define LOCAL_DATE 12
#define LOCAL_CRC 14
#define LOCAL_COMPRESSED 18
#define LOCAL_SIZE 22
#define LOCAL_NAME_LENGTH 26
#define LOCAL_EXTRA_LENGTH 28
#define LOCAL_NAME 30
#define CENTRAL_MADE_BY 4
#define CENTRAL_VERSION 6
#define CENTRAL_FLAGS 8
#define CENTRAL_METHOD 10
#define CENTRAL_DATE 14
#define CENTRAL_CRC 16
#define CENTRAL_COMPRESSED 20
#define CENTRAL_SIZE 24
#define CENTRAL_NAME_LENGTH 28
#define CENTRAL_EXTRA_LENGTH 30
#define CENTRAL_COMMENT_LENGTH 32
#define CENTRAL_DISK 34
#define CENTRAL_OFFSET 42
#define END_DISK 4
#define END_DISK_ENTRIES 8
#define END_ENTRIES 10
#define END_DIRECTORY_SIZE 12
#define END_DIRECTORY_OFFSET 16
#define END_COMMENT 20
/* A data descriptor: an optional signature, then CRC-32, compressed and uncompressed size. */
#define SIGNATURE_BYTES 4
#define FIELD_BYTES 4
#define DESCRIPTOR_FIELDS_BYTES 12
#define METHOD_STORED 0
#define METHOD_DEFLATE 8
#define METHOD_BZIP2 12
#define FLAG_ENCRYPTED 0x0001U
#define FLAG_DESCRIPTOR 0x0008U
#define FLAG_STRONG_ENCRYPTION 0x0040U
#define FLAG_UTF8 0x0800U
#define FLAG_ENCRYPTED_DIRECTORY 0x2000U
#define DOS_DATE_1980_01_01 0x0021U
#define VERSION_20 20
#define ZIP64_U16 0xFFFFU
#define ZIP64_U32 0xFFFFFFFFU
#define DESCRIPTOR_SIGNED 1
#define DESCRIPTOR_PLAIN 2

typedef struct test_entry {
    const char *name;
    const void *data;
    size_t size;
    int deflate;
    /* DESCRIPTOR_SIGNED or DESCRIPTOR_PLAIN: flag 3, zeros in the local header, a descriptor. */
    int descriptor;
    uint16_t flags;
    /* XORed into the CRC-32 both headers record. */
    uint32_t crc_flip;
    /* When set, both headers declare declared_size as the uncompressed size. */
    int declare;
    uint32_t declared_size;
    /* Deflate only: drop the stream's last bytes, or add bytes after its end (headers agree). */
    size_t stream_cut;
    size_t stream_junk;
    const unsigned char *central_extra;
    size_t central_extra_size;
    const unsigned char *local_extra;
    size_t local_extra_size;
    const char *central_comment;
} test_entry;

typedef struct built_zip {
    unsigned char bytes[ZIP_CAPACITY];
    size_t size;
    size_t local[ENTRIES_MAX];
    size_t central[ENTRIES_MAX];
    size_t directory;
    size_t end;
} built_zip;

static char dir[TEMP_DIR_PATH_MAX];
static skiff_storage *posix;
static fake_storage storage;
static skiff_storage_roots roots;
static skiff_saves_unit *unit;
static built_zip zip;
static test_entry entries[ENTRIES_MAX];
static char names[ENTRIES_MAX][NAME_BYTES];
static unsigned char data[DATA_BYTES];

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    char app[TEMP_DIR_PATH_MAX];
    TEST_ASSERT_TRUE(temp_dir_path(dir, "PSP/GAME/Skiff", app, sizeof app));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_roots_init(dir, app, &roots));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&posix));
    fake_storage_init(&storage, posix);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_mkdirs(posix, roots.app));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_saves_unit_create(&unit));
    memset(entries, 0, sizeof entries);
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (unsigned char)('a' + i % 26);
    }
}

void tearDown(void) {
    skiff_saves_unit_destroy(unit);
    skiff_storage_destroy(&storage.base);
    skiff_storage_destroy(posix);
    temp_dir_remove(dir);
}

static void put16(unsigned char *at, uint32_t value) {
    at[0] = (unsigned char)(value & 0xFFU);
    at[1] = (unsigned char)((value >> 8) & 0xFFU);
}

static void put32(unsigned char *at, uint32_t value) {
    put16(at, value & 0xFFFFU);
    put16(at + 2, value >> 16);
}

static void append(const void *bytes, size_t size) {
    if (size == 0) {
        return;
    }
    TEST_ASSERT_TRUE(zip.size + size <= sizeof zip.bytes);
    memcpy(zip.bytes + zip.size, bytes, size);
    zip.size += size;
}

/* Raw deflate (no zlib header), as zip entries hold it. */
static size_t deflate_raw(const void *in, size_t size, unsigned char *out, size_t capacity) {
    static unsigned char input[DATA_BYTES];
    TEST_ASSERT_TRUE(size <= sizeof input);
    memcpy(input, in, size);
    z_stream z;
    memset(&z, 0, sizeof z);
    TEST_ASSERT_EQUAL_INT(
        Z_OK, deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY));
    z.next_in = input;
    z.avail_in = (uInt)size;
    z.next_out = out;
    z.avail_out = (uInt)capacity;
    TEST_ASSERT_EQUAL_INT(Z_STREAM_END, deflate(&z, Z_FINISH));
    const size_t made = capacity - z.avail_out;
    deflateEnd(&z);
    return made;
}

/* Lays entries out as a zip: prefix junk, the entries, a gap, the directory, the end record. */
static void build_with(size_t count, size_t prefix, size_t gap) {
    memset(&zip, 0, sizeof zip);
    unsigned char filler[64];
    memset(filler, 'J', sizeof filler);
    append(filler, prefix);
    static unsigned char stream[DEFLATE_CAPACITY];
    uint32_t crcs[ENTRIES_MAX];
    uint32_t sizes[ENTRIES_MAX];
    uint32_t packed[ENTRIES_MAX];
    for (size_t i = 0; i < count; i++) {
        const test_entry *e = &entries[i];
        const size_t name_length = strlen(e->name);
        const unsigned char *body = e->data;
        size_t body_size = e->size;
        if (e->deflate) {
            body_size = deflate_raw(e->data, e->size, stream, sizeof stream - e->stream_junk);
            body_size -= e->stream_cut;
            memset(stream + body_size, 'Z', e->stream_junk);
            body_size += e->stream_junk;
            body = stream;
        }
        crcs[i] = (uint32_t)crc32(0L, e->data, (uInt)e->size) ^ e->crc_flip;
        sizes[i] = e->declare ? e->declared_size : (uint32_t)e->size;
        packed[i] = e->deflate ? (uint32_t)body_size : (e->declare ? e->declared_size : sizes[i]);
        const uint16_t flags = (uint16_t)(e->flags | (e->descriptor ? FLAG_DESCRIPTOR : 0));
        unsigned char h[LOCAL_BYTES];
        memset(h, 0, sizeof h);
        put32(h, LOCAL_SIGNATURE);
        put16(h + LOCAL_VERSION, VERSION_20);
        put16(h + LOCAL_FLAGS, flags);
        put16(h + LOCAL_METHOD, e->deflate ? METHOD_DEFLATE : METHOD_STORED);
        put16(h + LOCAL_DATE, DOS_DATE_1980_01_01);
        if (!e->descriptor) {
            put32(h + LOCAL_CRC, crcs[i]);
            put32(h + LOCAL_COMPRESSED, packed[i]);
            put32(h + LOCAL_SIZE, sizes[i]);
        }
        put16(h + LOCAL_NAME_LENGTH, (uint32_t)name_length);
        put16(h + LOCAL_EXTRA_LENGTH, (uint32_t)e->local_extra_size);
        zip.local[i] = zip.size;
        append(h, sizeof h);
        append(e->name, name_length);
        append(e->local_extra, e->local_extra_size);
        append(body, body_size);
        if (e->descriptor) {
            unsigned char d[SIGNATURE_BYTES + DESCRIPTOR_FIELDS_BYTES];
            size_t at = 0;
            if (e->descriptor == DESCRIPTOR_SIGNED) {
                put32(d, DESCRIPTOR_SIGNATURE);
                at = SIGNATURE_BYTES;
            }
            put32(d + at, crcs[i]);
            put32(d + at + FIELD_BYTES, packed[i]);
            put32(d + at + (size_t)2 * FIELD_BYTES, sizes[i]);
            append(d, at + DESCRIPTOR_FIELDS_BYTES);
        }
    }
    append(filler, gap);
    zip.directory = zip.size;
    for (size_t i = 0; i < count; i++) {
        const test_entry *e = &entries[i];
        const size_t name_length = strlen(e->name);
        const size_t comment = e->central_comment != NULL ? strlen(e->central_comment) : 0;
        unsigned char h[CENTRAL_BYTES];
        memset(h, 0, sizeof h);
        put32(h, CENTRAL_SIGNATURE);
        put16(h + CENTRAL_MADE_BY, VERSION_20);
        put16(h + CENTRAL_VERSION, VERSION_20);
        put16(h + CENTRAL_FLAGS, (uint16_t)(e->flags | (e->descriptor ? FLAG_DESCRIPTOR : 0)));
        put16(h + CENTRAL_METHOD, e->deflate ? METHOD_DEFLATE : METHOD_STORED);
        put16(h + CENTRAL_DATE, DOS_DATE_1980_01_01);
        put32(h + CENTRAL_CRC, crcs[i]);
        put32(h + CENTRAL_COMPRESSED, packed[i]);
        put32(h + CENTRAL_SIZE, sizes[i]);
        put16(h + CENTRAL_NAME_LENGTH, (uint32_t)name_length);
        put16(h + CENTRAL_EXTRA_LENGTH, (uint32_t)e->central_extra_size);
        put16(h + CENTRAL_COMMENT_LENGTH, (uint32_t)comment);
        put32(h + CENTRAL_OFFSET, (uint32_t)zip.local[i]);
        zip.central[i] = zip.size;
        append(h, sizeof h);
        append(e->name, name_length);
        append(e->central_extra, e->central_extra_size);
        append(e->central_comment, comment);
    }
    unsigned char end[END_BYTES];
    memset(end, 0, sizeof end);
    put32(end, END_SIGNATURE);
    put16(end + END_DISK_ENTRIES, (uint32_t)count);
    put16(end + END_ENTRIES, (uint32_t)count);
    put32(end + END_DIRECTORY_SIZE, (uint32_t)(zip.size - zip.directory));
    put32(end + END_DIRECTORY_OFFSET, (uint32_t)zip.directory);
    zip.end = zip.size;
    append(end, sizeof end);
}

static void build(size_t count) { build_with(count, 0, 0); }

static test_entry *file(size_t i, const char *name, size_t size) {
    entries[i].name = name;
    entries[i].data = data;
    entries[i].size = size;
    return &entries[i];
}

/* The valid unit every refusal starts from: two folders, stored and deflated files. */
static size_t base_unit(void) {
    file(0, DATA00 "/PARAM.SFO", 40);
    file(1, DATA00 "/DATA.BIN", DATA_BYTES)->deflate = 1;
    file(2, DATA01 "/DATA.BIN", 100);
    return 3;
}

static void write_zip(size_t size) {
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, UNIT_PATH, path, sizeof path));
    skiff_file *out = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_open(posix, path, SKIFF_FILE_REPLACE, 0, &out));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(out, zip.bytes, size));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(out));
}

static skiff_err read_unit(void) {
    write_zip(zip.size);
    const skiff_err err =
        skiff_saves_read_unit(&storage.base, &roots, UNIT_PATH, GAME, STAGING, unit);
    TEST_PRINTF("%zu-byte unit: %s, problem %s \"%s\", %zu files, hash \"%s\"", zip.size,
                skiff_err_name(err), skiff_saves_problem_name(unit->problem), unit->problem_name,
                unit->file_count, unit->content_hash);
    return err;
}

static size_t staged_entries;

static skiff_storage_list_step count_entry(void *ctx, const skiff_storage_entry *entry) {
    (void)ctx;
    TEST_PRINTF("left in staging: %s", entry->name);
    staged_entries++;
    return SKIFF_STORAGE_LIST_CONTINUE;
}

static void assert_nothing_staged(void) {
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, STAGING, path, sizeof path));
    staged_entries = 0;
    size_t skipped = 0;
    const skiff_err err = skiff_storage_list(posix, path, count_entry, NULL, &skipped);
    TEST_ASSERT_TRUE(err == SKIFF_OK || err == SKIFF_ERR_STORAGE_NOT_FOUND);
    TEST_ASSERT_EQUAL_size_t(0, staged_entries);
}

static void assert_refused(skiff_saves_problem problem, const char *why) {
    TEST_PRINTF("refuse: %s", why);
    TEST_ASSERT_EQUAL_STRING(skiff_err_name(SKIFF_ERR_STORAGE_SAVE_DAMAGED),
                             skiff_err_name(read_unit()));
    TEST_ASSERT_EQUAL_STRING(skiff_saves_problem_name(problem),
                             skiff_saves_problem_name(unit->problem));
    TEST_ASSERT_EQUAL_size_t(0, unit->file_count);
    TEST_ASSERT_EQUAL_size_t(0, unit->folder_count);
    TEST_ASSERT_EQUAL_STRING("", unit->content_hash);
    assert_nothing_staged();
}

static void assert_name_refused(const char *name, skiff_saves_problem problem) {
    memset(entries, 0, sizeof entries);
    const size_t count = base_unit();
    file(count, name, 5);
    build(count + 1);
    TEST_PRINTF("entry name \"%s\"", name);
    assert_refused(problem, "a name a unit may not hold");
    TEST_ASSERT_EQUAL_STRING(name, unit->problem_name);
}

/* ---- What must pass ---- */

static void test_the_base_unit_is_accepted(void) {
    build(base_unit());
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, read_unit());
    TEST_ASSERT_EQUAL_size_t(2, unit->folder_count);
    TEST_ASSERT_EQUAL_size_t(3, unit->file_count);
    TEST_ASSERT_EQUAL_STRING(DATA00 "/DATA.BIN", unit->files[0].name);
    TEST_ASSERT_EQUAL_STRING(DATA00 "/PARAM.SFO", unit->files[1].name);
    TEST_ASSERT_EQUAL_UINT64(DATA_BYTES + 140, unit->total_bytes);
}

static void test_other_writers_variants_are_accepted(void) {
    static const unsigned char TIMESTAMP_EXTRA[] = {0x55, 0x54, 0x05, 0x00, 0x01, 0, 0, 0, 0};
    static const unsigned char PADDING[] = {0, 0};
    file(0, DATA00 "/", 0);
    file(1, DATA00 "/PARAM.SFO", 40)->descriptor = DESCRIPTOR_SIGNED;
    test_entry *e = file(2, DATA00 "/DATA.BIN", DATA_BYTES);
    e->deflate = 1;
    e->descriptor = DESCRIPTOR_PLAIN;
    e->central_extra = TIMESTAMP_EXTRA;
    e->central_extra_size = sizeof TIMESTAMP_EXTRA;
    e->local_extra = TIMESTAMP_EXTRA;
    e->local_extra_size = sizeof TIMESTAMP_EXTRA;
    e = file(3, GAME "EMPTYFOLDER/", 0);
    e->central_comment = "a folder entry with no files";
    e = file(4, DATA01 "/EMPTY.BIN", 0);
    e->flags = FLAG_UTF8;
    e->central_extra = PADDING;
    e->central_extra_size = sizeof PADDING;
    build(5);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, read_unit());
    TEST_ASSERT_EQUAL_size_t(3, unit->folder_count);
    TEST_ASSERT_EQUAL_STRING(GAME "EMPTYFOLDER", unit->folders[2]);
    TEST_ASSERT_EQUAL_size_t(3, unit->file_count);
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(
        SKIFF_OK, skiff_storage_resolve(&roots, STAGING "/" GAME "EMPTYFOLDER", path, sizeof path));
    skiff_storage_info info;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_stat(posix, path, &info));
    TEST_ASSERT_TRUE(info.is_dir);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, STAGING "/" DATA01 "/EMPTY.BIN",
                                                          path, sizeof path));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_stat(posix, path, &info));
    TEST_ASSERT_EQUAL_UINT64(0, info.size);
}

static void test_folder_names_match_the_game_ignoring_case(void) {
    file(0, "skif00001data00/DATA.BIN", 10);
    build(1);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, read_unit());
    TEST_ASSERT_EQUAL_STRING("skif00001data00", unit->folders[0]);
}

/* ---- Names ---- */

static void test_names_that_leave_the_folder_are_refused(void) {
    assert_name_refused("/" DATA00 "/ABS.BIN", SKIFF_SAVES_PROBLEM_PATH);
    assert_name_refused(DATA00 "/../ESCAPE.BIN", SKIFF_SAVES_PROBLEM_PATH);
    assert_name_refused(DATA00 "/./HERE.BIN", SKIFF_SAVES_PROBLEM_PATH);
    assert_name_refused("../" DATA00 "/UP.BIN", SKIFF_SAVES_PROBLEM_PATH);
    assert_name_refused(DATA00 "\\WINDOWS.BIN", SKIFF_SAVES_PROBLEM_PATH);
    assert_name_refused("ms0:/" DATA00 "/DEVICE.BIN", SKIFF_SAVES_PROBLEM_PATH);
    assert_name_refused(DATA00 "//EMPTY.BIN", SKIFF_SAVES_PROBLEM_PATH);
    assert_name_refused("ROOT.BIN", SKIFF_SAVES_PROBLEM_PATH);
    assert_name_refused(DATA00 "/SUB/DEEP.BIN", SKIFF_SAVES_PROBLEM_NESTED_FOLDER);
    assert_name_refused(DATA00 "/SUB/", SKIFF_SAVES_PROBLEM_NESTED_FOLDER);
}

static void test_names_that_are_not_plain_or_belong_to_another_game_are_refused(void) {
    char long_folder[NAME_BYTES];
    snprintf(long_folder, sizeof long_folder, "%s%0*d/X.BIN", GAME,
             SKIFF_SAVES_NAME_MAX + 1 - (int)strlen(GAME), 0);
    assert_name_refused(long_folder, SKIFF_SAVES_PROBLEM_NAME);
    assert_name_refused(DATA00 "/CAF\xC3\xA9.BIN", SKIFF_SAVES_PROBLEM_NAME);
    assert_name_refused(DATA00 "/CTRL\x01.BIN", SKIFF_SAVES_PROBLEM_NAME);
    assert_name_refused(DATA00 "/STAR*.BIN", SKIFF_SAVES_PROBLEM_NAME);
    assert_name_refused(DATA00 "/CON", SKIFF_SAVES_PROBLEM_NAME);
    assert_name_refused(GAME "DOT./X.BIN", SKIFF_SAVES_PROBLEM_NAME);
    assert_name_refused("SKIF00002DATA00/OTHER.BIN", SKIFF_SAVES_PROBLEM_OTHER_GAME);
    assert_name_refused("SKIF/SHORT.BIN", SKIFF_SAVES_PROBLEM_OTHER_GAME);
}

static void test_empty_and_overlong_names_are_refused(void) {
    file(0, "", 5);
    build(1);
    assert_refused(SKIFF_SAVES_PROBLEM_PATH, "an empty name");
    char overlong[NAME_BYTES];
    memset(overlong, 'N', sizeof overlong - 1);
    overlong[sizeof overlong - 1] = '\0';
    memcpy(overlong, DATA00 "/", strlen(DATA00 "/"));
    file(0, overlong, 5);
    build(1);
    assert_refused(SKIFF_SAVES_PROBLEM_NAME, "a name longer than a unit entry");
}

static void test_duplicates_are_refused(void) {
    const size_t count = base_unit();
    file(count, DATA00 "/PARAM.SFO", 5);
    build(count + 1);
    assert_refused(SKIFF_SAVES_PROBLEM_DUPLICATE, "the same entry twice");
    file(count, DATA00 "/param.sfo", 5);
    build(count + 1);
    assert_refused(SKIFF_SAVES_PROBLEM_DUPLICATE, "the same file in another case");
    file(count, "skif00001data01/OTHER.BIN", 5);
    build(count + 1);
    assert_refused(SKIFF_SAVES_PROBLEM_DUPLICATE, "a folder spelled two ways");
    file(count, DATA01 "/", 0);
    file(count + 1, DATA01 "/", 0);
    build(count + 2);
    assert_refused(SKIFF_SAVES_PROBLEM_DUPLICATE, "a folder entry twice");
}

/* ---- Format ---- */

static void test_encryption_zip64_disks_and_methods_are_refused(void) {
    static const uint16_t ENCRYPTED[] = {FLAG_ENCRYPTED, FLAG_STRONG_ENCRYPTION,
                                         FLAG_ENCRYPTED_DIRECTORY};
    for (size_t i = 0; i < sizeof ENCRYPTED / sizeof ENCRYPTED[0]; i++) {
        memset(entries, 0, sizeof entries);
        const size_t count = base_unit();
        entries[1].flags = ENCRYPTED[i];
        build(count);
        TEST_PRINTF("flags 0x%04x", ENCRYPTED[i]);
        assert_refused(SKIFF_SAVES_PROBLEM_ENCRYPTED, "an encrypted entry");
    }
    memset(entries, 0, sizeof entries);
    const size_t count = base_unit();
    build(count);
    put32(zip.bytes + zip.central[2] + CENTRAL_COMPRESSED, ZIP64_U32);
    assert_refused(SKIFF_SAVES_PROBLEM_ZIP64, "sizes in a ZIP64 record");
    build(count);
    put16(zip.bytes + zip.end + END_ENTRIES, ZIP64_U16);
    put16(zip.bytes + zip.end + END_DISK_ENTRIES, ZIP64_U16);
    assert_refused(SKIFF_SAVES_PROBLEM_ZIP64, "an entry count in a ZIP64 record");
    static const unsigned char ZIP64_EXTRA[] = {0x01, 0x00, 0x08, 0x00, 1, 0, 0, 0, 0, 0, 0, 0};
    entries[2].central_extra = ZIP64_EXTRA;
    entries[2].central_extra_size = sizeof ZIP64_EXTRA;
    build(count);
    assert_refused(SKIFF_SAVES_PROBLEM_ZIP64, "a ZIP64 extra field");
    entries[2].central_extra = NULL;
    entries[2].central_extra_size = 0;
    entries[2].local_extra = ZIP64_EXTRA;
    entries[2].local_extra_size = sizeof ZIP64_EXTRA;
    build(count);
    assert_refused(SKIFF_SAVES_PROBLEM_ZIP64, "a ZIP64 extra field in a local header only");
    static const unsigned char LONG_FIELD[] = {0x55, 0x54, 0x09, 0x00, 1, 0};
    entries[2].local_extra = LONG_FIELD;
    entries[2].local_extra_size = sizeof LONG_FIELD;
    build(count);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "a local extra field running past its end");
    entries[2].local_extra = NULL;
    entries[2].local_extra_size = 0;
    build(count);
    put32(zip.bytes + zip.local[2] + LOCAL_COMPRESSED, ZIP64_U32);
    assert_refused(SKIFF_SAVES_PROBLEM_ZIP64, "a local header pointing at ZIP64 sizes");
    build(count);
    put16(zip.bytes + zip.end + END_DISK, 1);
    assert_refused(SKIFF_SAVES_PROBLEM_MULTI_DISK, "the end record on disk 1");
    build(count);
    put16(zip.bytes + zip.end + END_DISK_ENTRIES, 2);
    assert_refused(SKIFF_SAVES_PROBLEM_MULTI_DISK, "entries on another disk");
    build(count);
    put16(zip.bytes + zip.central[1] + CENTRAL_DISK, 1);
    assert_refused(SKIFF_SAVES_PROBLEM_MULTI_DISK, "an entry starting on disk 1");
    build(count);
    put16(zip.bytes + zip.central[1] + CENTRAL_METHOD, METHOD_BZIP2);
    assert_refused(SKIFF_SAVES_PROBLEM_METHOD, "bzip2");
}

static void test_caps_are_checked_before_anything_is_inflated(void) {
    for (size_t i = 0; i <= SKIFF_SAVES_FOLDERS_MAX; i++) {
        snprintf(names[i], sizeof names[i], GAME "F%03zu/X.BIN", i);
        file(i, names[i], 1);
    }
    build(SKIFF_SAVES_FOLDERS_MAX + 1);
    assert_refused(SKIFF_SAVES_PROBLEM_TOO_MANY_FOLDERS, "65 folders");
    for (size_t i = 0; i <= SKIFF_SAVES_FILES_MAX; i++) {
        snprintf(names[i], sizeof names[i], DATA00 "/F%03zu.BIN", i);
        file(i, names[i], 1);
    }
    build(SKIFF_SAVES_FILES_MAX + 1);
    assert_refused(SKIFF_SAVES_PROBLEM_TOO_MANY_FILES, "257 files");
    const size_t too_many = SKIFF_SAVES_FILES_MAX + SKIFF_SAVES_FOLDERS_MAX + 1;
    for (size_t i = 0; i < too_many; i++) {
        snprintf(names[i], sizeof names[i], DATA00 "/E%03zu.BIN", i);
        file(i, names[i], 0);
    }
    build(too_many);
    assert_refused(SKIFF_SAVES_PROBLEM_TOO_MANY_FILES, "more entries than files and folders");
    memset(entries, 0, sizeof entries);
    test_entry *bomb = file(0, DATA00 "/BOMB.BIN", DATA_BYTES);
    bomb->deflate = 1;
    bomb->declare = 1;
    bomb->declared_size = (uint32_t)SKIFF_SAVES_UNIT_BYTES_MAX + 1;
    build(1);
    assert_refused(SKIFF_SAVES_PROBLEM_TOO_LARGE, "a file declaring more than 64 MiB");
}

static void test_a_unit_file_over_the_cap_is_refused_unread(void) {
    build(base_unit());
    write_zip(zip.size);
    char path[SKIFF_STORAGE_PATH_MAX];
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_resolve(&roots, UNIT_PATH, path, sizeof path));
    TEST_ASSERT_EQUAL_INT(0, truncate(path, (off_t)SKIFF_SAVES_UNIT_FILE_BYTES_MAX + 1));
    TEST_ASSERT_EQUAL_INT(
        SKIFF_ERR_STORAGE_SAVE_DAMAGED,
        skiff_saves_read_unit(&storage.base, &roots, UNIT_PATH, GAME, NULL, unit));
    TEST_ASSERT_EQUAL_INT(SKIFF_SAVES_PROBLEM_TOO_LARGE, unit->problem);
}

/* ---- Data ---- */

static void test_sizes_and_crcs_that_lie_are_refused(void) {
    const size_t count = base_unit();
    build(count);
    put32(zip.bytes + zip.central[0] + CENTRAL_SIZE, 41);
    assert_refused(SKIFF_SAVES_PROBLEM_SIZE, "a stored entry whose sizes disagree");
    entries[1].declare = 1;
    entries[1].declared_size = DATA_BYTES - 1;
    build(count);
    assert_refused(SKIFF_SAVES_PROBLEM_SIZE, "deflate data larger than declared");
    entries[1].declared_size = DATA_BYTES + 1;
    build(count);
    assert_refused(SKIFF_SAVES_PROBLEM_SIZE, "deflate data smaller than declared");
    entries[1].declare = 0;
    entries[2].crc_flip = 1;
    build(count);
    assert_refused(SKIFF_SAVES_PROBLEM_CRC, "a stored file failing its CRC-32");
    entries[2].crc_flip = 0;
    entries[1].crc_flip = 0x80000000U;
    build(count);
    assert_refused(SKIFF_SAVES_PROBLEM_CRC, "a deflated file failing its CRC-32");
}

static void test_damaged_deflate_data_is_refused(void) {
    const size_t count = base_unit();
    build(count);
    zip.bytes[zip.local[1] + LOCAL_NAME + strlen(entries[1].name)] = 0xFF;
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "an invalid deflate block type");
    entries[1].stream_cut = 2;
    build(count);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "a deflate stream cut short");
    entries[1].stream_cut = 0;
    entries[1].stream_junk = 3;
    build(count);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "bytes after the end of a deflate stream");
}

/* ---- Layout ---- */

static void test_comments_junk_gaps_and_cuts_are_refused(void) {
    const size_t count = base_unit();
    build(count);
    static const char JUNK[] = "trailing";
    append(JUNK, sizeof JUNK - 1);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "bytes after the end record");
    build(count);
    put16(zip.bytes + zip.end + END_COMMENT, sizeof JUNK - 1);
    append(JUNK, sizeof JUNK - 1);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "a zip comment");
    build_with(count, 7, 0);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "bytes before the first entry");
    build_with(count, 0, 5);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "a gap before the central directory");
    build(count);
    zip.size -= 1;
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "cut one byte short");
    zip.size = END_BYTES - 1;
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "shorter than an end record");
    build(count);
    put32(zip.bytes + zip.end + END_DIRECTORY_OFFSET, (uint32_t)zip.directory - 1);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "a directory offset that lies");
    build(count);
    put16(zip.bytes + zip.end + END_ENTRIES, (uint32_t)count + 1);
    put16(zip.bytes + zip.end + END_DISK_ENTRIES, (uint32_t)count + 1);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "more entries than the directory holds");
    build(count);
    put16(zip.bytes + zip.end + END_ENTRIES, (uint32_t)count - 1);
    put16(zip.bytes + zip.end + END_DISK_ENTRIES, (uint32_t)count - 1);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "fewer entries than the directory holds");
    build(count);
    put32(zip.bytes + zip.central[1], LOCAL_SIGNATURE);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "a directory record without its signature");
}

static void test_a_directory_that_disagrees_with_the_local_headers_is_refused(void) {
    const size_t count = base_unit();
    build(count);
    put32(zip.bytes + zip.central[2] + CENTRAL_OFFSET, (uint32_t)zip.local[1]);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "two entries at one offset (overlap)");
    build(count);
    put32(zip.bytes + zip.local[1], CENTRAL_SIGNATURE);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "a local header without its signature");
    build(count);
    put16(zip.bytes + zip.local[0] + LOCAL_METHOD, METHOD_DEFLATE);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "another method in the local header");
    build(count);
    put16(zip.bytes + zip.local[0] + LOCAL_FLAGS, FLAG_UTF8);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "other flags in the local header");
    build(count);
    put32(zip.bytes + zip.local[2] + LOCAL_CRC, 1);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "another CRC-32 in the local header");
    build(count);
    zip.bytes[zip.local[2] + LOCAL_NAME + 1] = 'X';
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "another name in the local header");
    entries[1].descriptor = DESCRIPTOR_SIGNED;
    build(count);
    put32(zip.bytes + zip.local[2] - DESCRIPTOR_FIELDS_BYTES, 0);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "a data descriptor with another CRC-32");
    entries[1].descriptor = 0;
    file(count, DATA01 "/", 0);
    entries[count].data = data;
    entries[count].size = 3;
    build(count + 1);
    assert_refused(SKIFF_SAVES_PROBLEM_STRUCTURE, "a folder entry holding data");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_base_unit_is_accepted);
    RUN_TEST(test_other_writers_variants_are_accepted);
    RUN_TEST(test_folder_names_match_the_game_ignoring_case);
    RUN_TEST(test_names_that_leave_the_folder_are_refused);
    RUN_TEST(test_names_that_are_not_plain_or_belong_to_another_game_are_refused);
    RUN_TEST(test_empty_and_overlong_names_are_refused);
    RUN_TEST(test_duplicates_are_refused);
    RUN_TEST(test_encryption_zip64_disks_and_methods_are_refused);
    RUN_TEST(test_caps_are_checked_before_anything_is_inflated);
    RUN_TEST(test_a_unit_file_over_the_cap_is_refused_unread);
    RUN_TEST(test_sizes_and_crcs_that_lie_are_refused);
    RUN_TEST(test_damaged_deflate_data_is_refused);
    RUN_TEST(test_comments_junk_gaps_and_cuts_are_refused);
    RUN_TEST(test_a_directory_that_disagrees_with_the_local_headers_is_refused);
    return UNITY_END();
}
