#include "temp_dir.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEMP_DIR_TEMPLATE "skiff-test-XXXXXX"
#define DEFAULT_TMP "/tmp"

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

void temp_dir_remove(const char *dir) {
    DIR *listing = opendir(dir);
    if (listing == NULL) {
        return;
    }
    char path[TEMP_DIR_PATH_MAX];
    for (const struct dirent *entry = readdir(listing); entry != NULL; entry = readdir(listing)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 &&
            temp_dir_path(dir, entry->d_name, path, sizeof path)) {
            unlink(path);
        }
    }
    closedir(listing);
    rmdir(dir);
}
