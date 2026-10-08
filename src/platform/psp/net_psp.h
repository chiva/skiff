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

/* Room for a Network Settings profile's name with its end, as SceNetApctlInfo holds the name of the
 * profile a connection uses. */
#define SKIFF_PSP_NET_PROFILE_NAME_MAX 64

/* The first Network Settings profile, and the last one skiff_psp_net_connected_profile() looks at:
 * a bound well above the profiles the XMB lets a player save. */
#define SKIFF_PSP_NET_FIRST_PROFILE 1
#define SKIFF_PSP_NET_LAST_PROFILE 24

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
    /* A join started by skiff_psp_net_connect_start() and not yet ended: the firmware's event
     * handler for it (registered with this struct as its context) and when it gives up, in system
     * time (sceKernelGetSystemTimeWide()). */
    int join_pending;
    int join_handler;
    long long join_deadline_us;
    int join_progressed;
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
 * then holds the firmware's error for it, when it reported one. SKIFF_ERR_INVALID_ARG for a NULL
 * net, a profile below 1, before skiff_psp_net_load() finished, or while a join is pending.
 * Blocks the calling thread (the worker's); the UI thread uses the two calls below, which this is a
 * loop over.
 */
skiff_err skiff_psp_net_connect(skiff_psp_net *net, int profile, long long timeout_us);

/*
 * Starts joining profile `profile` and returns at once; the join gives up timeout_us from now (by
 * the system timer). SKIFF_OK when it is under way: call skiff_psp_net_connect_poll() until it has
 * ended, e.g. once per frame. Otherwise it never started, with skiff_psp_net_connect()'s errors.
 * While a join is pending, skiff_psp_net_disconnect() abandons it and skiff_psp_net_unload()
 * refuses (the firmware still holds a handler pointing at net); one thread at a time uses a net.
 */
skiff_err skiff_psp_net_connect_start(skiff_psp_net *net, int profile, long long timeout_us);

/*
 * Looks at a pending join once, without waiting. Returns 0 while it is still under way. Returns 1
 * once it has ended, with *result its outcome: SKIFF_OK when the access point gave an address,
 * else skiff_psp_net_connect()'s errors (a join that runs past its timeout is
 * SKIFF_ERR_NET_WIFI_JOIN; the state is read before the clock, so one that got its address in time
 * succeeds). Also 1, with SKIFF_ERR_INVALID_ARG, for a NULL net or with no join pending; a NULL
 * result is invalid too (and leaves the join alone).
 */
int skiff_psp_net_connect_poll(skiff_psp_net *net, skiff_err *result);

/*
 * The name of Network Settings profile `profile` (1 is the first) into name. Needs no network
 * module. SKIFF_ERR_NET_UNAVAILABLE when there is no such profile, SKIFF_ERR_BUFFER_TOO_SMALL when
 * the name does not fit (name empty), SKIFF_ERR_INVALID_ARG for a NULL name, no name_size or a
 * profile below 1.
 */
skiff_err skiff_psp_net_profile_name(int profile, char *name, size_t name_size);

/*
 * The profile the current connection uses, into *profile: the one whose name is the connection's
 * (sceNetApctlGetInfo), e.g. after the network picker connected, so it can be joined again later
 * without the picker. Profiles are compared by name, from 1 to SKIFF_PSP_NET_LAST_PROFILE: with
 * two profiles of the same name, the first is taken (it may name another access point, and the
 * next join then fails as with any profile that stopped working). SKIFF_ERR_NET_UNAVAILABLE without
 * a connection or when no profile has that name (*profile untouched); SKIFF_ERR_INVALID_ARG for a
 * NULL argument or before skiff_psp_net_load() finished.
 */
skiff_err skiff_psp_net_connected_profile(skiff_psp_net *net, int *profile);

/* The address the access point gave this PSP, e.g. "192.168.1.20". */
skiff_err skiff_psp_net_ip(skiff_psp_net *net, char *ip, size_t ip_size);

/*
 * Whether the connection still stands, cheap enough to ask during a download: SKIFF_OK while the
 * access point has given an address, SKIFF_ERR_NET_UNAVAILABLE once the Wi-Fi switch is off,
 * SKIFF_ERR_NET_CONNECTION_LOST when the access point is gone (out of range, or after a suspend).
 * Records the state in apctl_state but not as a failed call. SKIFF_ERR_INVALID_ARG before
 * skiff_psp_net_load() finished.
 */
skiff_err skiff_psp_net_online(skiff_psp_net *net);

/*
 * Drops the connection, if any, and waits up to timeout_us until it is reported gone. SKIFF_OK only
 * then; otherwise the modules must stay loaded (process exit releases them). A pending join is
 * abandoned first (its handler removed, the firmware told to disconnect).
 */
skiff_err skiff_psp_net_disconnect(skiff_psp_net *net, long long timeout_us);

/*
 * Undoes skiff_psp_net_load() in reverse, down to stage NONE, then puts the clock back. Refuses
 * while the access point is not disconnected or a join is pending, and stops at the first step that
 * fails: stage then names the layer still live, so the caller can retry or leave the rest to the
 * process exit. The clock is tried on every path, refusals and failed layers included; with Wi-Fi
 * still up the firmware may keep the session clock, and clock_changed then stays set for the next
 * call. SKIFF_ERR_NET_UNAVAILABLE unless both the layers and the clock are back.
 */
skiff_err skiff_psp_net_unload(skiff_psp_net *net);

#endif
