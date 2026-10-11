#include "skiff/storage.h"

#include <string.h>

#define DEVICE_SEPARATOR ':'
#define PATH_SEPARATOR '/'

static int usable(const skiff_storage *storage) { return storage != NULL && storage->ops != NULL; }

static int usable_file(const skiff_file *file) { return file != NULL && usable(file->storage); }

skiff_err skiff_storage_open(skiff_storage *storage, const char *path, skiff_file_mode mode,
                             uint64_t offset, skiff_file **out) {
    if (out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    *out = NULL;
    const int known_mode =
        mode == SKIFF_FILE_READ || mode == SKIFF_FILE_WRITE_AT || mode == SKIFF_FILE_REPLACE;
    if (!usable(storage) || path == NULL || path[0] == '\0' || !known_mode ||
        (mode == SKIFF_FILE_REPLACE && offset != 0)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return storage->ops->open(storage, path, mode, offset, out);
}

skiff_err skiff_file_read(skiff_file *file, void *buffer, size_t size, size_t *got) {
    if (got != NULL) {
        *got = 0;
    }
    if (!usable_file(file) || got == NULL || (buffer == NULL && size > 0)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return size == 0 ? SKIFF_OK : file->storage->ops->read(file, buffer, size, got);
}

skiff_err skiff_file_write(skiff_file *file, const void *data, size_t size) {
    if (!usable_file(file) || (data == NULL && size > 0)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return size == 0 ? SKIFF_OK : file->storage->ops->write(file, data, size);
}

skiff_err skiff_file_sync(skiff_file *file) {
    return usable_file(file) ? file->storage->ops->sync(file) : SKIFF_ERR_INVALID_ARG;
}

skiff_err skiff_file_close(skiff_file *file) {
    if (file == NULL) {
        return SKIFF_OK;
    }
    return usable_file(file) ? file->storage->ops->close(file) : SKIFF_ERR_INVALID_ARG;
}

skiff_err skiff_storage_size(skiff_storage *storage, const char *path, uint64_t *out) {
    if (out != NULL) {
        *out = 0;
    }
    if (!usable(storage) || path == NULL || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return storage->ops->size(storage, path, out);
}

skiff_err skiff_storage_rename(skiff_storage *storage, const char *from, const char *to) {
    if (!usable(storage) || from == NULL || to == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return storage->ops->rename(storage, from, to);
}

skiff_err skiff_storage_remove(skiff_storage *storage, const char *path) {
    if (!usable(storage) || path == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return storage->ops->remove(storage, path);
}

skiff_err skiff_storage_mkdir(skiff_storage *storage, const char *path) {
    if (!usable(storage) || path == NULL || path[0] == '\0') {
        return SKIFF_ERR_INVALID_ARG;
    }
    return storage->ops->mkdir(storage, path);
}

/* Where the folders of path start: after "ms0:/" (or "ms0:"), or after a leading '/'. */
static size_t first_folder(const char *path) {
    const char *separator = strchr(path, DEVICE_SEPARATOR);
    const char *slash = strchr(path, PATH_SEPARATOR);
    size_t start = 0;
    if (separator != NULL && (slash == NULL || separator < slash)) {
        start = (size_t)(separator - path) + 1;
    }
    while (path[start] == PATH_SEPARATOR) {
        start++;
    }
    return start;
}

skiff_err skiff_storage_mkdirs(skiff_storage *storage, const char *path) {
    if (!usable(storage) || path == NULL || path[0] == '\0') {
        return SKIFF_ERR_INVALID_ARG;
    }
    char walk[SKIFF_STORAGE_PATH_MAX];
    size_t length = strlen(path);
    if (length >= sizeof walk) {
        return SKIFF_ERR_INVALID_ARG;
    }
    memcpy(walk, path, length + 1);
    while (length > 0 && walk[length - 1] == PATH_SEPARATOR) {
        walk[--length] = '\0';
    }
    const size_t start = first_folder(walk);
    if (start >= length) {
        return SKIFF_ERR_INVALID_ARG; /* a device or "/" alone: nothing to create */
    }
    skiff_err err = SKIFF_OK;
    for (size_t i = start; err == SKIFF_OK && i < length; i++) {
        if (walk[i] == PATH_SEPARATOR && walk[i - 1] != PATH_SEPARATOR) {
            walk[i] = '\0';
            err = storage->ops->mkdir(storage, walk);
            walk[i] = PATH_SEPARATOR;
        }
    }
    return err == SKIFF_OK ? storage->ops->mkdir(storage, walk) : err;
}

skiff_err skiff_storage_free_space(skiff_storage *storage, const char *path, uint64_t *out) {
    if (out != NULL) {
        *out = 0;
    }
    if (!usable(storage) || path == NULL || path[0] == '\0' || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    return storage->ops->free_space(storage, path, out);
}

skiff_err skiff_storage_check_room(skiff_storage *storage, const char *path, uint64_t needed) {
    if (needed > SKIFF_STORAGE_MAX_FILE_BYTES) {
        return SKIFF_ERR_STORAGE_FILE_TOO_LARGE;
    }
    uint64_t free_bytes = 0;
    const skiff_err err = skiff_storage_free_space(storage, path, &free_bytes);
    if (err != SKIFF_OK) {
        return err;
    }
    /* needed is below 4 GiB, so the sum cannot overflow. */
    return free_bytes < needed + SKIFF_STORAGE_FREE_MARGIN_BYTES ? SKIFF_ERR_STORAGE_NO_SPACE
                                                                 : SKIFF_OK;
}

/* days_from_civil() (Howard Hinnant's algorithm), the inverse of the one in src/log/log.c: the
 * proleptic Gregorian calendar in 400-year eras of 146097 days, with years starting on 1 March so
 * the leap day is the last of the year. Years from 1 keep every intermediate value non-negative. */
#define DAYS_FROM_0000_03_01_TO_1970 719468
#define DAYS_PER_ERA 146097
#define YEARS_PER_ERA 400
#define DAYS_PER_YEAR 365
#define LEAP_EVERY 4
#define NO_LEAP_EVERY 100
#define MONTH_SPAN_NUMERATOR 153
#define MONTH_SPAN_DENOMINATOR 5
#define MONTH_SPAN_OFFSET 2
#define MARCH 3
#define FEBRUARY 2
#define MONTHS_PER_YEAR 12
#define YEAR_MIN 1
#define YEAR_MAX 9999
#define HOURS_PER_DAY 24
#define MINUTES_PER_HOUR 60
#define SECONDS_PER_MINUTE 60
#define SECONDS_PER_HOUR ((int64_t)SECONDS_PER_MINUTE * MINUTES_PER_HOUR)
#define SECONDS_PER_DAY (SECONDS_PER_HOUR * HOURS_PER_DAY)

static int is_leap_year(int year) {
    return (year % LEAP_EVERY == 0 && year % NO_LEAP_EVERY != 0) || year % YEARS_PER_ERA == 0;
}

static int days_in_month(int year, int month) {
    static const int DAYS[MONTHS_PER_YEAR] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == FEBRUARY && is_leap_year(year) ? DAYS[month - 1] + 1 : DAYS[month - 1];
}

static int valid_date(const skiff_storage_date *date) {
    return date->year >= YEAR_MIN && date->year <= YEAR_MAX && date->month >= 1 &&
           date->month <= MONTHS_PER_YEAR && date->day >= 1 &&
           date->day <= days_in_month(date->year, date->month) && date->hour >= 0 &&
           date->hour < HOURS_PER_DAY && date->minute >= 0 && date->minute < MINUTES_PER_HOUR &&
           date->second >= 0 && date->second < SECONDS_PER_MINUTE;
}

skiff_err skiff_storage_mtime_from_date(const skiff_storage_date *date, int64_t *out) {
    if (out != NULL) {
        *out = SKIFF_STORAGE_MTIME_UNKNOWN;
    }
    if (date == NULL || out == NULL || !valid_date(date)) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const int64_t year = date->month <= FEBRUARY ? date->year - 1 : date->year;
    const int64_t month_from_march = (date->month + MONTHS_PER_YEAR - MARCH) % MONTHS_PER_YEAR;
    const int64_t era = year / YEARS_PER_ERA;
    const int64_t year_of_era = year - era * YEARS_PER_ERA;
    const int64_t day_of_year =
        (MONTH_SPAN_NUMERATOR * month_from_march + MONTH_SPAN_OFFSET) / MONTH_SPAN_DENOMINATOR +
        date->day - 1;
    const int64_t day_of_era = DAYS_PER_YEAR * year_of_era + year_of_era / LEAP_EVERY -
                               year_of_era / NO_LEAP_EVERY + day_of_year;
    const int64_t days = era * DAYS_PER_ERA + day_of_era - DAYS_FROM_0000_03_01_TO_1970;
    *out = days * SECONDS_PER_DAY + date->hour * SECONDS_PER_HOUR +
           (int64_t)date->minute * SECONDS_PER_MINUTE + date->second;
    return SKIFF_OK;
}

skiff_err skiff_storage_stat(skiff_storage *storage, const char *path, skiff_storage_info *out) {
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (!usable(storage) || path == NULL || path[0] == '\0' || out == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    const skiff_err err = storage->ops->stat(storage, path, out);
    if (err != SKIFF_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}

typedef struct list_filter {
    skiff_storage_entry_fn on_entry;
    void *ctx;
    size_t *skipped;
    skiff_storage_entry entry;
} list_filter;

static int is_dot_entry(const char *name, size_t length) {
    return (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.');
}

static skiff_storage_list_step filter_entry(void *ctx, const char *name, size_t length,
                                            const skiff_storage_info *info) {
    list_filter *filter = ctx;
    if (is_dot_entry(name, length)) {
        return SKIFF_STORAGE_LIST_CONTINUE;
    }
    if (length == 0 || length >= sizeof filter->entry.name) {
        (*filter->skipped)++;
        return SKIFF_STORAGE_LIST_CONTINUE;
    }
    memcpy(filter->entry.name, name, length);
    filter->entry.name[length] = '\0';
    filter->entry.info = *info;
    return filter->on_entry(filter->ctx, &filter->entry) == SKIFF_STORAGE_LIST_CONTINUE
               ? SKIFF_STORAGE_LIST_CONTINUE
               : SKIFF_STORAGE_LIST_STOP;
}

skiff_err skiff_storage_list(skiff_storage *storage, const char *path,
                             skiff_storage_entry_fn on_entry, void *ctx, size_t *skipped) {
    if (skipped != NULL) {
        *skipped = 0;
    }
    if (!usable(storage) || path == NULL || path[0] == '\0' || on_entry == NULL ||
        skipped == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    list_filter filter;
    memset(&filter, 0, sizeof filter);
    filter.on_entry = on_entry;
    filter.ctx = ctx;
    filter.skipped = skipped;
    return storage->ops->list(storage, path, filter_entry, &filter);
}

static skiff_storage_list_step note_content(void *ctx, const char *name, size_t length,
                                            const skiff_storage_info *info) {
    (void)info;
    if (is_dot_entry(name, length)) {
        return SKIFF_STORAGE_LIST_CONTINUE;
    }
    *(int *)ctx = 1;
    return SKIFF_STORAGE_LIST_STOP;
}

/* The folder is checked empty first, whatever the device would do: PPSSPP's sceIoRmdir removes a
 * folder with everything in it, and a save must never go with its folder. */
skiff_err skiff_storage_rmdir(skiff_storage *storage, const char *path) {
    if (!usable(storage) || path == NULL || path[0] == '\0') {
        return SKIFF_ERR_INVALID_ARG;
    }
    int has_content = 0;
    const skiff_err err = storage->ops->list(storage, path, note_content, &has_content);
    if (err != SKIFF_OK) {
        return err;
    }
    return has_content ? SKIFF_ERR_STORAGE_IO : storage->ops->rmdir(storage, path);
}

void skiff_storage_destroy(skiff_storage *storage) {
    if (usable(storage) && storage->ops->destroy != NULL) {
        storage->ops->destroy(storage);
    }
}
