#ifndef SKIFF_PSP_NET_PSP_H
#define SKIFF_PSP_NET_PSP_H

#include <stddef.h>

#include "skiff/error.h"

/*
 * The PSP's network stack: firmware modules, the libraries on top of them, and the access-point
 * connection. Load once, connect through a saved Network Settings profile (no dialog), and undo
 * both in reverse; tearing the modules down under a live connection can hang the PSP, so unload
 * only after a disconnect that succeeded. Each call records the firmware call that failed and its
 * result, for logs and bug reports.
 */

/* How far skiff_psp_net_load() got; skiff_psp_net_unload() undoes exactly that much. */
typedef enum skiff_psp_net_stage {
    SKIFF_PSP_NET_NONE,
    SKIFF_PSP_NET_COMMON_MODULE,
    SKIFF_PSP_NET_INET_MODULE,
    SKIFF_PSP_NET_NET,
    SKIFF_PSP_NET_INET,
    SKIFF_PSP_NET_RESOLVER,
    SKIFF_PSP_NET_APCTL,
} skiff_psp_net_stage;

/* Room for an IPv4 address in dotted form, as SceNetApctlInfo holds it. */
#define SKIFF_PSP_NET_IP_MAX 16

typedef struct skiff_psp_net {
    skiff_psp_net_stage stage;
    /* The last firmware call that failed and what it returned; NULL and 0 while none has. */
    const char *failed_call;
    int sce_result;
    /* The last access-point state seen (PSP_NET_APCTL_STATE_*), to tell where a join stopped. */
    int apctl_state;
} skiff_psp_net;

/* Loads the modules and starts the libraries. SKIFF_ERR_NET_UNAVAILABLE if any step fails. */
skiff_err skiff_psp_net_load(skiff_psp_net *net);

/*
 * Joins the access point of Network Settings profile `profile` (1 is the first) and waits for an IP
 * address, up to timeout_us. SKIFF_ERR_NET_UNAVAILABLE: the Wi-Fi switch is off, or there is no
 * such profile. SKIFF_ERR_NET_WIFI_JOIN: the join was refused or did not finish in time.
 */
skiff_err skiff_psp_net_connect(skiff_psp_net *net, int profile, long long timeout_us);

/* The address the access point gave this PSP, e.g. "192.168.1.20". */
skiff_err skiff_psp_net_ip(skiff_psp_net *net, char *ip, size_t ip_size);

/*
 * Drops the connection, if any, and waits up to timeout_us until it is reported gone. SKIFF_OK only
 * then; otherwise the modules must stay loaded (process exit releases them).
 */
skiff_err skiff_psp_net_disconnect(skiff_psp_net *net, long long timeout_us);

/*
 * Undoes skiff_psp_net_load() in reverse; every step runs even if an earlier one fails, and stage
 * returns to NONE. SKIFF_ERR_NET_UNAVAILABLE if any step failed.
 */
skiff_err skiff_psp_net_unload(skiff_psp_net *net);

#endif
