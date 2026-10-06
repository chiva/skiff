#ifndef SKIFF_TEST_FAKE_STORAGE_H
#define SKIFF_TEST_FAKE_STORAGE_H

/*
 * A skiff_storage for host tests of the layers above storage/ (jobs/): it passes every call to a
 * real storage (the POSIX one over a temporary directory) and injects what a Memory Stick does to a
 * long download: it fills up mid-write, a sync or rename fails, or every open file handle stops
 * working, as one may after the PSP suspends. Failures can be limited to paths with a given
 * suffix (".part"), so the rest keeps working.
 */

#include <stddef.h>
#include <stdint.h>

#include "skiff/storage.h"

typedef struct fake_storage {
    skiff_storage base;
    skiff_storage *inner; /* not owned */
    /* Only paths ending in this are failed; NULL or "" for every path. */
    const char *fail_suffix;
    /* Once this many bytes have been written to matching files, a write stores only what fits and
     * fails with write_error; SKIFF_OK for never. */
    uint64_t write_budget;
    skiff_err write_error;
    /* Every sync of a matching file fails with this; SKIFF_OK for none. */
    skiff_err sync_error;
    /* Every rename of a matching file (by its old name) fails with this; SKIFF_OK for none. */
    skiff_err rename_error;
    /* Every remove of a matching file deletes it and then fails with this, as a device that reports
     * an error after the directory entry is gone; SKIFF_OK for none. */
    skiff_err remove_error;
    /* After this many writes to matching files, every file open at that moment fails each later
     * call with SKIFF_ERR_STORAGE_IO, as a handle lost to a suspend; 0 for never. Files opened
     * afterwards work. */
    int stale_after_writes;
    /* What the fake saw. */
    uint64_t bytes_written;
    int writes;
    int syncs;
    int opens;
    int handles_lost;
    /* Bumped when open handles go stale; a file remembers the value it was opened under. */
    int generation;
} fake_storage;

/* Wraps inner. Nothing to free: destroying the fake does not destroy inner. */
void fake_storage_init(fake_storage *fake, skiff_storage *inner);

#endif
