#ifndef SKIFF_LOG_H
#define SKIFF_LOG_H

/*
 * The log file players attach to bug reports (PSP/GAME/Skiff/skiff.log). A logger is an object the
 * app creates and hands to the layers that log (jobs/, app/); net/ and romm/ return errors instead.
 *
 * Lines are kept in memory and written in batches, because every Memory Stick write during a
 * download also pauses Wi-Fi reception (docs/development/hardware-findings.md): a batch goes out
 * when the buffer is full, after every warning or error (so they survive a crash), on
 * skiff_log_flush() and on skiff_log_destroy(). jobs/ calls skiff_log_flush() right after the
 * download engine's own write, while reception is already paused. Each batch opens the file,
 * writes, syncs and closes it again: a crash right after a warning cannot lose it, and a suspend,
 * which invalidates open files, cannot break the log.
 * Past cap_bytes the file is moved to "<path>.1" (replacing an older one) and a new one started.
 *
 * Secrets are redacted here, not by callers: every value registered with skiff_log_add_secret()
 * (the token, custom header values) is replaced by SKIFF_LOG_REDACTED wherever it appears in a
 * line, and skiff_log_header() shows only the values of headers known to be harmless. Control
 * characters are replaced, so text from a server cannot forge log lines. Logging never fails its
 * caller: a batch the Memory Stick refuses twice is dropped, and the next line says how many lines
 * were lost. Whatever part of it reached the file stays there (the file cannot be shortened), cut
 * off from the next batch by a line break.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/error.h"
#include "skiff/storage.h"

#define SKIFF_LOG_ROTATED_SUFFIX ".1"
/* Room for the log's path with SKIFF_LOG_ROTATED_SUFFIX and its terminator. */
#define SKIFF_LOG_PATH_MAX 256
/* Two files of at most this size stay on the Memory Stick. */
#define SKIFF_LOG_DEFAULT_CAP_BYTES ((uint64_t)256 * 1024)
#define SKIFF_LOG_DEFAULT_BUFFER_BYTES ((size_t)8 * 1024)
#define SKIFF_LOG_BUFFER_MAX ((size_t)64 * 1024)
/* The longest line, its newline included; longer ones are cut and end in SKIFF_LOG_CUT_MARKER. */
#define SKIFF_LOG_LINE_MAX 256
#define SKIFF_LOG_CUT_MARKER "..."
#define SKIFF_LOG_REDACTED "[redacted]"
/* Every message once a secret could not be registered (skiff_log_add_secret()). */
#define SKIFF_LOG_WITHHELD "[withheld: a secret could not be registered]"
/* Shorter values are too likely to appear in ordinary text to be found and replaced reliably. */
#define SKIFF_LOG_SECRET_MIN 8
#define SKIFF_LOG_SECRET_MAX 255
#define SKIFF_LOG_SECRETS_MAX 12
/* "2026-10-06 12:34:56.789Z" and its terminator. */
#define SKIFF_LOG_TIMESTAMP_MAX 25

/* In order of importance: a logger set to a level keeps that level and the ones before it. */
typedef enum skiff_log_level {
    SKIFF_LOG_ERROR,
    SKIFF_LOG_WARN,
    SKIFF_LOG_INFO,
    SKIFF_LOG_DEBUG,
} skiff_log_level;

/* The time now in milliseconds since 1970 (UTC); 0 when the clock cannot be read. On the PSP this
 * must come from the real-time clock: the C library's time() has no date there. */
typedef int (*skiff_log_clock_fn)(void *ctx, int64_t *unix_ms);
typedef void (*skiff_log_lock_fn)(void *ctx);

typedef struct skiff_log_config {
    /* Not owned; must outlive the logger. */
    skiff_storage *storage;
    /* Copied. */
    const char *path;
    /* 0 selects SKIFF_LOG_DEFAULT_CAP_BYTES and SKIFF_LOG_DEFAULT_BUFFER_BYTES. The buffer holds at
     * least one line and at most cap_bytes. */
    uint64_t cap_bytes;
    size_t buffer_bytes;
    skiff_log_level level;
    /* NULL: lines carry no timestamp. */
    skiff_log_clock_fn clock;
    void *clock_ctx;
    /* For a logger shared by threads: held around every call. Both or neither; NULL for one
     * thread. */
    skiff_log_lock_fn lock;
    skiff_log_lock_fn unlock;
    void *lock_ctx;
} skiff_log_config;

typedef struct skiff_log skiff_log;

/*
 * Creates a logger (free it with skiff_log_destroy()); nothing is written yet. Returns
 * SKIFF_ERR_INVALID_ARG for a NULL argument or storage, an empty path, an unknown level, a buffer
 * smaller than a line or larger than the cap or SKIFF_LOG_BUFFER_MAX, or only one of lock and
 * unlock; SKIFF_ERR_BUFFER_TOO_SMALL for a path too long for SKIFF_LOG_PATH_MAX;
 * SKIFF_ERR_NO_MEMORY. *out is NULL on error.
 */
skiff_err skiff_log_create(const skiff_log_config *config, skiff_log **out);

/* Writes what is buffered and frees the logger. Does nothing for NULL. */
void skiff_log_destroy(skiff_log *log);

/*
 * Adds the line "<timestamp> <E|W|I|D> <tag>: <message>" if level is kept. tag names the layer
 * ("jobs"). Errors go in as their stable name and number ("SKIFF_ERR_NET_TIMEOUT (103)"), never
 * the translated message. Does nothing for a NULL logger or format.
 */
__attribute__((format(printf, 4, 5))) void
skiff_log_write(skiff_log *log, skiff_log_level level, const char *tag, const char *format, ...);

/* Adds "<name>: <value>" for an HTTP header, with the value replaced by SKIFF_LOG_REDACTED unless
 * the header is one known to carry no secret (Content-Length, Content-Range, Content-Type, ETag).
 */
void skiff_log_header(skiff_log *log, skiff_log_level level, const char *tag, const char *name,
                      const char *value);

/*
 * Redacts value (copied) from every later line. Registering a value twice is not an error.
 * SKIFF_ERR_INVALID_ARG for a NULL argument or an empty value. A secret that cannot be registered
 * fails closed: SKIFF_ERR_INVALID_ARG for one shorter than SKIFF_LOG_SECRET_MIN,
 * SKIFF_ERR_BUFFER_TOO_SMALL for one longer than SKIFF_LOG_SECRET_MAX or past
 * SKIFF_LOG_SECRETS_MAX, and every later line shows SKIFF_LOG_WITHHELD instead of its message.
 * config.ini's checks keep configured values inside these limits.
 */
skiff_err skiff_log_add_secret(skiff_log *log, const char *value);

/* Writes what is buffered now. Like every call here it cannot fail its caller: a batch the Memory
 * Stick refuses is dropped and counted (see above). Does nothing for NULL. */
void skiff_log_flush(skiff_log *log);

/* "error", "warn", "info" or "debug", as config.ini spells them. */
const char *skiff_log_level_name(skiff_log_level level);

/* The level for one of those names, ignoring case. SKIFF_ERR_CONFIG_INVALID_VALUE for any other
 * name, SKIFF_ERR_INVALID_ARG for NULL. */
skiff_err skiff_log_level_from_name(const char *name, skiff_log_level *out);

/* "YYYY-MM-DD HH:MM:SS.mmmZ" for unix_ms, without the C library's time functions.
 * SKIFF_ERR_INVALID_ARG for NULL or a year outside 0-9999, SKIFF_ERR_BUFFER_TOO_SMALL if out is
 * shorter than SKIFF_LOG_TIMESTAMP_MAX. */
skiff_err skiff_log_format_timestamp(int64_t unix_ms, char *out, size_t out_size);

#endif
