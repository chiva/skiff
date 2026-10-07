#ifndef SKIFF_DOWNLOAD_H
#define SKIFF_DOWNLOAD_H

/*
 * One resumable download into a file on the Memory Stick. Bytes land in "<target>.part" and are
 * renamed to the target only once complete and checked. "<target>.resume" records how many bytes of
 * the .part file are known to be on the device, the CRC-32 up to there and the ETag, so a download
 * cut by Wi-Fi, a suspend or a quit continues where it stopped: Range from that offset with
 * If-Range set to the ETag, so a file that changed on the server comes back whole (200) and starts
 * over instead of being stitched from two versions.
 *
 * One call is one attempt. The caller (jobs/, or a hardware probe) decides whether to try again:
 * skiff_download_retryable() says which failures a reconnect can fix, and every attempt opens its
 * own files, so a file handle lost to a suspend costs only the attempt it happened in.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"
#include "skiff/http.h"
#include "skiff/storage.h"
#include "skiff/transport.h"

#define SKIFF_DOWNLOAD_PART_SUFFIX ".part"
#define SKIFF_DOWNLOAD_STATE_SUFFIX ".resume"
/* Room for a target path with either suffix and its terminator. */
#define SKIFF_DOWNLOAD_PATH_MAX 256
/* FAT32 cannot hold a file of 4 GiB or more. */
#define SKIFF_DOWNLOAD_MAX_BYTES SKIFF_STORAGE_MAX_FILE_BYTES
/* Writes go to the Memory Stick in blocks this large. While the Memory Stick writes, Wi-Fi data
 * stops arriving and the transfer takes a while to pick up again, so fewer, larger writes are
 * faster: on a PSP-1000 at 333 MHz, 64 MiB downloaded at 386 KB/s with 1 MiB writes and 296 KB/s
 * with 128 KB ones (a writer thread did not help: 310 KB/s). */
#define SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES ((size_t)1024 * 1024)
/* How often the progress is made durable: at most this much is downloaded again after a power cut
 * (about 9 s at 470 KB/s). */
#define SKIFF_DOWNLOAD_CHECKPOINT_BYTES ((uint64_t)4 * 1024 * 1024)

/* ---- The .resume file ---- */

#define SKIFF_DOWNLOAD_STATE_VERSION 1
/* The longest .resume file: every line at its widest, the ETag at SKIFF_HTTP_ETAG_MAX. */
#define SKIFF_DOWNLOAD_STATE_MAX 320

typedef struct skiff_download_state {
    /* The download's expected size and CRC-32, so a state left by another file is not reused. */
    uint64_t size;
    int has_expected_crc32;
    uint32_t expected_crc32;
    /* A strong ETag, or empty: without one a download cannot resume. */
    char etag[SKIFF_HTTP_ETAG_MAX];
    /* Bytes of the .part file known to be on the device, and the CRC-32 of exactly those. */
    uint64_t offset;
    uint32_t crc32;
} skiff_download_state;

/*
 * Writes state as text ("key=value" lines ending in a "check=" line, the CRC-32 of the lines
 * before it) into out and its length into *length. SKIFF_ERR_INVALID_ARG for a NULL argument, an
 * ETag with a line break or an offset past size; SKIFF_ERR_BUFFER_TOO_SMALL if it does not fit.
 */
skiff_err skiff_download_state_format(const skiff_download_state *state, char *out, size_t out_size,
                                      size_t *length);

/*
 * Reads what skiff_download_state_format() wrote. The file is rewritten in place (FAT has no atomic
 * replace), so anything else is refused with SKIFF_ERR_INVALID_ARG: a check that does not match,
 * as after a write cut short by a power loss, a missing, repeated or reordered line, a malformed
 * number, an unknown version, or an offset past size.
 */
skiff_err skiff_download_state_parse(const char *text, size_t length, skiff_download_state *out);

/* ---- Downloading ---- */

/* Told after every chunk how many bytes of total the target holds so far, resumed ones included. */
typedef void (*skiff_download_progress_fn)(void *ctx, uint64_t done, uint64_t total);

/* Told right after a block reached the Memory Stick: Wi-Fi reception is paused during the write
 * anyway, so other writes (the log's) cost the transfer nothing extra now. */
typedef void (*skiff_download_write_fn)(void *ctx);

typedef struct skiff_download_spec {
    /* The file's URL and the request headers (an Authorization header, for RomM). */
    const char *url;
    const skiff_http_header *headers;
    size_t header_count;
    /* Where the file goes; the .part and .resume files sit next to it. */
    const char *target_path;
    /* 0: a file already at target_path is never removed: the finished download stops with
     * SKIFF_ERR_STORAGE_NAME_TAKEN and keeps its .part and .resume files, so the next attempt
     * finishes it once the file is moved. Nonzero: that file is Skiff's own earlier copy of this
     * download (skiff/install.h) and is replaced. */
    int replace_target;
    /* What RomM records for the file: its size (required) and, when known, its CRC-32. */
    uint64_t expected_size;
    int has_expected_crc32;
    uint32_t expected_crc32;
    /* Passed to the transport (skiff/transport.h); NULL for none. */
    skiff_http_stop_fn should_stop;
    void *stop_ctx;
    skiff_download_progress_fn on_progress;
    void *progress_ctx;
    /* NULL for none. */
    skiff_download_write_fn after_write;
    void *write_ctx;
} skiff_download_spec;

typedef struct skiff_download_result {
    /* Where this attempt started: 0 for the whole file. */
    uint64_t resumed_from;
    /* Body bytes this attempt received. */
    uint64_t bytes_received;
    /* The server sent the whole file although a range was asked for: the ETag changed, or the
     * server ignores ranges. */
    int restarted;
    /* The target is in place and matched the expected size and CRC-32. */
    int complete;
    /* The last response (status, ETag, TLS version and cipher), for logs. */
    skiff_http_response response;
} skiff_download_result;

/*
 * Runs one attempt and fills result. SKIFF_OK means the target is complete; otherwise the progress
 * made is kept for the next attempt, except as noted:
 *   - SKIFF_ERR_INVALID_ARG: a NULL argument, a missing URL or target, a target without a folder,
 *     a zero size, a path too long for SKIFF_DOWNLOAD_PATH_MAX;
 *   - SKIFF_ERR_STORAGE_FILE_TOO_LARGE: expected_size above SKIFF_DOWNLOAD_MAX_BYTES;
 *   - before any request, skiff_storage_check_room()'s error for the target's folder:
 *     SKIFF_ERR_STORAGE_NO_SPACE when it has less free space than what is still to come plus
 *     SKIFF_STORAGE_FREE_MARGIN_BYTES, SKIFF_ERR_STORAGE_NOT_FOUND when the folder is missing; a
 *     device that cannot tell its free space (SKIFF_ERR_NOT_IMPLEMENTED) is not refused;
 *   - the transport's error (skiff_transport_perform()), or the stop hook's;
 *   - the RomM error for an HTTP status other than 200 or 206 (skiff_http_status_error()); the
 *     .part file is not touched;
 *   - SKIFF_ERR_ROMM_BAD_RESPONSE: a size, range or status that does not fit the file; a 416 also
 *     discards the progress, since the server no longer has what it was resuming;
 *   - SKIFF_ERR_NET_CONNECTION_LOST: the body ended early without a transport error;
 *   - SKIFF_ERR_ROMM_CHECKSUM: the whole file arrived but its CRC-32 is not the expected one; the
 *     .part file and the progress are deleted, so the next attempt starts over;
 *   - SKIFF_ERR_STORAGE_IO also when the finished .part file is not exactly the expected size; it
 * is deleted with the progress, so the next attempt starts over;
 *   - SKIFF_ERR_STORAGE_NAME_TAKEN: the whole file arrived and checked out, but a file is already
 * at the target and replace_target is 0; the progress is kept;
 *   - a storage error (3xx) from the Memory Stick.
 */
skiff_err skiff_download_attempt(skiff_transport *transport, skiff_storage *storage,
                                 const skiff_download_spec *spec, skiff_download_result *result);

/* 1 for the failures a later attempt can get past once the network is back: Wi-Fi off or not
 * joined, name lookup, connect, timeout, connection lost. 0 for everything else. */
int skiff_download_retryable(skiff_err err);

/* Deletes the .part and .resume files of target_path (a cancelled download). Files that do not
 * exist are not an error. */
skiff_err skiff_download_discard(skiff_storage *storage, const char *target_path);

#endif
