#ifndef SKIFF_PSP_KIRK_ENTROPY_H
#define SKIFF_PSP_KIRK_ENTROPY_H

#include "skiff/error.h"

/*
 * Why Skiff's TLS entropy hook (kirk_entropy.c) would refuse, so a failed TLS start can tell the
 * player what to do: SKIFF_ERR_NET_NEEDS_ARK without ARK custom firmware, SKIFF_ERR_NET_ENTROPY
 * once the generator has failed its health test or Mbed TLS made a request the hook refuses,
 * SKIFF_OK otherwise. Checks for ARK on first use,
 * so it can also be called before TLS starts.
 */
skiff_err skiff_psp_entropy_status(void);

#endif
