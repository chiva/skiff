#ifndef SKIFF_TEST_FAKE_STORAGE_H
#define SKIFF_TEST_FAKE_STORAGE_H

/*
 * A skiff_storage for host tests of the layers above storage/ (jobs/): it passes every call to a
 * real storage (the POSIX one over a temporary directory) and injects what a Memory Stick does to a
 * long download: it fills up mid-write, a read, sync or rename fails, or every open file handle
 * stops working, as one may after the PSP suspends; a folder listing, stat or removal fails, also
 * part-way through a listing; the power goes after a given number of changes. Failures can be
 * limited to paths with a given suffix (".part"), so the rest keeps working.
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
    /* Once this many bytes have been read from matching files, a read gets only what is left of the
     * budget, and the next fails with read_error; SKIFF_OK for never. */
    uint64_t read_budget;
    skiff_err read_error;
    /* Every sync of a matching file fails with this; SKIFF_OK for none. */
    skiff_err sync_error;
    /* Every rename of a matching file (by its old name) fails with this; SKIFF_OK for none. */
    skiff_err rename_error;
    /* The next this many size queries of matching files fail with SKIFF_ERR_STORAGE_IO; 0 for
     * none. */
    int size_failures;
    /* Every remove of a matching file deletes it and then fails with this, as a device that reports
     * an error after the directory entry is gone; SKIFF_OK for none. */
    skiff_err remove_error;
    /* After this many writes to matching files, every file open at that moment fails each later
     * call with SKIFF_ERR_STORAGE_IO, as a handle lost to a suspend; 0 for never. Files opened
     * afterwards work. */
    int stale_after_writes;
    /* Every folder creation of a matching path fails with this; SKIFF_OK for none. */
    skiff_err mkdir_error;
    /* When set, free_space reports free_bytes instead of asking the inner storage. */
    int has_free_bytes;
    uint64_t free_bytes;
    /* When not SKIFF_OK, free_space fails with this (SKIFF_ERR_NOT_IMPLEMENTED: a device that
     * cannot tell). */
    skiff_err free_space_error;
    /* Every listing of a matching folder passes on list_error_after of its entries ("." and ".."
     * not counted) and fails with this at the next one, as a folder handle lost to a suspend
     * part-way through; a folder with no more entries lists whole. 0 fails it before any entry.
     * SKIFF_OK for none. */
    skiff_err list_error;
    int list_error_after;
    /* Listings report every entry without a date (SKIFF_STORAGE_MTIME_UNKNOWN), as a device that
     * holds none for it. */
    int hide_mtimes;
    /* Every stat of a matching path fails with this; SKIFF_OK for none. */
    skiff_err stat_error;
    /* Every folder removal of a matching path fails with this, leaving the folder; SKIFF_OK for
     * none. */
    skiff_err rmdir_error;
    /* Once this many changes reached the device (an open for writing, a write, sync, rename,
     * removal, folder creation or removal that succeeded, on any path), every later call fails with
     * SKIFF_ERR_STORAGE_NO_MEDIA before reaching the inner storage, as if the power went: what was
     * done stays done, nothing more happens. 0 for never. */
    int cut_after_changes;
    /* Called at every open and remove, before the inner storage acts ("open" or "remove", and the
     * path): a test runs code "during" Memory Stick I/O here. NULL for none. */
    void (*on_call)(void *ctx, const char *call, const char *path);
    void *on_call_ctx;
    /* What the fake saw. */
    int mkdirs;
    int rmdirs;
    int free_space_queries;
    uint64_t bytes_written;
    uint64_t bytes_read;
    int writes;
    int syncs;
    int opens;
    int closes;
    int handles_lost;
    /* Changes that reached the inner storage, counted on every path (see cut_after_changes). */
    int changes;
    /* Bumped when open handles go stale; a file remembers the value it was opened under. */
    int generation;
} fake_storage;

/* Wraps inner. Nothing to free: destroying the fake does not destroy inner. */
void fake_storage_init(fake_storage *fake, skiff_storage *inner);

#endif
