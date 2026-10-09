#include "net_psp.h"

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psppower.h>
#include <psputility.h>
#include <pspwlan.h>
#include <stdio.h>
#include <string.h>

enum {
    /* sceNetInit's pool and its callout and interrupt threads, as pspsdk's samples set them. */
    NET_POOL_BYTES = 128 * 1024,
    NET_CALLOUT_PRIORITY = 42,
    NET_CALLOUT_STACK_BYTES = 4 * 1024,
    NET_INTERRUPT_PRIORITY = 42,
    NET_INTERRUPT_STACK_BYTES = 4 * 1024,
    APCTL_STACK_BYTES = 0x8000,
    APCTL_PRIORITY = 48,
    APCTL_POLL_US = 50 * 1000,
    WLAN_SWITCH_OFF = 0,
    /* Before reading the clock back after a change. */
    CLOCK_SETTLE_US = 10 * 1000,
    /* The work area one resolver needs (pspsdk's samples give it 1 KB). */
    RESOLVER_BUFFER_BYTES = 1024,
    IPV4_TEXT_MAX = 16,
};

/* Records a failed call; returns whether `result` is a success. */
static int step(skiff_psp_net *net, const char *call, int result) {
    if (result < 0) {
        net->failed_call = call;
        net->sce_result = result;
        return 0;
    }
    return 1;
}

static void clear_failure(skiff_psp_net *net) {
    net->failed_call = NULL;
    net->sce_result = 0;
}

static void read_clock(skiff_psp_net *net) {
    net->cpu_mhz = scePowerGetCpuClockFrequency();
    net->bus_mhz = scePowerGetBusClockFrequency();
}

/* Best effort: the network works at any clock, so a refused change only shows in net->cpu_mhz. */
static void set_session_clock(skiff_psp_net *net, int cpu_mhz) {
    read_clock(net);
    net->cpu_mhz_before = net->cpu_mhz;
    net->bus_mhz_before = net->bus_mhz;
    const int bus_mhz = cpu_mhz / 2;
    if (cpu_mhz == SKIFF_PSP_NET_CPU_MHZ_UNCHANGED ||
        (cpu_mhz == net->cpu_mhz && bus_mhz == net->bus_mhz) ||
        scePowerSetClockFrequency(cpu_mhz, cpu_mhz, bus_mhz) < 0) {
        return;
    }
    net->clock_changed = 1;
    sceKernelDelayThread(CLOCK_SETTLE_US);
    read_clock(net);
}

skiff_err skiff_psp_net_load(skiff_psp_net *net, int cpu_mhz) {
    if (net == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    net->stage = SKIFF_PSP_NET_NONE;
    net->apctl_state = PSP_NET_APCTL_STATE_DISCONNECTED;
    net->apctl_furthest_state = PSP_NET_APCTL_STATE_DISCONNECTED;
    net->apctl_error = 0;
    net->clock_changed = 0;
    net->join_pending = 0;
    clear_failure(net);
    if (cpu_mhz < SKIFF_PSP_NET_CPU_MHZ_UNCHANGED || cpu_mhz > SKIFF_PSP_NET_CPU_MHZ) {
        return SKIFF_ERR_INVALID_ARG;
    }
    set_session_clock(net, cpu_mhz);
    if (!step(net, "sceUtilityLoadNetModule(COMMON)",
              sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON))) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    net->stage = SKIFF_PSP_NET_COMMON_MODULE;
    if (!step(net, "sceUtilityLoadNetModule(INET)", sceUtilityLoadNetModule(PSP_NET_MODULE_INET))) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    net->stage = SKIFF_PSP_NET_INET_MODULE;
    if (!step(net, "sceNetInit",
              sceNetInit(NET_POOL_BYTES, NET_CALLOUT_PRIORITY, NET_CALLOUT_STACK_BYTES,
                         NET_INTERRUPT_PRIORITY, NET_INTERRUPT_STACK_BYTES))) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    net->stage = SKIFF_PSP_NET_NET;
    if (!step(net, "sceNetInetInit", sceNetInetInit())) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    net->stage = SKIFF_PSP_NET_INET;
    /* The C library's getaddrinfo() creates resolvers, which needs this. */
    if (!step(net, "sceNetResolverInit", sceNetResolverInit())) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    net->stage = SKIFF_PSP_NET_RESOLVER;
    if (!step(net, "sceNetApctlInit", sceNetApctlInit(APCTL_STACK_BYTES, APCTL_PRIORITY))) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    net->stage = SKIFF_PSP_NET_APCTL;
    return SKIFF_OK;
}

/* Called by the firmware on its own thread at each step of a join; keeps what a failure needs. */
static void on_apctl_event(int old_state, int new_state, int event, int error, void *context) {
    (void)old_state;
    skiff_psp_net *net = context;
    if (new_state > net->apctl_furthest_state) {
        net->apctl_furthest_state = new_state;
    }
    if (event == PSP_NET_APCTL_EVENT_ERROR) {
        net->apctl_error = error;
    }
}

/* A join that failed: the firmware's own error when it gave one, else the step that noticed. */
static skiff_err join_failed(skiff_psp_net *net, const char *call) {
    net->failed_call = call;
    net->sce_result = net->apctl_error != 0 ? net->apctl_error : -1;
    return SKIFF_ERR_NET_WIFI_JOIN;
}

/* Removes a pending join's event handler: after this the firmware no longer writes into net. */
static void end_join(skiff_psp_net *net) {
    if (net->join_pending) {
        sceNetApctlDelHandler(net->join_handler);
        net->join_pending = 0;
    }
}

/*
 * One look at a pending join; returns 1 once it has ended, with *result its outcome. The state
 * climbs through scanning, joining and getting an address; falling back to DISCONNECTED after it
 * moved means the access point refused or vanished.
 */
static int join_ended(skiff_psp_net *net, skiff_err *result) {
    int state = PSP_NET_APCTL_STATE_DISCONNECTED;
    if (!step(net, "sceNetApctlGetState", sceNetApctlGetState(&state))) {
        *result = SKIFF_ERR_NET_WIFI_JOIN;
        return 1;
    }
    net->apctl_state = state;
    if (state == PSP_NET_APCTL_STATE_GOT_IP) {
        *result = SKIFF_OK;
        return 1;
    }
    if (state != PSP_NET_APCTL_STATE_DISCONNECTED) {
        net->join_progressed = 1;
    } else if (net->join_progressed || net->apctl_error != 0) {
        *result = join_failed(net, "sceNetApctlConnect (join failed)");
        return 1;
    }
    if (sceKernelGetSystemTimeWide() >= net->join_deadline_us) {
        *result = join_failed(net, "sceNetApctlConnect (timed out)");
        return 1;
    }
    return 0;
}

skiff_err skiff_psp_net_connect_start(skiff_psp_net *net, int profile, long long timeout_us) {
    if (net == NULL || profile < SKIFF_PSP_NET_FIRST_PROFILE || net->stage != SKIFF_PSP_NET_APCTL ||
        net->join_pending) {
        return SKIFF_ERR_INVALID_ARG;
    }
    clear_failure(net);
    if (sceWlanGetSwitchState() == WLAN_SWITCH_OFF) {
        step(net, "sceWlanGetSwitchState (switch off)", -1);
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    if (!step(net, "sceUtilityCheckNetParam", sceUtilityCheckNetParam(profile))) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    net->apctl_furthest_state = PSP_NET_APCTL_STATE_DISCONNECTED;
    net->apctl_error = 0;
    const int handler = sceNetApctlAddHandler(on_apctl_event, net);
    if (!step(net, "sceNetApctlAddHandler", handler)) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    if (!step(net, "sceNetApctlConnect", sceNetApctlConnect(profile))) {
        sceNetApctlDelHandler(handler);
        return SKIFF_ERR_NET_WIFI_JOIN;
    }
    net->join_pending = 1;
    net->join_handler = handler;
    net->join_progressed = 0;
    net->join_deadline_us = (long long)sceKernelGetSystemTimeWide() + timeout_us;
    return SKIFF_OK;
}

int skiff_psp_net_connect_poll(skiff_psp_net *net, skiff_err *result) {
    if (result == NULL) {
        return 1;
    }
    if (net == NULL || !net->join_pending) {
        *result = SKIFF_ERR_INVALID_ARG;
        return 1;
    }
    if (!join_ended(net, result)) {
        return 0;
    }
    end_join(net);
    return 1;
}

skiff_err skiff_psp_net_connect(skiff_psp_net *net, int profile, long long timeout_us) {
    skiff_err err = skiff_psp_net_connect_start(net, profile, timeout_us);
    if (err != SKIFF_OK) {
        return err;
    }
    while (!skiff_psp_net_connect_poll(net, &err)) {
        sceKernelDelayThread(APCTL_POLL_US);
    }
    return err;
}

skiff_err skiff_psp_net_profile_name(int profile, char *name, size_t name_size) {
    if (name != NULL && name_size > 0) {
        name[0] = '\0';
    }
    if (name == NULL || name_size == 0 || profile < SKIFF_PSP_NET_FIRST_PROFILE) {
        return SKIFF_ERR_INVALID_ARG;
    }
    netData data;
    memset(&data, 0, sizeof data);
    if (sceUtilityCheckNetParam(profile) < 0 ||
        sceUtilityGetNetParam(profile, PSP_NETPARAM_NAME, &data) < 0) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    data.asString[sizeof data.asString - 1] = '\0';
    const size_t length = strlen(data.asString);
    if (length >= name_size) {
        return SKIFF_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(name, data.asString, length + 1);
    return SKIFF_OK;
}

skiff_err skiff_psp_net_connected_profile(skiff_psp_net *net, int *profile) {
    if (net == NULL || profile == NULL || net->stage != SKIFF_PSP_NET_APCTL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    clear_failure(net);
    int state = PSP_NET_APCTL_STATE_DISCONNECTED;
    if (!step(net, "sceNetApctlGetState", sceNetApctlGetState(&state))) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    net->apctl_state = state;
    if (state != PSP_NET_APCTL_STATE_GOT_IP) {
        step(net, "skiff_psp_net_connected_profile (not connected)", -1);
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    union SceNetApctlInfo info;
    memset(&info, 0, sizeof info);
    if (!step(net, "sceNetApctlGetInfo(PROFILE_NAME)",
              sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PROFILE_NAME, &info))) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    info.name[sizeof info.name - 1] = '\0';
    for (int candidate = SKIFF_PSP_NET_FIRST_PROFILE; candidate <= SKIFF_PSP_NET_LAST_PROFILE;
         candidate++) {
        char name[SKIFF_PSP_NET_PROFILE_NAME_MAX];
        if (skiff_psp_net_profile_name(candidate, name, sizeof name) == SKIFF_OK &&
            strcmp(name, info.name) == 0) {
            *profile = candidate;
            return SKIFF_OK;
        }
    }
    step(net, "skiff_psp_net_connected_profile (no profile has its name)", -1);
    return SKIFF_ERR_NET_UNAVAILABLE;
}

/* Digits and three dots: an address curl can use as it is (it rejects a malformed one). */
static int is_dotted_address(const char *host) {
    int dots = 0;
    for (const char *at = host; *at != '\0'; at++) {
        if (*at == '.') {
            dots++;
        } else if (*at < '0' || *at > '9') {
            return 0;
        }
    }
    return dots == 3;
}

skiff_err skiff_psp_net_resolve(const char *host, unsigned timeout_s, int retries, char *address,
                                size_t address_size) {
    if (host == NULL || host[0] == '\0' || address == NULL || address_size < IPV4_TEXT_MAX) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (is_dotted_address(host)) {
        return snprintf(address, address_size, "%s", host) < (int)address_size
                   ? SKIFF_OK
                   : SKIFF_ERR_INVALID_ARG;
    }
    unsigned char work[RESOLVER_BUFFER_BYTES];
    int resolver = 0;
    if (sceNetResolverCreate(&resolver, work, sizeof work) < 0) {
        return SKIFF_ERR_NET_DNS;
    }
    struct in_addr found;
    memset(&found, 0, sizeof found);
    const int result = sceNetResolverStartNtoA(resolver, host, &found, timeout_s, retries);
    sceNetResolverDelete(resolver);
    if (result < 0) {
        return SKIFF_ERR_NET_DNS;
    }
    /* s_addr is in network order: its bytes are the address's, first to last. */
    const unsigned char *bytes = (const unsigned char *)&found.s_addr;
    snprintf(address, address_size, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    return SKIFF_OK;
}

skiff_err skiff_psp_net_ip(skiff_psp_net *net, char *ip, size_t ip_size) {
    if (net == NULL || ip == NULL || ip_size < SKIFF_PSP_NET_IP_MAX) {
        return SKIFF_ERR_INVALID_ARG;
    }
    ip[0] = '\0';
    union SceNetApctlInfo info;
    if (!step(net, "sceNetApctlGetInfo(IP)", sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info))) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    snprintf(ip, ip_size, "%s", info.ip);
    return SKIFF_OK;
}

skiff_err skiff_psp_net_online(skiff_psp_net *net) {
    if (net == NULL || net->stage != SKIFF_PSP_NET_APCTL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    if (sceWlanGetSwitchState() == WLAN_SWITCH_OFF) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    int state = PSP_NET_APCTL_STATE_DISCONNECTED;
    if (sceNetApctlGetState(&state) < 0) {
        return SKIFF_ERR_NET_CONNECTION_LOST;
    }
    net->apctl_state = state;
    return state == PSP_NET_APCTL_STATE_GOT_IP ? SKIFF_OK : SKIFF_ERR_NET_CONNECTION_LOST;
}

skiff_err skiff_psp_net_disconnect(skiff_psp_net *net, long long timeout_us) {
    if (net == NULL || net->stage != SKIFF_PSP_NET_APCTL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    clear_failure(net);
    int state = PSP_NET_APCTL_STATE_DISCONNECTED;
    if (!step(net, "sceNetApctlGetState", sceNetApctlGetState(&state))) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    net->apctl_state = state;
    if (state == PSP_NET_APCTL_STATE_DISCONNECTED && !net->join_pending) {
        return SKIFF_OK;
    }
    /* A join just asked for may still read DISCONNECTED before it starts scanning, so it is told to
     * stop all the same. Until the firmware accepts that, the join stays pending: unloading must
     * still refuse. */
    if (!step(net, "sceNetApctlDisconnect", sceNetApctlDisconnect())) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
    end_join(net);
    const long long start_us = sceKernelGetSystemTimeWide();
    while (sceKernelGetSystemTimeWide() - start_us < timeout_us) {
        if (step(net, "sceNetApctlGetState", sceNetApctlGetState(&state))) {
            net->apctl_state = state;
            if (state == PSP_NET_APCTL_STATE_DISCONNECTED) {
                clear_failure(net);
                return SKIFF_OK;
            }
        }
        sceKernelDelayThread(APCTL_POLL_US);
    }
    step(net, "sceNetApctlDisconnect (timed out)", -1);
    return SKIFF_ERR_NET_TIMEOUT;
}

/* One layer of the teardown: on failure the stage stays at this layer, which is still live. */
static int term(skiff_psp_net *net, skiff_psp_net_stage layer, const char *call, int result) {
    if (!step(net, call, result)) {
        return 0;
    }
    net->stage = (skiff_psp_net_stage)(layer - 1);
    return 1;
}

/*
 * Puts back the clock from before skiff_psp_net_load(). Done only once it reads back: with Wi-Fi
 * up the firmware accepts the call and keeps its clock, so clock_changed stays set for a retry. An
 * earlier failure stays the one reported.
 */
static int restore_clock(skiff_psp_net *net) {
    if (!net->clock_changed) {
        return 1;
    }
    const int result =
        scePowerSetClockFrequency(net->cpu_mhz_before, net->cpu_mhz_before, net->bus_mhz_before);
    sceKernelDelayThread(CLOCK_SETTLE_US);
    net->clock_changed = scePowerGetCpuClockFrequency() != net->cpu_mhz_before ||
                         scePowerGetBusClockFrequency() != net->bus_mhz_before;
    if (net->clock_changed && net->failed_call == NULL) {
        step(net, "scePowerSetClockFrequency (restore, clock kept)", result < 0 ? result : -1);
    }
    return !net->clock_changed;
}

/* Whether the layers may come down: never while the access point is connected or being joined. */
static int access_point_released(skiff_psp_net *net) {
    if (net->stage < SKIFF_PSP_NET_APCTL) {
        return 1;
    }
    if (net->join_pending) {
        step(net, "skiff_psp_net_unload (join pending)", -1);
        return 0;
    }
    int state = PSP_NET_APCTL_STATE_DISCONNECTED;
    if (!step(net, "sceNetApctlGetState", sceNetApctlGetState(&state))) {
        return 0;
    }
    net->apctl_state = state;
    if (state != PSP_NET_APCTL_STATE_DISCONNECTED) {
        step(net, "skiff_psp_net_unload (still connected)", -1);
        return 0;
    }
    return 1;
}

static int unload_layers(skiff_psp_net *net) {
    return (net->stage < SKIFF_PSP_NET_APCTL ||
            term(net, SKIFF_PSP_NET_APCTL, "sceNetApctlTerm", sceNetApctlTerm())) &&
           (net->stage < SKIFF_PSP_NET_RESOLVER ||
            term(net, SKIFF_PSP_NET_RESOLVER, "sceNetResolverTerm", sceNetResolverTerm())) &&
           (net->stage < SKIFF_PSP_NET_INET ||
            term(net, SKIFF_PSP_NET_INET, "sceNetInetTerm", sceNetInetTerm())) &&
           (net->stage < SKIFF_PSP_NET_NET ||
            term(net, SKIFF_PSP_NET_NET, "sceNetTerm", sceNetTerm())) &&
           (net->stage < SKIFF_PSP_NET_INET_MODULE ||
            term(net, SKIFF_PSP_NET_INET_MODULE, "sceUtilityUnloadNetModule(INET)",
                 sceUtilityUnloadNetModule(PSP_NET_MODULE_INET))) &&
           (net->stage < SKIFF_PSP_NET_COMMON_MODULE ||
            term(net, SKIFF_PSP_NET_COMMON_MODULE, "sceUtilityUnloadNetModule(COMMON)",
                 sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON)));
}

skiff_err skiff_psp_net_unload(skiff_psp_net *net) {
    if (net == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    clear_failure(net);
    const int layers_down = access_point_released(net) && unload_layers(net);
    /* Also after a refusal or a failed layer, so no exit path leaves the session clock behind. */
    const int clock_restored = restore_clock(net);
    return layers_down && clock_restored ? SKIFF_OK : SKIFF_ERR_NET_UNAVAILABLE;
}
