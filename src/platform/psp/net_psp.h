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
 *
 * Loading can also raise the CPU clock for the session, and unloading puts it back. TLS is
 * CPU-bound on a PSP: on a PSP-1000 HTTPS downloads went from about 350 KB/s at 222 MHz to about
 * 470 KB/s at 333 MHz. The clock has to change before the modules load, because with Wi-Fi on the
 * firmware accepts the call and keeps the old clock.
 */

/* The clock while online (bus at half), and what to pass to keep the current one. */
#define SKIFF_PSP_NET_CPU_MHZ 333
#define SKIFF_PSP_NET_CPU_MHZ_UNCHANGED 0

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
    /* The furthest state a join reached, and the error the firmware reported for it (0 if none):
     * written by the firmware's event handler, on its own thread. */
    volatile int apctl_furthest_state;
    volatile int apctl_error;
    /* The CPU and bus clock in MHz after skiff_psp_net_load(), for logs: not what was asked for
     * when the firmware kept its clock. */
    int cpu_mhz;
    int bus_mhz;
    /* The clock before skiff_psp_net_load(), which skiff_psp_net_unload() puts back while
     * clock_changed is set. Kept apart from stage: the clock is restored even when a layer fails to
     * come down. */
    int cpu_mhz_before;
    int bus_mhz_before;
    int clock_changed;
} skiff_psp_net;

/*
 * Sets the CPU clock to cpu_mhz (SKIFF_PSP_NET_CPU_MHZ, or SKIFF_PSP_NET_CPU_MHZ_UNCHANGED to leave
 * it), then loads the modules and starts the libraries. A clock the firmware refuses or ignores is
 * not an error: compare cpu_mhz afterwards. SKIFF_ERR_INVALID_ARG for a NULL net or a cpu_mhz
 * outside 0 to SKIFF_PSP_NET_CPU_MHZ; SKIFF_ERR_NET_UNAVAILABLE if a module or library fails to
 * start.
 */
skiff_err skiff_psp_net_load(skiff_psp_net *net, int cpu_mhz);

/*
 * Joins the access point of Network Settings profile `profile` (1 is the first) and waits for an IP
 * address, up to timeout_us. SKIFF_ERR_NET_UNAVAILABLE: the Wi-Fi switch is off, or there is no
 * such profile. SKIFF_ERR_NET_WIFI_JOIN: the join was refused or did not finish in time; sce_result
 * then holds the firmware's error for it, when it reported one.
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
 * Undoes skiff_psp_net_load() in reverse, down to stage NONE, then puts the clock back (also when a
 * layer failed to come down, though with Wi-Fi still up the firmware may keep the session clock).
 * Refuses while the access point is not
 * disconnected, and stops at the first step that fails: stage then names the layer still live, so
 * the caller can retry or leave the rest to the process exit. SKIFF_ERR_NET_UNAVAILABLE on either.
 */
skiff_err skiff_psp_net_unload(skiff_psp_net *net);

#endif
