#ifndef SKIFF_TEST_TEMP_DIR_H
#define SKIFF_TEST_TEMP_DIR_H

/* A fresh directory per test for the storage tests, removed with everything in it afterwards. */

#include <stddef.h>

#define TEMP_DIR_PATH_MAX 256

/* Creates a new, empty directory under the system's temporary directory; 0 on success. */
int temp_dir_create(char *out, size_t out_size);

/* "<dir>/<name>" into out; 0 if it does not fit. */
int temp_dir_path(const char *dir, const char *name, char *out, size_t out_size);

/* Removes dir and the files in it (the tests create no subdirectories). */
void temp_dir_remove(const char *dir);

#endif
