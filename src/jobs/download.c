#include "skiff/download.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define WEAK_ETAG_PREFIX "W/"

enum {
    HTTP_STATUS_OK = 200,
    HTTP_STATUS_PARTIAL = 206,
    HTTP_STATUS_RANGE_NOT_SATISFIABLE = 416,
};

/* One attempt, shared with the body callback. */
typedef struct download {
    skiff_storage *storage;
    const skiff_download_spec *spec;
    skiff_download_result *result;
    char part_path[SKIFF_DOWNLOAD_PATH_MAX];
    char state_path[SKIFF_DOWNLOAD_PATH_MAX];
    /* What is durable: state.offset bytes of the .part file and their CRC-32. */
    skiff_download_state state;
    /* What has been accepted, in the file or still in the buffer. */
    uint64_t received;
    uint32_t crc32;
    uint64_t next_checkpoint;
    int range_requested;
    /* The response passed the checks and the .part file is open. */
    skiff_file *part;
    unsigned char *buffer;
    size_t buffered;
    /* The status or size was refused before anything was written. */
    int refused;
    /* A Memory Stick call failed: the progress cannot be trusted to save. */
    int storage_failed;
} download;

static int is_strong_etag(const char *etag) {
    return etag[0] != '\0' && strncmp(etag, WEAK_ETAG_PREFIX, strlen(WEAK_ETAG_PREFIX)) != 0;
}

static skiff_err remove_if_present(skiff_storage *storage, const char *path) {
    const skiff_err err = skiff_storage_remove(storage, path);
    return err == SKIFF_ERR_STORAGE_NOT_FOUND ? SKIFF_OK : err;
}

/* "<target><suffix>" into out; 0 if it does not fit. */
static int sibling_path(const char *target, const char *suffix, char *out) {
    const int written = snprintf(out, SKIFF_DOWNLOAD_PATH_MAX, "%s%s", target, suffix);
    return written > 0 && written < SKIFF_DOWNLOAD_PATH_MAX;
}

/* The folder of target ("ms0:/ISO" for "ms0:/ISO/Game.iso") into out; 0 without one. */
static int folder_of(const char *target, char *out) {
    const char *last_slash = strrchr(target, '/');
    if (last_slash == NULL || last_slash == target) {
        return 0;
    }
    const size_t length = (size_t)(last_slash - target);
    memcpy(out, target, length);
    out[length] = '\0';
    return 1;
}

/*
 * Whether what is still to come fits, with the storage layer's margin, before any request: a
 * missing folder or Memory Stick fails here too. Only a device that cannot report its free space
 * is let through, to the writes, which fail with SKIFF_ERR_STORAGE_NO_SPACE when it fills up.
 */
static skiff_err check_room(download *d) {
    char folder[SKIFF_DOWNLOAD_PATH_MAX];
    if (!folder_of(d->spec->target_path, folder)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const skiff_err err =
        skiff_storage_check_room(d->storage, folder, d->spec->expected_size - d->state.offset);
    return err == SKIFF_ERR_NOT_IMPLEMENTED ? SKIFF_OK : err;
}

static skiff_err storage_step(download *d, skiff_err err) {
    if (err != SKIFF_OK) {
        d->storage_failed = 1;
    }
    return err;
}

/* Rewrites the .resume file and makes it durable. */
static skiff_err save_state(download *d) {
    char text[SKIFF_DOWNLOAD_STATE_MAX];
    size_t length = 0;
    skiff_err err = skiff_download_state_format(&d->state, text, sizeof text, &length);
    if (err != SKIFF_OK) {
        return err;
    }
    skiff_file *file = NULL;
    err = skiff_storage_open(d->storage, d->state_path, SKIFF_FILE_REPLACE, 0, &file);
    if (err == SKIFF_OK) {
        err = skiff_file_write(file, text, length);
    }
    if (err == SKIFF_OK) {
        err = skiff_file_sync(file);
    }
    const skiff_err closed = skiff_file_close(file);
    return storage_step(d, err != SKIFF_OK ? err : closed);
}

static skiff_err write_buffer(download *d) {
    if (d->buffered == 0) {
        return SKIFF_OK;
    }
    const skiff_err err = skiff_file_write(d->part, d->buffer, d->buffered);
    d->buffered = 0;
    if (err == SKIFF_OK && d->spec->after_write != NULL) {
        d->spec->after_write(d->spec->write_ctx);
    }
    return storage_step(d, err);
}

/* Everything accepted reaches the device, then the .resume file says so. */
static skiff_err checkpoint(download *d) {
    skiff_err err = write_buffer(d);
    if (err == SKIFF_OK) {
        err = storage_step(d, skiff_file_sync(d->part));
    }
    if (err != SKIFF_OK) {
        return err;
    }
    d->state.offset = d->received;
    d->state.crc32 = d->crc32;
    d->next_checkpoint = d->received + SKIFF_DOWNLOAD_CHECKPOINT_BYTES;
    return save_state(d);
}

/* A durable state for the same file, whose .part file holds at least its offset and no more than
 * the whole file (bytes past the offset are rewritten; bytes past the end would survive). */
static int state_usable(const download *d, const skiff_download_state *state) {
    uint64_t part_size = 0;
    return state->size == d->spec->expected_size &&
           state->has_expected_crc32 == d->spec->has_expected_crc32 &&
           (!state->has_expected_crc32 || state->expected_crc32 == d->spec->expected_crc32) &&
           is_strong_etag(state->etag) &&
           skiff_storage_size(d->storage, d->part_path, &part_size) == SKIFF_OK &&
           part_size >= state->offset && part_size <= state->size;
}

/* The saved progress, or a fresh start (and no stale files) when there is none to trust. Only a
 * Memory Stick failure is an error. */
static skiff_err load_state(download *d) {
    char text[SKIFF_DOWNLOAD_STATE_MAX + 1];
    size_t length = 0;
    skiff_file *file = NULL;
    skiff_err err = skiff_storage_open(d->storage, d->state_path, SKIFF_FILE_READ, 0, &file);
    if (err == SKIFF_OK) {
        err = skiff_file_read(file, text, sizeof text, &length);
        const skiff_err closed = skiff_file_close(file);
        err = err != SKIFF_OK ? err : closed;
    }
    if (err != SKIFF_OK && err != SKIFF_ERR_STORAGE_NOT_FOUND) {
        return err;
    }
    skiff_download_state saved;
    if (err == SKIFF_OK && skiff_download_state_parse(text, length, &saved) == SKIFF_OK &&
        state_usable(d, &saved)) {
        d->state = saved;
        return SKIFF_OK;
    }
    memset(&d->state, 0, sizeof d->state);
    d->state.size = d->spec->expected_size;
    d->state.has_expected_crc32 = d->spec->has_expected_crc32;
    d->state.expected_crc32 = d->spec->expected_crc32;
    err = remove_if_present(d->storage, d->part_path);
    return err == SKIFF_OK ? remove_if_present(d->storage, d->state_path) : err;
}

/* Starts writing at offset: the .part file as it is (206), or emptied (200). */
static skiff_err open_part(download *d, skiff_file_mode mode, uint64_t offset, uint32_t crc32) {
    const skiff_err err = skiff_storage_open(d->storage, d->part_path, mode, offset, &d->part);
    if (err != SKIFF_OK) {
        return storage_step(d, err);
    }
    d->received = offset;
    d->crc32 = crc32;
    d->next_checkpoint = offset + SKIFF_DOWNLOAD_CHECKPOINT_BYTES;
    return SKIFF_OK;
}

static skiff_err refuse(download *d, skiff_err err) {
    d->refused = 1;
    return err;
}

/* Called with the first body bytes: the status and headers decide where they go, before anything
 * touches the Memory Stick. */
static skiff_err accept_response(download *d) {
    const skiff_http_response *response = &d->result->response;
    const uint64_t size = d->spec->expected_size;
    if (response->status == HTTP_STATUS_PARTIAL) {
        if (!d->range_requested || !response->has_content_range ||
            response->range_start != d->state.offset || response->range_total != size) {
            return refuse(d, SKIFF_ERR_ROMM_BAD_RESPONSE);
        }
        return open_part(d, SKIFF_FILE_WRITE_AT, d->state.offset, d->state.crc32);
    }
    if (response->status != HTTP_STATUS_OK) {
        const skiff_err err = skiff_http_status_error(response->status);
        return refuse(d, err != SKIFF_OK ? err : SKIFF_ERR_ROMM_BAD_RESPONSE);
    }
    if (response->has_content_length && response->content_length != size) {
        return refuse(d, SKIFF_ERR_ROMM_BAD_RESPONSE);
    }
    /* The whole file: whatever was saved belongs to another version, or the server ignores ranges.
     * The .resume file is rewritten first, so it never vouches for bytes from the old version. */
    d->result->restarted = d->range_requested;
    d->state.offset = 0;
    d->state.crc32 = 0;
    snprintf(d->state.etag, sizeof d->state.etag, "%s",
             is_strong_etag(response->etag) ? response->etag : "");
    const skiff_err err = save_state(d);
    return err != SKIFF_OK ? err : open_part(d, SKIFF_FILE_REPLACE, 0, 0);
}

static skiff_err on_body(void *ctx, const unsigned char *data, size_t size) {
    download *d = ctx;
    if (d->part == NULL) {
        const skiff_err err = accept_response(d);
        if (err != SKIFF_OK) {
            return err;
        }
    }
    if (size > d->spec->expected_size - d->received) {
        return refuse(d, SKIFF_ERR_ROMM_BAD_RESPONSE);
    }
    d->crc32 = (uint32_t)crc32(d->crc32, data, (uInt)size);
    d->received += size;
    d->result->bytes_received += size;
    while (size > 0) {
        const size_t room = SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES - d->buffered;
        const size_t taken = size < room ? size : room;
        memcpy(d->buffer + d->buffered, data, taken);
        d->buffered += taken;
        data += taken;
        size -= taken;
        if (d->buffered == SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES) {
            const skiff_err err = write_buffer(d);
            if (err != SKIFF_OK) {
                return err;
            }
        }
    }
    if (d->received >= d->next_checkpoint) {
        const skiff_err err = checkpoint(d);
        if (err != SKIFF_OK) {
            return err;
        }
    }
    if (d->spec->on_progress != NULL) {
        d->spec->on_progress(d->spec->progress_ctx, d->received, d->spec->expected_size);
    }
    return SKIFF_OK;
}

/* Closes the .part file; first saves the progress unless the Memory Stick already failed. */
static skiff_err close_part(download *d, int save) {
    skiff_err err = SKIFF_OK;
    if (save && !d->storage_failed && d->received > d->state.offset) {
        err = checkpoint(d);
    }
    const skiff_err closed = storage_step(d, skiff_file_close(d->part));
    d->part = NULL;
    return err != SKIFF_OK ? err : closed;
}

/* Drops the progress and returns `reason`, or the Memory Stick's error if it could not. */
static skiff_err discard_with(download *d, skiff_err reason) {
    const skiff_err err = skiff_download_discard(d->storage, d->spec->target_path);
    return err != SKIFF_OK ? err : reason;
}

/* Every byte is in: check it, and put it under the final name. */
static skiff_err finish(download *d) {
    uint64_t part_size = 0;
    skiff_err err = skiff_storage_size(d->storage, d->part_path, &part_size);
    if (err != SKIFF_OK) {
        return err;
    }
    if (part_size != d->spec->expected_size) {
        return discard_with(d, SKIFF_ERR_STORAGE_IO);
    }
    if (d->spec->has_expected_crc32 && d->state.crc32 != d->spec->expected_crc32) {
        return discard_with(d, SKIFF_ERR_ROMM_CHECKSUM);
    }
    if (!d->spec->replace_target) {
        /* Checked now, not when the download was planned: a file copied there meanwhile (over USB,
         * between two launches) is never Skiff's to remove. */
        uint64_t existing = 0;
        err = skiff_storage_size(d->storage, d->spec->target_path, &existing);
        if (err == SKIFF_OK) {
            return SKIFF_ERR_STORAGE_NAME_TAKEN;
        }
        if (err != SKIFF_ERR_STORAGE_NOT_FOUND) {
            return err;
        }
    }
    err = remove_if_present(d->storage, d->spec->target_path);
    if (err == SKIFF_OK) {
        err = skiff_storage_rename(d->storage, d->part_path, d->spec->target_path);
    }
    if (err != SKIFF_OK) {
        return err;
    }
    /* Best effort: a .resume file left behind has no .part file, so it is never trusted again. */
    remove_if_present(d->storage, d->state_path);
    d->result->complete = 1;
    return SKIFF_OK;
}

/* After the transfer: what the response and the bytes that came mean for the file. */
static skiff_err conclude(download *d, skiff_err err) {
    const skiff_http_response *response = &d->result->response;
    /* A range the server no longer has, with or without an error page: the progress is useless. */
    if (d->part == NULL && response->status == HTTP_STATUS_RANGE_NOT_SATISFIABLE) {
        return discard_with(d, SKIFF_ERR_ROMM_BAD_RESPONSE);
    }
    if (d->part != NULL) {
        const int whole = err == SKIFF_OK && d->received == d->spec->expected_size;
        const skiff_err closed = close_part(d, 1);
        if (err != SKIFF_OK || closed != SKIFF_OK) {
            return err != SKIFF_OK ? err : closed;
        }
        return whole ? finish(d) : SKIFF_ERR_NET_CONNECTION_LOST;
    }
    if (err != SKIFF_OK || d->refused) {
        return err;
    }
    /* No body at all: an empty error page. */
    const skiff_err status_err = skiff_http_status_error(response->status);
    return status_err != SKIFF_OK ? status_err : SKIFF_ERR_ROMM_BAD_RESPONSE;
}

static int spec_valid(const skiff_download_spec *spec) {
    return spec->url != NULL && spec->url[0] != '\0' && spec->target_path != NULL &&
           spec->target_path[0] != '\0' && spec->expected_size > 0;
}

skiff_err skiff_download_attempt(skiff_transport *transport, skiff_storage *storage,
                                 const skiff_download_spec *spec, skiff_download_result *result) {
    if (result != NULL) {
        memset(result, 0, sizeof *result);
    }
    if (transport == NULL || storage == NULL || spec == NULL || result == NULL ||
        !spec_valid(spec)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (spec->expected_size > SKIFF_DOWNLOAD_MAX_BYTES) {
        return SKIFF_ERR_STORAGE_FILE_TOO_LARGE;
    }
    download d;
    memset(&d, 0, sizeof d);
    d.storage = storage;
    d.spec = spec;
    d.result = result;
    if (!sibling_path(spec->target_path, SKIFF_DOWNLOAD_PART_SUFFIX, d.part_path) ||
        !sibling_path(spec->target_path, SKIFF_DOWNLOAD_STATE_SUFFIX, d.state_path)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_err err = load_state(&d);
    if (err != SKIFF_OK) {
        return err;
    }
    result->resumed_from = d.state.offset;
    if (d.state.offset == spec->expected_size) {
        /* Everything arrived before, but the attempt ended before the rename. */
        return finish(&d);
    }
    err = check_room(&d);
    if (err != SKIFF_OK) {
        return err;
    }
    d.buffer = malloc(SKIFF_DOWNLOAD_WRITE_BUFFER_BYTES);
    if (d.buffer == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    d.range_requested = d.state.offset > 0;
    const skiff_http_request request = {
        .url = spec->url,
        .headers = spec->headers,
        .header_count = spec->header_count,
        .has_range = d.range_requested,
        .range_start = d.state.offset,
        .if_range = d.range_requested ? d.state.etag : NULL,
        .on_body = on_body,
        .body_ctx = &d,
        .should_stop = spec->should_stop,
        .stop_ctx = spec->stop_ctx,
    };
    err = conclude(&d, skiff_transport_perform(transport, &request, &result->response));
    free(d.buffer);
    return err;
}

int skiff_download_retryable(skiff_err err) {
    switch (err) {
    case SKIFF_ERR_NET_UNAVAILABLE:
    case SKIFF_ERR_NET_DNS:
    case SKIFF_ERR_NET_CONNECT:
    case SKIFF_ERR_NET_TIMEOUT:
    case SKIFF_ERR_NET_WIFI_JOIN:
    case SKIFF_ERR_NET_CONNECTION_LOST:
        return 1;
    default:
        return 0;
    }
}

skiff_err skiff_download_discard(skiff_storage *storage, const char *target_path) {
    char part_path[SKIFF_DOWNLOAD_PATH_MAX];
    char state_path[SKIFF_DOWNLOAD_PATH_MAX];
    if (storage == NULL || target_path == NULL || target_path[0] == '\0' ||
        !sibling_path(target_path, SKIFF_DOWNLOAD_PART_SUFFIX, part_path) ||
        !sibling_path(target_path, SKIFF_DOWNLOAD_STATE_SUFFIX, state_path)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const skiff_err part_err = remove_if_present(storage, part_path);
    const skiff_err state_err = remove_if_present(storage, state_path);
    return part_err != SKIFF_OK ? part_err : state_err;
}
