#ifndef SKIFF_PSP_STORAGE_PSP_H
#define SKIFF_PSP_STORAGE_PSP_H

/*
 * skiff_storage on the PSP's own file calls (sceIo): 64-bit file positions, which newlib's stdio
 * cannot reach past 2 GiB (its off_t is 32 bits). Paths are full device paths ("ms0:/PSP/...",
 * "ef0:/..." on a PSP Go); one without a device is refused, since sync works per device. A rename
 * never replaces an existing file.
 */

#include "skiff/storage.h"

/* The last firmware call that failed and what it returned, for logs and bug reports: a handle that
 * stopped working after a suspend shows here. */
typedef struct skiff_psp_storage_failure {
    const char *call;
    int sce_result;
} skiff_psp_storage_failure;

/* SKIFF_ERR_INVALID_ARG for a NULL out, SKIFF_ERR_NO_MEMORY when allocation fails. */
skiff_err skiff_psp_storage_create(skiff_storage **out);

/* NULL and 0 while no call has failed, or for a storage that is not the PSP's. */
skiff_psp_storage_failure skiff_psp_storage_last_failure(const skiff_storage *storage);

#endif
