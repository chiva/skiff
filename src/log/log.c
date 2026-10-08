#include "skiff/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Text is formatted at twice the line length before redaction, so a long secret replaced by the
 * shorter SKIFF_LOG_REDACTED still leaves a full line. */
#define SCRATCH_MAX (2 * SKIFF_LOG_LINE_MAX)
#define NO_TAG "-"
#define LOG_TAG "log"
#define LEVEL_LETTERS "EWID"
#define CONTROL_REPLACEMENT '?'
#define ASCII_DELETE 0x7F
#define ASCII_FIRST_PRINTABLE 0x20

#define MS_PER_SECOND 1000
#define SECONDS_PER_MINUTE 60
#define MINUTES_PER_HOUR 60
#define HOURS_PER_DAY 24
#define SECONDS_PER_HOUR ((int64_t)SECONDS_PER_MINUTE * MINUTES_PER_HOUR)
#define MS_PER_DAY (MS_PER_SECOND * SECONDS_PER_HOUR * HOURS_PER_DAY)
/* 0000-01-01 and 9999-12-31 23:59:59.999 in milliseconds since 1970. */
#define FIRST_FORMATTED_MS (-62167219200000LL)
#define LAST_FORMATTED_MS 253402300799999LL

/* civil_from_days() (Howard Hinnant's algorithm): the proleptic Gregorian calendar in 400-year eras
 * of 146097 days, with years starting on 1 March so the leap day is the last of the year. */
#define DAYS_FROM_0000_03_01_TO_1970 719468
#define DAYS_PER_ERA 146097
#define YEARS_PER_ERA 400
#define DAYS_PER_YEAR 365
#define DAYS_PER_4_YEARS 1460
#define DAYS_PER_CENTURY 36524
#define LEAP_EVERY 4
#define NO_LEAP_EVERY 100
#define MONTH_SPAN_NUMERATOR 153
#define MONTH_SPAN_DENOMINATOR 5
#define MONTH_SPAN_OFFSET 2
#define MONTHS_FROM_MARCH_TO_DECEMBER 10
#define MARCH 3
#define FEBRUARY 2
#define MONTHS_PER_YEAR 12

struct skiff_log {
    skiff_storage *storage;
    char path[SKIFF_LOG_PATH_MAX];
    char rotated_path[SKIFF_LOG_PATH_MAX];
    uint64_t cap_bytes;
    skiff_log_level level;
    skiff_log_clock_fn clock;
    void *clock_ctx;
    skiff_log_lock_fn lock;
    skiff_log_lock_fn unlock;
    void *lock_ctx;
    char secrets[SKIFF_LOG_SECRETS_MAX][SKIFF_LOG_SECRET_MAX + 1];
    size_t secret_lengths[SKIFF_LOG_SECRETS_MAX];
    size_t secret_count;
    /* Lines that left the buffer unwritten since the last batch that reached the file. Always the
     * oldest ones, so the report of them (report, report_length bytes, composed when the count
     * changes) goes at the front of the next batch. */
    unsigned long dropped_lines;
    char report[SKIFF_LOG_LINE_MAX];
    size_t report_length;
    /* The last batch failed and is still buffered: a full buffer then gives up its oldest lines
     * instead of trying again, until a warning, an error, a flush, or a buffer's worth of dropped
     * bytes (dropped_bytes_since_try) calls for another try. */
    int failing;
    size_t dropped_bytes_since_try;
    /* Where the buffered batch was last tried, while its start is unchanged since: the next try
     * writes there again, over whatever part of it reached the file. */
    int retry_known;
    skiff_file_mode retry_mode;
    uint64_t retry_offset;
    int retry_separate;
    /* A failed batch whose start then changed may have left part of itself in the file from
     * failed_offset on; the next batch then starts on a new line. */
    int fragment_possible;
    uint64_t failed_offset;
    /* A secret could not be registered, so no message can be shown safely. */
    int withholding;
    size_t pending_bytes;
    size_t buffer_bytes;
    char buffer[];
};

static const char *const LEVEL_NAMES[] = {"error", "warn", "info", "debug"};

/* Shown in full; any other header's value may be a credential (Authorization, cookies, a proxy's
 * access token) and is redacted. */
static const char *const HARMLESS_HEADERS[] = {"Content-Length", "Content-Range", "Content-Type",
                                               "ETag"};

static int known_level(skiff_log_level level) {
    return level >= SKIFF_LOG_ERROR && level <= SKIFF_LOG_DEBUG;
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static int equals_ignoring_case(const char *a, const char *b) {
    while (*a != '\0' && ascii_lower(*a) == ascii_lower(*b)) {
        a++;
        b++;
    }
    return ascii_lower(*a) == ascii_lower(*b);
}

static void take_lock(const skiff_log *log) {
    if (log->lock != NULL) {
        log->lock(log->lock_ctx);
    }
}

static void release_lock(const skiff_log *log) {
    if (log->unlock != NULL) {
        log->unlock(log->lock_ctx);
    }
}

/* Floor division, so times before 1970 fall on the day they belong to. */
static int64_t floor_div(int64_t value, int64_t divisor) {
    const int64_t quotient = value / divisor;
    return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}

skiff_err skiff_log_format_timestamp(int64_t unix_ms, char *out, size_t out_size) {
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (out == NULL || unix_ms < FIRST_FORMATTED_MS || unix_ms > LAST_FORMATTED_MS) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (out_size < SKIFF_LOG_TIMESTAMP_MAX) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    const int64_t days = floor_div(unix_ms, MS_PER_DAY);
    const int64_t ms_of_day = unix_ms - days * MS_PER_DAY;

    const int64_t shifted = days + DAYS_FROM_0000_03_01_TO_1970;
    const int64_t era = floor_div(shifted, DAYS_PER_ERA);
    const int64_t day_of_era = shifted - era * DAYS_PER_ERA;
    const int64_t year_of_era = (day_of_era - day_of_era / DAYS_PER_4_YEARS +
                                 day_of_era / DAYS_PER_CENTURY - day_of_era / (DAYS_PER_ERA - 1)) /
                                DAYS_PER_YEAR;
    const int64_t day_of_year =
        day_of_era -
        (DAYS_PER_YEAR * year_of_era + year_of_era / LEAP_EVERY - year_of_era / NO_LEAP_EVERY);
    const int64_t month_from_march =
        (MONTH_SPAN_DENOMINATOR * day_of_year + MONTH_SPAN_OFFSET) / MONTH_SPAN_NUMERATOR;
    const int64_t day =
        day_of_year -
        (MONTH_SPAN_NUMERATOR * month_from_march + MONTH_SPAN_OFFSET) / MONTH_SPAN_DENOMINATOR + 1;
    const int64_t month = month_from_march < MONTHS_FROM_MARCH_TO_DECEMBER
                              ? month_from_march + MARCH
                              : month_from_march + MARCH - MONTHS_PER_YEAR;
    const int64_t year = year_of_era + era * YEARS_PER_ERA + (month <= FEBRUARY ? 1 : 0);

    const int64_t ms = ms_of_day % MS_PER_SECOND;
    const int64_t seconds_of_day = ms_of_day / MS_PER_SECOND;
    const int64_t second = seconds_of_day % SECONDS_PER_MINUTE;
    const int64_t minute = seconds_of_day / SECONDS_PER_MINUTE % MINUTES_PER_HOUR;
    const int64_t hour = seconds_of_day / SECONDS_PER_HOUR;
    snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d.%03dZ", (int)year, (int)month, (int)day,
             (int)hour, (int)minute, (int)second, (int)ms);
    return SKIFF_OK;
}

const char *skiff_log_level_name(skiff_log_level level) {
    return known_level(level) ? LEVEL_NAMES[level] : "unknown";
}

skiff_err skiff_log_level_from_name(const char *name, skiff_log_level *out) {
    if (name == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < sizeof LEVEL_NAMES / sizeof LEVEL_NAMES[0]; i++) {
        if (equals_ignoring_case(name, LEVEL_NAMES[i])) {
            *out = (skiff_log_level)i;
            return SKIFF_OK;
        }
    }
    return SKIFF_ERR_CONFIG_INVALID_VALUE;
}

skiff_err skiff_log_create(const skiff_log_config *config, skiff_log **out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (config == NULL || config->storage == NULL || config->path == NULL ||
        config->path[0] == '\0' || !known_level(config->level) ||
        (config->lock == NULL) != (config->unlock == NULL)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const uint64_t cap_bytes =
        config->cap_bytes != 0 ? config->cap_bytes : SKIFF_LOG_DEFAULT_CAP_BYTES;
    const size_t buffer_bytes =
        config->buffer_bytes != 0 ? config->buffer_bytes : SKIFF_LOG_DEFAULT_BUFFER_BYTES;
    if (buffer_bytes < SKIFF_LOG_LINE_MAX || buffer_bytes > SKIFF_LOG_BUFFER_MAX ||
        buffer_bytes > cap_bytes) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (strlen(config->path) + sizeof SKIFF_LOG_ROTATED_SUFFIX > SKIFF_LOG_PATH_MAX) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    skiff_log *log = calloc(1, sizeof *log + buffer_bytes);
    if (log == NULL) {
        return SKIFF_ERR_NO_MEMORY;
    }
    log->storage = config->storage;
    snprintf(log->path, sizeof log->path, "%s", config->path);
    snprintf(log->rotated_path, sizeof log->rotated_path, "%s%s", config->path,
             SKIFF_LOG_ROTATED_SUFFIX);
    log->cap_bytes = cap_bytes;
    log->buffer_bytes = buffer_bytes;
    log->level = config->level;
    log->clock = config->clock;
    log->clock_ctx = config->clock_ctx;
    log->lock = config->lock;
    log->unlock = config->unlock;
    log->lock_ctx = config->lock_ctx;
    *out = log;
    return SKIFF_OK;
}

/* ---- Writing batches ---- */

/* Moves a full log aside so the batch starts a new file. When that fails the old lines are given
 * up instead (the new file replaces the log): the cap is what the Memory Stick was promised. */
static void rotate(skiff_log *log) {
    (void)skiff_storage_remove(log->storage, log->rotated_path);
    (void)skiff_storage_rename(log->storage, log->path, log->rotated_path);
}

/* A batch appended after part of a lost one starts with a line break: the file cannot be shortened
 * to drop the fragment, so it at least gets a line of its own. */
static int after_fragment(const skiff_log *log, skiff_file_mode mode, uint64_t offset) {
    return log->fragment_possible && mode == SKIFF_FILE_WRITE_AT && offset > log->failed_offset;
}

/* Where a batch goes, and whether it starts with a line break. */
typedef struct batch_plan {
    skiff_file_mode mode;
    uint64_t offset;
    int separate;
} batch_plan;

/*
 * Where the batch goes: appended to the log, or a new file once the log is missing or full. A
 * batch tried before goes where it went then (it starts as it did), so whatever part of it reached
 * the file is written over, not repeated; that place is checked against the cap like the end.
 */
static skiff_err plan_batch(skiff_log *log, batch_plan *plan) {
    uint64_t size = 0;
    const skiff_err err = skiff_storage_size(log->storage, log->path, &size);
    plan->mode = SKIFF_FILE_REPLACE;
    plan->offset = 0;
    plan->separate = 0;
    if (err == SKIFF_ERR_STORAGE_NOT_FOUND ||
        (err == SKIFF_OK && log->retry_known && log->retry_mode == SKIFF_FILE_REPLACE)) {
        return SKIFF_OK;
    }
    if (err != SKIFF_OK) {
        return err;
    }
    uint64_t end = size;
    int separate = after_fragment(log, SKIFF_FILE_WRITE_AT, size);
    if (log->retry_known && size >= log->retry_offset) {
        end = log->retry_offset;
        separate = log->retry_separate;
    }
    const uint64_t batch_bytes = log->report_length + log->pending_bytes + (separate ? 1U : 0U);
    /* The report and the lines fit the buffer together, and the buffer is at most the cap, so this
     * cannot overflow. */
    if (end > log->cap_bytes || batch_bytes > log->cap_bytes - end) {
        rotate(log);
        return SKIFF_OK;
    }
    plan->mode = SKIFF_FILE_WRITE_AT;
    plan->offset = end;
    plan->separate = separate;
    return SKIFF_OK;
}

static skiff_err write_batch(skiff_log *log, const batch_plan *plan) {
    skiff_file *file = NULL;
    skiff_err err = skiff_storage_open(log->storage, log->path, plan->mode, plan->offset, &file);
    if (err != SKIFF_OK) {
        return err;
    }
    if (plan->separate) {
        err = skiff_file_write(file, "\n", 1);
    }
    if (err == SKIFF_OK && log->report_length > 0) {
        err = skiff_file_write(file, log->report, log->report_length);
    }
    if (err == SKIFF_OK && log->pending_bytes > 0) {
        err = skiff_file_write(file, log->buffer, log->pending_bytes);
    }
    if (err == SKIFF_OK) {
        /* On the device, so a crash or power cut right after a warning cannot lose it. */
        err = skiff_file_sync(file);
    }
    const skiff_err close_err = skiff_file_close(file);
    return err != SKIFF_OK ? err : close_err;
}

static skiff_err try_batch(skiff_log *log) {
    batch_plan plan;
    skiff_err err = plan_batch(log, &plan);
    if (err != SKIFF_OK) {
        return err;
    }
    err = write_batch(log, &plan);
    if (err != SKIFF_OK) {
        /* Part of it may be in the file now: the next try writes over it. */
        log->retry_known = 1;
        log->retry_mode = plan.mode;
        log->retry_offset = plan.offset;
        log->retry_separate = plan.separate;
    }
    return err;
}

/*
 * One try, then one more: a write after a suspend fails on the handle, and a file opened again
 * works. A batch refused both times stays in the buffer and goes out with the next one; right
 * after waking, the Memory Stick can refuse for longer than one retry.
 */
static skiff_err flush_locked(skiff_log *log) {
    log->dropped_bytes_since_try = 0;
    if (log->pending_bytes == 0 && log->report_length == 0) {
        return SKIFF_OK;
    }
    skiff_err err = try_batch(log);
    if (err != SKIFF_OK) {
        err = try_batch(log);
    }
    if (err != SKIFF_OK) {
        log->failing = 1;
        return err;
    }
    log->failing = 0;
    log->dropped_lines = 0;
    log->report_length = 0;
    log->retry_known = 0;
    log->fragment_possible = 0;
    log->pending_bytes = 0;
    return SKIFF_OK;
}

/* The caller has made room. */
static void buffer_line(skiff_log *log, const char *line, size_t length) {
    memcpy(log->buffer + log->pending_bytes, line, length);
    log->pending_bytes += length;
}

/* ---- Composing lines ---- */

/* The end of the longest registered secret starting at text + start, or start when none does. When
 * text was cut, a secret whose start fills the rest of it counts too, running to the end. */
static size_t secret_end(const skiff_log *log, const char *text, size_t available, size_t start,
                         int cut) {
    const size_t rest = available - start;
    size_t end = start;
    for (size_t i = 0; i < log->secret_count; i++) {
        const size_t length = log->secret_lengths[i];
        if (length <= rest && start + length > end &&
            memcmp(text + start, log->secrets[i], length) == 0) {
            end = start + length;
        } else if (cut && length > rest && memcmp(text + start, log->secrets[i], rest) == 0) {
            end = available;
        }
    }
    return end;
}

/* How much of text, from its start, is secret: the longest secret starting there, stretched by any
 * secret starting inside it that goes further (one value nested in or overlapping another), so no
 * part of either is left in the clear. 0 when no secret starts at text. */
static size_t secret_span(const skiff_log *log, const char *text, size_t available, int cut) {
    size_t span = secret_end(log, text, available, 0, cut);
    for (size_t start = 1; start < span; start++) {
        const size_t end = secret_end(log, text, available, start, cut);
        span = end > span ? end : span;
    }
    return span;
}

/* Drops a tail that could be the start of a secret the cut went through. */
static size_t drop_partial_secret(const skiff_log *log, const char *line, size_t length) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (size_t i = 0; i < log->secret_count; i++) {
            const size_t longest =
                log->secret_lengths[i] - 1 < length ? log->secret_lengths[i] - 1 : length;
            for (size_t k = longest; k > 0; k--) {
                if (memcmp(line + length - k, log->secrets[i], k) == 0) {
                    length -= k;
                    changed = 1;
                    break;
                }
            }
        }
    }
    return length;
}

/*
 * Copies text into line (SKIFF_LOG_LINE_MAX bytes, newline included) with every secret replaced
 * and control characters made visible; cut says text was already shortened. Returns the length.
 */
static size_t redact_into(const skiff_log *log, const char *text, size_t text_length, int cut,
                          char *line) {
    const size_t room = SKIFF_LOG_LINE_MAX - 1;
    const size_t redacted_length = sizeof SKIFF_LOG_REDACTED - 1;
    size_t length = 0;
    size_t at = 0;
    while (at < text_length) {
        const size_t span = secret_span(log, text + at, text_length - at, cut);
        if (span > 0) {
            if (length + redacted_length > room) {
                break;
            }
            memcpy(line + length, SKIFF_LOG_REDACTED, redacted_length);
            length += redacted_length;
            at += span;
            continue;
        }
        if (length == room) {
            break;
        }
        const unsigned char c = (unsigned char)text[at];
        char shown = text[at];
        if (c < ASCII_FIRST_PRINTABLE || c == ASCII_DELETE) {
            shown = CONTROL_REPLACEMENT;
        }
        line[length++] = shown;
        at++;
    }
    if (at < text_length || cut) {
        const size_t marker_length = sizeof SKIFF_LOG_CUT_MARKER - 1;
        if (length > room - marker_length) {
            length = room - marker_length;
        }
        length = drop_partial_secret(log, line, length);
        memcpy(line + length, SKIFF_LOG_CUT_MARKER, marker_length);
        length += marker_length;
    }
    line[length++] = '\n';
    return length;
}

/* The finished line, newline included, in line (SKIFF_LOG_LINE_MAX bytes); returns its length, 0
 * when it could not be formatted. */
static size_t compose_line(const skiff_log *log, skiff_log_level level, const char *tag,
                           const char *message, int cut, char *line) {
    char timestamp[SKIFF_LOG_TIMESTAMP_MAX] = "";
    int64_t now_ms = 0;
    if (log->clock == NULL || !log->clock(log->clock_ctx, &now_ms) ||
        skiff_log_format_timestamp(now_ms, timestamp, sizeof timestamp) != SKIFF_OK) {
        timestamp[0] = '\0';
    }
    char text[SCRATCH_MAX];
    const int written =
        snprintf(text, sizeof text, "%s%s%c %s: %s", timestamp, timestamp[0] != '\0' ? " " : "",
                 LEVEL_LETTERS[level], tag != NULL ? tag : NO_TAG,
                 log->withholding ? SKIFF_LOG_WITHHELD : message);
    if (written < 0) {
        return 0;
    }
    const size_t text_length = (size_t)written < sizeof text ? (size_t)written : sizeof text - 1;
    return redact_into(log, text, text_length, cut || (size_t)written >= sizeof text, line);
}

/*
 * Counts a line that will never be written (length bytes). The batch's start changes with the
 * report, so a part of it already in the file can no longer be written over: the next batch starts
 * on a new line after it instead.
 */
static void count_dropped(skiff_log *log, size_t length) {
    log->dropped_lines++;
    log->dropped_bytes_since_try += length;
    if (log->retry_known) {
        log->retry_known = 0;
        if (!log->fragment_possible) {
            log->fragment_possible = 1;
            log->failed_offset = log->retry_mode == SKIFF_FILE_REPLACE ? 0 : log->retry_offset;
        }
    }
    char message[SCRATCH_MAX];
    snprintf(message, sizeof message, "%lu earlier lines could not be written", log->dropped_lines);
    log->report_length = compose_line(log, SKIFF_LOG_WARN, LOG_TAG, message, 0, log->report);
}

/* Gives up the oldest buffered line. Oldest, not the newest: the last lines tell what the player
 * saw when they filed the report, and a warning stays as long as later lines leave room for it. */
static void drop_oldest(skiff_log *log) {
    const char *end = memchr(log->buffer, '\n', log->pending_bytes);
    const size_t length = end != NULL ? (size_t)(end - log->buffer) + 1 : log->pending_bytes;
    memmove(log->buffer, log->buffer + length, log->pending_bytes - length);
    log->pending_bytes -= length;
    count_dropped(log, length);
}

static int fits(const skiff_log *log, size_t length) {
    return log->report_length + log->pending_bytes + length <= log->buffer_bytes;
}

/*
 * Room for a line of length bytes next to the report: a full buffer is written, or, while the
 * Memory Stick refuses, gives up its oldest lines. A warning or error (urgent) is written right
 * after anyway, so it tries first. 0 when the line cannot fit even alone.
 */
static int make_room(skiff_log *log, size_t length, int urgent) {
    if (fits(log, length)) {
        return 1;
    }
    /* Otherwise retried once a buffer's worth of lines has been lost, as often as a full buffer was
     * written before the failure; a refusing Memory Stick gets no extra try per line. */
    int tried = 0;
    if (!log->failing || urgent || log->dropped_bytes_since_try >= log->buffer_bytes) {
        (void)flush_locked(log);
        tried = 1;
    }
    while (!fits(log, length) && log->pending_bytes > 0) {
        drop_oldest(log);
    }
    /* Only the report is in the way (a buffer too small for it and a long line): it goes alone. */
    if (!fits(log, length) && !tried) {
        (void)flush_locked(log);
    }
    return fits(log, length);
}

static void log_message(skiff_log *log, skiff_log_level level, const char *tag, const char *message,
                        int cut) {
    char line[SKIFF_LOG_LINE_MAX];
    take_lock(log);
    const size_t length = compose_line(log, level, tag, message, cut, line);
    if (length > 0) {
        if (make_room(log, length, level <= SKIFF_LOG_WARN)) {
            buffer_line(log, line, length);
        } else {
            count_dropped(log, length);
        }
        if (level <= SKIFF_LOG_WARN) {
            (void)flush_locked(log);
        }
    }
    release_lock(log);
}

static int kept(const skiff_log *log, skiff_log_level level) {
    return log != NULL && known_level(level) && level <= log->level;
}

void skiff_log_write(skiff_log *log, skiff_log_level level, const char *tag, const char *format,
                     ...) {
    if (format == NULL || !kept(log, level)) {
        return;
    }
    char message[SCRATCH_MAX];
    va_list args;
    va_start(args, format);
    /* clang-tidy on amd64 (not arm64) loses track of va_start here, as in #26. */
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    const int written = vsnprintf(message, sizeof message, format, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    log_message(log, level, tag, message, (size_t)written >= sizeof message);
}

static int harmless_header(const char *name) {
    for (size_t i = 0; i < sizeof HARMLESS_HEADERS / sizeof HARMLESS_HEADERS[0]; i++) {
        if (equals_ignoring_case(name, HARMLESS_HEADERS[i])) {
            return 1;
        }
    }
    return 0;
}

void skiff_log_header(skiff_log *log, skiff_log_level level, const char *tag, const char *name,
                      const char *value) {
    if (name == NULL || !kept(log, level)) {
        return;
    }
    const char *shown = harmless_header(name) && value != NULL ? value : SKIFF_LOG_REDACTED;
    char message[SCRATCH_MAX];
    const int written = snprintf(message, sizeof message, "%s: %s", name, shown);
    if (written < 0) {
        return;
    }
    log_message(log, level, tag, message, (size_t)written >= sizeof message);
}

skiff_err skiff_log_add_secret(skiff_log *log, const char *value) {
    if (log == NULL || value == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const size_t length = strlen(value);
    if (length == 0) {
        return SKIFF_ERR_INVALID_ARG;
    }
    skiff_err err = SKIFF_OK;
    take_lock(log);
    int known = 0;
    for (size_t i = 0; i < log->secret_count && !known; i++) {
        known = log->secret_lengths[i] == length && memcmp(log->secrets[i], value, length) == 0;
    }
    if (length < SKIFF_LOG_SECRET_MIN) {
        err = SKIFF_ERR_INVALID_ARG;
    } else if (length > SKIFF_LOG_SECRET_MAX ||
               (!known && log->secret_count == SKIFF_LOG_SECRETS_MAX)) {
        err = SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    if (err != SKIFF_OK) {
        /* Fail closed: a secret that cannot be found must not be printable either. */
        log->withholding = 1;
    } else if (!known) {
        memcpy(log->secrets[log->secret_count], value, length + 1);
        log->secret_lengths[log->secret_count] = length;
        log->secret_count++;
    }
    release_lock(log);
    return err;
}

void skiff_log_flush(skiff_log *log) {
    if (log == NULL) {
        return;
    }
    take_lock(log);
    (void)flush_locked(log);
    release_lock(log);
}

void skiff_log_destroy(skiff_log *log) {
    if (log == NULL) {
        return;
    }
    skiff_log_flush(log);
    free(log);
}
