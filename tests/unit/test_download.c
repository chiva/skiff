/*
 * Resumable downloads (skiff/download.h) against the fake transport and a fake Memory Stick over a
 * temporary directory: a download cut at any point continues from what is durable, a file that
 * changed on the server starts over, an error page never reaches the .part file, and a failing
 * Memory Stick never leaves a .resume file that vouches for bytes it does not hold.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "skiff/download.h"

#include "fake_storage.h"
#include "fake_transport.h"
#include "storage_posix.h"
#include "temp_dir.h"
#include "unity.h"

#define CONTENT_PATH "/api/roms/1/content/Skiff%20Test.iso"
#define CONTENT_URL "https://romm.test" CONTENT_PATH
#define ETAG "\"v1\""
#define CHANGED_ETAG "\"v2\""
#define WEAK_ETAG "W/\"v1\""
#define MIB ((uint64_t)1024 * 1024)
/* Two checkpoints and an odd tail. */
#define BODY_BYTES (9 * MIB + 77)
#define HEADERS_MAX 256

static char dir[TEMP_DIR_PATH_MAX];
static char target[TEMP_DIR_PATH_MAX];
static char part[TEMP_DIR_PATH_MAX + sizeof SKIFF_DOWNLOAD_PART_SUFFIX];
static char state_file[TEMP_DIR_PATH_MAX + sizeof SKIFF_DOWNLOAD_STATE_SUFFIX];
static skiff_storage *posix;
static fake_storage storage;
static fake_transport transport;
static unsigned char *body;
static uint32_t body_crc;
static skiff_download_spec spec;
static skiff_download_result result;

/* What the progress callback saw. */
typedef struct progress_log {
    int calls;
    uint64_t last_done;
    int went_backwards;
} progress_log;

static progress_log progress;

static void on_progress(void *ctx, uint64_t done, uint64_t total) {
    progress_log *log = ctx;
    TEST_ASSERT_EQUAL_UINT64(BODY_BYTES, total);
    if (done < log->last_done) {
        log->went_backwards = 1;
    }
    log->last_done = done;
    log->calls++;
}

/* A response whose body is `size` bytes of the synthetic file. */
static fake_route *serve_on(fake_transport *fake, const char *status_line, const char *etag,
                            int with_length, size_t size) {
    char headers[HEADERS_MAX];
    int used = snprintf(headers, sizeof headers, "%s\r\n", status_line);
    if (etag != NULL) {
        used += snprintf(headers + used, sizeof headers - (size_t)used, "ETag: %s\r\n", etag);
    }
    if (with_length) {
        used += snprintf(headers + used, sizeof headers - (size_t)used, "Content-Length: %zu\r\n",
                         size);
    }
    used += snprintf(headers + used, sizeof headers - (size_t)used, "\r\n");
    const size_t raw_size = (size_t)used + size;
    char *raw = malloc(raw_size);
    TEST_ASSERT_NOT_NULL(raw);
    memcpy(raw, headers, (size_t)used);
    for (size_t i = 0; i < size; i++) {
        raw[(size_t)used + i] = (char)body[i % BODY_BYTES];
    }
    fake_route *route = fake_transport_add_raw(fake, CONTENT_PATH, raw, raw_size);
    free(raw);
    TEST_ASSERT_NOT_NULL(route);
    return route;
}

static fake_route *serve(const char *etag) {
    return serve_on(&transport, "HTTP/1.1 200 OK", etag, 1, BODY_BYTES);
}

void setUp(void) {
    TEST_ASSERT_EQUAL_INT(0, temp_dir_create(dir, sizeof dir));
    TEST_ASSERT_TRUE(temp_dir_path(dir, "Skiff Test.iso", target, sizeof target));
    snprintf(part, sizeof part, "%s" SKIFF_DOWNLOAD_PART_SUFFIX, target);
    snprintf(state_file, sizeof state_file, "%s" SKIFF_DOWNLOAD_STATE_SUFFIX, target);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_posix_storage_create(&posix));
    fake_storage_init(&storage, posix);
    fake_transport_init(&transport);
    body = malloc(BODY_BYTES);
    TEST_ASSERT_NOT_NULL(body);
    uint32_t seed = 0x5EED1234U;
    for (uint64_t i = 0; i < BODY_BYTES; i++) {
        seed = seed * 1103515245U + 12345U;
        body[i] = (unsigned char)(seed >> 16);
    }
    body_crc = (uint32_t)crc32(0L, body, (uInt)BODY_BYTES);
    memset(&progress, 0, sizeof progress);
    memset(&spec, 0, sizeof spec);
    spec.url = CONTENT_URL;
    spec.target_path = target;
    spec.expected_size = BODY_BYTES;
    spec.has_expected_crc32 = 1;
    spec.expected_crc32 = body_crc;
    spec.on_progress = on_progress;
    spec.progress_ctx = &progress;
}

void tearDown(void) {
    skiff_transport_destroy(&transport.base);
    skiff_storage_destroy(&storage.base);
    skiff_storage_destroy(posix);
    free(body);
    temp_dir_remove(dir);
}

static skiff_err attempt_with(fake_transport *fake) {
    const skiff_err err = skiff_download_attempt(&fake->base, &storage.base, &spec, &result);
    TEST_PRINTF("attempt -> %s: HTTP %ld, resumed from %llu, received %llu, restarted %d, "
                "complete %d",
                skiff_err_name(err), result.response.status,
                (unsigned long long)result.resumed_from, (unsigned long long)result.bytes_received,
                result.restarted, result.complete);
    return err;
}

static skiff_err attempt(void) { return attempt_with(&transport); }

static int exists(const char *path) {
    uint64_t size = 0;
    return skiff_storage_size(posix, path, &size) == SKIFF_OK;
}

static uint64_t size_of(const char *path) {
    uint64_t size = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_OK, skiff_storage_size(posix, path, &size), path);
    return size;
}

/* The whole file (caller frees). */
static unsigned char *read_all(const char *path, uint64_t *size) {
    *size = size_of(path);
    unsigned char *bytes = malloc((size_t)*size + 1);
    TEST_ASSERT_NOT_NULL(bytes);
    skiff_file *file = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_open(posix, path, SKIFF_FILE_READ, 0, &file));
    size_t used = 0;
    size_t got = 0;
    do {
        TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                              skiff_file_read(file, bytes + used, (size_t)*size + 1 - used, &got));
        used += got;
    } while (got > 0);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    TEST_ASSERT_EQUAL_UINT64(*size, used);
    return bytes;
}

static void write_all(const char *path, const void *bytes, size_t size) {
    skiff_file *file = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_open(posix, path, SKIFF_FILE_REPLACE, 0, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, bytes, size));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
}

static skiff_download_state saved_state(void) {
    uint64_t size = 0;
    unsigned char *text = read_all(state_file, &size);
    skiff_download_state state;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        SKIFF_OK, skiff_download_state_parse((const char *)text, (size_t)size, &state),
        "the .resume file parses");
    free(text);
    TEST_PRINTF("saved state: offset %llu, crc32 %x, ETag '%s'", (unsigned long long)state.offset,
                (unsigned)state.crc32, state.etag);
    return state;
}

/* The saved offset and CRC describe the .part file's first bytes, which are the file's. */
static void assert_durable_prefix(uint64_t expected_offset) {
    const skiff_download_state state = saved_state();
    TEST_ASSERT_EQUAL_UINT64(expected_offset, state.offset);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)crc32(0L, body, (uInt)expected_offset), state.crc32);
    uint64_t size = 0;
    unsigned char *bytes = read_all(part, &size);
    TEST_ASSERT_TRUE_MESSAGE(size >= expected_offset, ".part holds at least the saved offset");
    TEST_ASSERT_EQUAL_MEMORY(body, bytes, (size_t)expected_offset);
    free(bytes);
}

static void assert_complete(void) {
    TEST_ASSERT_TRUE(result.complete);
    TEST_ASSERT_FALSE_MESSAGE(exists(part), "no .part left");
    TEST_ASSERT_FALSE_MESSAGE(exists(state_file), "no .resume left");
    uint64_t size = 0;
    unsigned char *bytes = read_all(target, &size);
    TEST_ASSERT_EQUAL_UINT64(BODY_BYTES, size);
    TEST_ASSERT_EQUAL_MEMORY(body, bytes, (size_t)BODY_BYTES);
    free(bytes);
}

static const fake_request *last_request(void) {
    TEST_ASSERT_TRUE(transport.log_count > 0);
    return &transport.log[transport.log_count - 1];
}

/* Cuts the first attempt after `bytes`, leaving the route ready to serve whole again. */
static void cut_after(fake_route *route, uint64_t bytes) {
    route->fail_after_bytes = bytes;
    route->fail_mid_body = SKIFF_ERR_NET_CONNECTION_LOST;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECTION_LOST, attempt());
    route->fail_mid_body = SKIFF_OK;
}

static void test_fresh_download_completes_and_is_renamed(void) {
    serve(ETAG);
    storage.fail_suffix = SKIFF_DOWNLOAD_PART_SUFFIX; /* count only the .part file's writes */
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    assert_complete();
    TEST_ASSERT_EQUAL_INT64(200, result.response.status);
    TEST_ASSERT_EQUAL_UINT64(0, result.resumed_from);
    TEST_ASSERT_EQUAL_UINT64(BODY_BYTES, result.bytes_received);
    TEST_ASSERT_FALSE(result.restarted);
    TEST_ASSERT_FALSE_MESSAGE(last_request()->has_range, "nothing to resume");
    TEST_ASSERT_EQUAL_UINT64(BODY_BYTES, progress.last_done);
    TEST_ASSERT_FALSE(progress.went_backwards);
    const int blocks = (int)((BODY_BYTES + SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES - 1) /
                             SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES);
    TEST_PRINTF("%d progress calls, %d .part writes, %d syncs", progress.calls, storage.writes,
                storage.syncs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(blocks, storage.writes,
                                  "1 MiB blocks, not one write per network chunk");
}

static void test_cut_mid_body_saves_the_exact_offset_and_resumes(void) {
    fake_route *route = serve(ETAG);
    const uint64_t cut = 5 * MIB + 123;
    cut_after(route, cut);
    assert_durable_prefix(cut);
    TEST_ASSERT_FALSE(exists(target));

    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    const fake_request *request = last_request();
    TEST_ASSERT_TRUE(request->has_range);
    TEST_ASSERT_EQUAL_UINT64(cut, request->range_start);
    TEST_ASSERT_EQUAL_STRING(ETAG, request->if_range);
    TEST_ASSERT_EQUAL_INT64(206, result.response.status);
    TEST_ASSERT_EQUAL_UINT64(cut, result.resumed_from);
    TEST_ASSERT_EQUAL_UINT64(BODY_BYTES - cut, result.bytes_received);
    assert_complete();
}

static void test_cut_before_the_first_checkpoint_and_at_the_very_start(void) {
    fake_route *route = serve(ETAG);
    cut_after(route, 1000);
    assert_durable_prefix(1000);
    cut_after(route, 0);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(1000, saved_state().offset,
                                     "a resume cut before any byte keeps what was saved");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    assert_complete();
}

static void test_cut_while_resuming_keeps_both_parts(void) {
    fake_route *route = serve(ETAG);
    cut_after(route, 2 * MIB);
    cut_after(route, 3 * MIB);
    assert_durable_prefix(5 * MIB);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_EQUAL_UINT64(5 * MIB, result.resumed_from);
    assert_complete();
}

static void test_stale_etag_restarts_with_the_whole_file(void) {
    fake_route *route = serve(ETAG);
    cut_after(route, 5 * MIB);
    route->current_etag = CHANGED_ETAG;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_TRUE_MESSAGE(last_request()->has_range, "a range was asked for");
    TEST_ASSERT_EQUAL_INT64(200, result.response.status);
    TEST_ASSERT_TRUE(result.restarted);
    TEST_ASSERT_EQUAL_UINT64(BODY_BYTES, result.bytes_received);
    TEST_ASSERT_TRUE_MESSAGE(progress.went_backwards, "progress starts over with the file");
    assert_complete();
}

static void test_a_restart_rewrites_the_state_before_any_new_byte(void) {
    fake_route *route = serve(ETAG);
    cut_after(route, 5 * MIB);
    route->current_etag = CHANGED_ETAG;
    route->fail_after_bytes = 1000;
    route->fail_mid_body = SKIFF_ERR_NET_TIMEOUT;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_TIMEOUT, attempt());
    const skiff_download_state state = saved_state();
    TEST_ASSERT_EQUAL_STRING_MESSAGE(CHANGED_ETAG, state.etag, "the new version's ETag");
    assert_durable_prefix(1000);
}

static void test_server_ignoring_ranges_restarts(void) {
    fake_route *route = serve(ETAG);
    cut_after(route, 5 * MIB);
    route->ignore_range = 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_TRUE(result.restarted);
    assert_complete();
}

static void test_weak_or_missing_etag_never_resumes(void) {
    fake_route *route = serve(WEAK_ETAG);
    cut_after(route, 5 * MIB);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", saved_state().etag, "a weak ETag is not kept");
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_FALSE_MESSAGE(last_request()->has_range, "If-Range needs a strong ETag");
    TEST_ASSERT_EQUAL_UINT64(0, result.resumed_from);
    assert_complete();

    skiff_transport_destroy(&transport.base);
    fake_transport_init(&transport);
    route = serve(NULL);
    cut_after(route, 1000);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_FALSE(last_request()->has_range);
    assert_complete();
}

/* The .part and .resume files as they are now, to compare after an attempt that must not touch
 * them. */
typedef struct snapshot {
    unsigned char *part;
    uint64_t part_size;
    unsigned char *state;
    uint64_t state_size;
} snapshot;

static snapshot take_snapshot(void) {
    snapshot taken;
    taken.part = read_all(part, &taken.part_size);
    taken.state = read_all(state_file, &taken.state_size);
    return taken;
}

static void assert_unchanged(snapshot *before) {
    snapshot after = take_snapshot();
    TEST_ASSERT_EQUAL_UINT64(before->part_size, after.part_size);
    TEST_ASSERT_EQUAL_MEMORY(before->part, after.part, (size_t)after.part_size);
    TEST_ASSERT_EQUAL_UINT64(before->state_size, after.state_size);
    TEST_ASSERT_EQUAL_MEMORY(before->state, after.state, (size_t)after.state_size);
    free(after.part);
    free(after.state);
    free(before->part);
    free(before->state);
}

/* Cuts a download at 5 MiB, then answers the resume with `status_line` and a short body. */
static skiff_err resume_answered_with(const char *raw) {
    fake_route *route = serve(ETAG);
    cut_after(route, 5 * MIB);
    fake_transport other;
    fake_transport_init(&other);
    TEST_ASSERT_NOT_NULL(fake_transport_add_raw(&other, CONTENT_PATH, raw, strlen(raw)));
    const skiff_err err = attempt_with(&other);
    skiff_transport_destroy(&other.base);
    return err;
}

static void test_error_pages_never_reach_the_part_file(void) {
    static const char *const PAGES[] = {
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\n\r\nUnauthorized",
        "HTTP/1.1 404 Not Found\r\n\r\n",
        "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 4\r\n\r\nbusy",
        "HTTP/1.1 204 No Content\r\n\r\n",
        "HTTP/1.1 302 Found\r\nLocation: /login\r\nContent-Length: 5\r\n\r\nlogin",
    };
    static const skiff_err EXPECTED[] = {
        SKIFF_ERR_ROMM_UNAUTHORIZED, SKIFF_ERR_ROMM_NOT_FOUND,    SKIFF_ERR_ROMM_SERVER,
        SKIFF_ERR_ROMM_BAD_RESPONSE, SKIFF_ERR_ROMM_BAD_RESPONSE,
    };
    for (size_t i = 0; i < sizeof PAGES / sizeof PAGES[0]; i++) {
        skiff_transport_destroy(&transport.base);
        fake_transport_init(&transport);
        skiff_download_discard(&storage.base, target);
        fake_route *route = serve(ETAG);
        cut_after(route, 5 * MIB);
        snapshot before = take_snapshot();
        fake_transport other;
        fake_transport_init(&other);
        TEST_ASSERT_NOT_NULL(
            fake_transport_add_raw(&other, CONTENT_PATH, PAGES[i], strlen(PAGES[i])));
        TEST_ASSERT_EQUAL_INT(EXPECTED[i], attempt_with(&other));
        skiff_transport_destroy(&other.base);
        assert_unchanged(&before);
    }
}

static void test_partial_response_that_does_not_fit_is_refused(void) {
    char raw[HEADERS_MAX];
    snprintf(raw, sizeof raw,
             "HTTP/1.1 206 Partial Content\r\nETag: " ETAG "\r\nContent-Range: bytes 0-3/%llu\r\n"
             "Content-Length: 4\r\n\r\nabcd",
             (unsigned long long)BODY_BYTES);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_ROMM_BAD_RESPONSE, resume_answered_with(raw),
                                  "206 starting at 0 instead of the saved offset");
    assert_durable_prefix(5 * MIB);
}

static void test_partial_response_with_another_total_is_refused(void) {
    const unsigned long long start = 5 * MIB;
    char raw[HEADERS_MAX];
    snprintf(raw, sizeof raw,
             "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %llu-%llu/%llu\r\n\r\nab", start,
             start + 1, start + 2);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, resume_answered_with(raw));
    assert_durable_prefix(5 * MIB);
}

static void test_unasked_partial_response_is_refused(void) {
    serve_on(&transport, "HTTP/1.1 206 Partial Content", ETAG, 1, 10);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, attempt());
    TEST_ASSERT_FALSE(exists(part));
}

static void test_range_no_longer_satisfiable_discards_the_progress(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          resume_answered_with("HTTP/1.1 416 Range Not Satisfiable\r\n"
                                               "Content-Length: 0\r\n\r\n"));
    TEST_ASSERT_FALSE(exists(part));
    TEST_ASSERT_FALSE(exists(state_file));
}

static void test_range_no_longer_satisfiable_with_an_error_page_discards_too(void) {
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE,
                          resume_answered_with("HTTP/1.1 416 Range Not Satisfiable\r\n"
                                               "Content-Length: 9\r\n\r\nNo range."));
    TEST_ASSERT_FALSE_MESSAGE(exists(part), "the next attempt must not ask for that range again");
    TEST_ASSERT_FALSE(exists(state_file));
}

static void test_part_longer_than_the_file_is_never_finished(void) {
    fake_route *route = serve(ETAG);
    route->fail_after_bytes = BODY_BYTES;
    route->fail_mid_body = SKIFF_ERR_NET_CONNECTION_LOST;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECTION_LOST, attempt());
    route->fail_mid_body = SKIFF_OK;
    assert_durable_prefix(BODY_BYTES);
    skiff_file *file = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK,
                          skiff_storage_open(posix, part, SKIFF_FILE_WRITE_AT, BODY_BYTES, &file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_write(file, "stale", 5));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_file_close(file));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_FALSE_MESSAGE(last_request()->has_range, "an oversized .part starts over");
    assert_complete();
}

static void test_complete_part_is_finished_without_a_request(void) {
    fake_route *route = serve(ETAG);
    route->fail_after_bytes = BODY_BYTES;
    route->fail_mid_body = SKIFF_ERR_NET_CONNECTION_LOST;
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_NET_CONNECTION_LOST, attempt(),
                                  "every byte came, then the connection broke");
    assert_durable_prefix(BODY_BYTES);
    const size_t requests = transport.request_count;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_EQUAL_size_t_MESSAGE(requests, transport.request_count, "nothing left to fetch");
    assert_complete();
}

static void test_failed_rename_is_finished_by_the_next_attempt(void) {
    serve(ETAG);
    storage.rename_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, attempt());
    TEST_ASSERT_FALSE(result.complete);
    assert_durable_prefix(BODY_BYTES);
    storage.rename_error = SKIFF_OK;
    const size_t requests = transport.request_count;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_EQUAL_size_t(requests, transport.request_count);
    assert_complete();
}

static void test_body_ending_early_without_an_error_is_a_lost_connection(void) {
    serve_on(&transport, "HTTP/1.1 200 OK", ETAG, 0, (size_t)(BODY_BYTES - 10));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_CONNECTION_LOST, attempt());
    assert_durable_prefix(BODY_BYTES - 10);
}

static void test_more_bytes_than_expected_are_refused(void) {
    serve_on(&transport, "HTTP/1.1 200 OK", ETAG, 0, (size_t)(BODY_BYTES + 10));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, attempt());
    TEST_ASSERT_FALSE(exists(target));
    TEST_ASSERT_TRUE_MESSAGE(saved_state().offset <= BODY_BYTES, "never more than the file");
}

static void test_wrong_content_length_is_refused_before_writing(void) {
    serve_on(&transport, "HTTP/1.1 200 OK", ETAG, 1, (size_t)(BODY_BYTES - 1));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, attempt());
    TEST_ASSERT_FALSE(exists(part));
    TEST_ASSERT_FALSE(exists(state_file));
}

static void test_empty_success_is_a_bad_response(void) {
    TEST_ASSERT_NOT_NULL(fake_transport_add_raw(&transport, CONTENT_PATH, "HTTP/1.1 200 OK\r\n\r\n",
                                                strlen("HTTP/1.1 200 OK\r\n\r\n")));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_BAD_RESPONSE, attempt());
}

static void test_checksum_mismatch_deletes_everything(void) {
    serve(ETAG);
    spec.expected_crc32 = body_crc ^ 1U;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_ROMM_CHECKSUM, attempt());
    TEST_ASSERT_FALSE(result.complete);
    TEST_ASSERT_FALSE(exists(part));
    TEST_ASSERT_FALSE(exists(state_file));
    TEST_ASSERT_FALSE(exists(target));
}

static void test_without_an_expected_crc_any_content_is_kept(void) {
    serve(ETAG);
    spec.has_expected_crc32 = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    assert_complete();
}

static void test_memory_stick_full_keeps_the_last_durable_checkpoint(void) {
    serve(ETAG);
    storage.fail_suffix = SKIFF_DOWNLOAD_PART_SUFFIX;
    storage.write_budget = 6 * MIB;
    storage.write_error = SKIFF_ERR_STORAGE_NO_SPACE;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_NO_SPACE, attempt());
    assert_durable_prefix(SKIFF_DOWNLOAD_CHECKPOINT_BYTES);
    storage.write_error = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_EQUAL_UINT64(SKIFF_DOWNLOAD_CHECKPOINT_BYTES, result.resumed_from);
    assert_complete();
}

static void test_handle_lost_to_a_suspend_costs_one_attempt(void) {
    serve(ETAG);
    storage.fail_suffix = SKIFF_DOWNLOAD_PART_SUFFIX;
    /* Write-buffer blocks: the handle goes stale after 5 MiB. */
    storage.stale_after_writes = (int)(5 * MIB / SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, attempt());
    TEST_ASSERT_EQUAL_INT(1, storage.handles_lost);
    assert_durable_prefix(SKIFF_DOWNLOAD_CHECKPOINT_BYTES);
    storage.stale_after_writes = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    assert_complete();
}

static void test_failing_sync_never_advances_the_saved_offset(void) {
    fake_route *route = serve(ETAG);
    cut_after(route, 1000);
    storage.fail_suffix = SKIFF_DOWNLOAD_PART_SUFFIX;
    storage.sync_error = SKIFF_ERR_STORAGE_IO;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_IO, attempt());
    assert_durable_prefix(1000);
    storage.sync_error = SKIFF_OK;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_EQUAL_UINT64(1000, result.resumed_from);
    assert_complete();
}

static void test_untrustworthy_progress_starts_fresh(void) {
    static const char GARBAGE[] = "version=1\nsize=12";
    fake_route *route = serve(ETAG);

    cut_after(route, 5 * MIB);
    write_all(state_file, GARBAGE, sizeof GARBAGE - 1);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_FALSE_MESSAGE(last_request()->has_range, "a torn .resume file is not trusted");
    assert_complete();

    cut_after(route, 5 * MIB);
    write_all(part, body, 1000);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_FALSE_MESSAGE(last_request()->has_range, "a .part shorter than the saved offset");
    assert_complete();

    cut_after(route, 5 * MIB);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_storage_remove(posix, part));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_FALSE_MESSAGE(last_request()->has_range, "no .part at all");
    assert_complete();

    cut_after(route, 5 * MIB);
    spec.has_expected_crc32 = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    TEST_ASSERT_FALSE_MESSAGE(last_request()->has_range, "progress saved for another file");
    assert_complete();
}

static void test_existing_target_is_replaced(void) {
    serve(ETAG);
    write_all(target, "old", 3);
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    assert_complete();
}

/* Stops the transfer with SKIFF_ERR_NET_UNAVAILABLE once `after` polls have passed. */
typedef struct stop_after {
    int polls;
    int after;
} stop_after;

static skiff_err stop_when_due(void *ctx) {
    stop_after *stop = ctx;
    return ++stop->polls > stop->after ? SKIFF_ERR_NET_UNAVAILABLE : SKIFF_OK;
}

static void test_stop_hook_ends_the_attempt_and_keeps_the_progress(void) {
    serve(ETAG);
    /* One poll before the response, then one per 1 KiB chunk. */
    stop_after stop = {0, 3001};
    spec.should_stop = stop_when_due;
    spec.stop_ctx = &stop;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_NET_UNAVAILABLE, attempt());
    assert_durable_prefix((uint64_t)3000 * FAKE_TRANSPORT_CHUNK_BYTES);
    spec.should_stop = NULL;
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, attempt());
    assert_complete();
}

static void test_retryable_failures(void) {
    static const skiff_err RETRYABLE[] = {
        SKIFF_ERR_NET_UNAVAILABLE, SKIFF_ERR_NET_DNS,       SKIFF_ERR_NET_CONNECT,
        SKIFF_ERR_NET_TIMEOUT,     SKIFF_ERR_NET_WIFI_JOIN, SKIFF_ERR_NET_CONNECTION_LOST,
    };
    static const skiff_err FINAL[] = {
        SKIFF_OK,
        SKIFF_ERR_NET_TLS_HANDSHAKE,
        SKIFF_ERR_NET_TLS_UNTRUSTED,
        SKIFF_ERR_NET_TLS_CLOCK,
        SKIFF_ERR_NET_NEEDS_ARK,
        SKIFF_ERR_ROMM_UNAUTHORIZED,
        SKIFF_ERR_ROMM_BAD_RESPONSE,
        SKIFF_ERR_ROMM_CHECKSUM,
        SKIFF_ERR_STORAGE_NO_SPACE,
        SKIFF_ERR_STORAGE_IO,
    };
    for (size_t i = 0; i < sizeof RETRYABLE / sizeof RETRYABLE[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(skiff_download_retryable(RETRYABLE[i]),
                                 skiff_err_name(RETRYABLE[i]));
    }
    for (size_t i = 0; i < sizeof FINAL / sizeof FINAL[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(skiff_download_retryable(FINAL[i]), skiff_err_name(FINAL[i]));
    }
}

static void test_arguments_and_limits(void) {
    char long_path[SKIFF_DOWNLOAD_PATH_MAX];
    memset(long_path, 'a', sizeof long_path - 3);
    long_path[sizeof long_path - 3] = '\0';
    serve(ETAG);
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_download_attempt(NULL, &storage.base, &spec, &result));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_download_attempt(&transport.base, NULL, &spec, &result));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_download_attempt(&transport.base, &storage.base, NULL, &result));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG,
                          skiff_download_attempt(&transport.base, &storage.base, &spec, NULL));
    spec.expected_size = 0;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, attempt());
    spec.expected_size = SKIFF_DOWNLOAD_MAX_BYTES + 1;
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_STORAGE_FILE_TOO_LARGE, attempt());
    spec.expected_size = BODY_BYTES;
    spec.url = "";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, attempt());
    spec.url = CONTENT_URL;
    spec.target_path = "";
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, attempt());
    spec.target_path = long_path;
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_ERR_INVALID_ARG, attempt(), "no room for the suffixes");
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, transport.request_count, "nothing was sent");

    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_download_discard(NULL, target));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_download_discard(&storage.base, ""));
    TEST_ASSERT_EQUAL_INT(SKIFF_ERR_INVALID_ARG, skiff_download_discard(&storage.base, long_path));
}

static void test_discard_removes_the_progress(void) {
    fake_route *route = serve(ETAG);
    cut_after(route, 5 * MIB);
    TEST_ASSERT_TRUE(exists(part));
    TEST_ASSERT_EQUAL_INT(SKIFF_OK, skiff_download_discard(&storage.base, target));
    TEST_ASSERT_FALSE(exists(part));
    TEST_ASSERT_FALSE(exists(state_file));
    TEST_ASSERT_EQUAL_INT_MESSAGE(SKIFF_OK, skiff_download_discard(&storage.base, target),
                                  "nothing left to discard is fine");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fresh_download_completes_and_is_renamed);
    RUN_TEST(test_cut_mid_body_saves_the_exact_offset_and_resumes);
    RUN_TEST(test_cut_before_the_first_checkpoint_and_at_the_very_start);
    RUN_TEST(test_cut_while_resuming_keeps_both_parts);
    RUN_TEST(test_stale_etag_restarts_with_the_whole_file);
    RUN_TEST(test_a_restart_rewrites_the_state_before_any_new_byte);
    RUN_TEST(test_server_ignoring_ranges_restarts);
    RUN_TEST(test_weak_or_missing_etag_never_resumes);
    RUN_TEST(test_error_pages_never_reach_the_part_file);
    RUN_TEST(test_partial_response_that_does_not_fit_is_refused);
    RUN_TEST(test_partial_response_with_another_total_is_refused);
    RUN_TEST(test_unasked_partial_response_is_refused);
    RUN_TEST(test_range_no_longer_satisfiable_discards_the_progress);
    RUN_TEST(test_range_no_longer_satisfiable_with_an_error_page_discards_too);
    RUN_TEST(test_part_longer_than_the_file_is_never_finished);
    RUN_TEST(test_complete_part_is_finished_without_a_request);
    RUN_TEST(test_failed_rename_is_finished_by_the_next_attempt);
    RUN_TEST(test_body_ending_early_without_an_error_is_a_lost_connection);
    RUN_TEST(test_more_bytes_than_expected_are_refused);
    RUN_TEST(test_wrong_content_length_is_refused_before_writing);
    RUN_TEST(test_empty_success_is_a_bad_response);
    RUN_TEST(test_checksum_mismatch_deletes_everything);
    RUN_TEST(test_without_an_expected_crc_any_content_is_kept);
    RUN_TEST(test_memory_stick_full_keeps_the_last_durable_checkpoint);
    RUN_TEST(test_handle_lost_to_a_suspend_costs_one_attempt);
    RUN_TEST(test_failing_sync_never_advances_the_saved_offset);
    RUN_TEST(test_untrustworthy_progress_starts_fresh);
    RUN_TEST(test_existing_target_is_replaced);
    RUN_TEST(test_stop_hook_ends_the_attempt_and_keeps_the_progress);
    RUN_TEST(test_retryable_failures);
    RUN_TEST(test_arguments_and_limits);
    RUN_TEST(test_discard_removes_the_progress);
    return UNITY_END();
}
