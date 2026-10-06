#ifndef SKIFF_HOST_STORAGE_POSIX_H
#define SKIFF_HOST_STORAGE_POSIX_H

/*
 * skiff_storage over the host's POSIX file calls, for host tests (never an EBOOT): paths are host
 * paths, typically under a temporary directory. It keeps the PSP's rules where they differ from
 * POSIX, so a test catches what would fail on a Memory Stick: rename refuses to replace an existing
 * file (FAT cannot do it in one step).
 */

#include "skiff/storage.h"

/* SKIFF_ERR_INVALID_ARG for a NULL out, SKIFF_ERR_NO_MEMORY when allocation fails. */
skiff_err skiff_posix_storage_create(skiff_storage **out);

#endif
