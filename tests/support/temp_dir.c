#include "temp_dir.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEMP_DIR_TEMPLATE "skiff-test-XXXXXX"
#define DEFAULT_TMP "/tmp"
/* File descriptors nftw() may keep open while it walks. */
#define TEMP_DIR_OPEN_FOLDERS 16

int temp_dir_create(char *out, size_t out_size) {
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = DEFAULT_TMP;
    }
    const int written = snprintf(out, out_size, "%s/" TEMP_DIR_TEMPLATE, base);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }
    return mkdtemp(out) != NULL ? 0 : -1;
}

int temp_dir_path(const char *dir, const char *name, char *out, size_t out_size) {
    const int written = snprintf(out, out_size, "%s/%s", dir, name);
    return written > 0 && (size_t)written < out_size;
}

static int remove_entry(const char *path, const struct stat *status, int type, struct FTW *walk) {
    (void)status;
    (void)type;
    (void)walk;
    remove(path);
    return 0;
}

void temp_dir_remove(const char *dir) {
    /* Children before their folder, without following links out of dir. */
    nftw(dir, remove_entry, TEMP_DIR_OPEN_FOLDERS, FTW_DEPTH | FTW_PHYS);
}
