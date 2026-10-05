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
    FIRST_PROFILE = 1,
    WLAN_SWITCH_OFF = 0,
    /* Before reading the clock back after a change. */
    CLOCK_SETTLE_US = 10 * 1000,
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

static skiff_err wait_for_ip(skiff_psp_net *net, long long timeout_us) {
    /* The state climbs through scanning, joining and getting an address; falling back to
     * DISCONNECTED after it moved means the access point refused or vanished. */
    const long long start_us = sceKernelGetSystemTimeWide();
    int progressed = 0;
    while (sceKernelGetSystemTimeWide() - start_us < timeout_us) {
        int state = PSP_NET_APCTL_STATE_DISCONNECTED;
        if (!step(net, "sceNetApctlGetState", sceNetApctlGetState(&state))) {
            return SKIFF_ERR_NET_WIFI_JOIN;
        }
        net->apctl_state = state;
        if (state == PSP_NET_APCTL_STATE_GOT_IP) {
            return SKIFF_OK;
        }
        if (state != PSP_NET_APCTL_STATE_DISCONNECTED) {
            progressed = 1;
        } else if (progressed || net->apctl_error != 0) {
            return join_failed(net, "sceNetApctlConnect (join failed)");
        }
        sceKernelDelayThread(APCTL_POLL_US);
    }
    return join_failed(net, "sceNetApctlConnect (timed out)");
}

skiff_err skiff_psp_net_connect(skiff_psp_net *net, int profile, long long timeout_us) {
    if (net == NULL || profile < FIRST_PROFILE || net->stage != SKIFF_PSP_NET_APCTL) {
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
    skiff_err err = SKIFF_ERR_NET_WIFI_JOIN;
    if (step(net, "sceNetApctlConnect", sceNetApctlConnect(profile))) {
        err = wait_for_ip(net, timeout_us);
    }
    sceNetApctlDelHandler(handler);
    return err;
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
    if (state == PSP_NET_APCTL_STATE_DISCONNECTED) {
        return SKIFF_OK;
    }
    if (!step(net, "sceNetApctlDisconnect", sceNetApctlDisconnect())) {
        return SKIFF_ERR_NET_UNAVAILABLE;
    }
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

/* Puts back the clock from before skiff_psp_net_load(); a module failure stays the one reported. */
static int restore_clock(skiff_psp_net *net) {
    if (!net->clock_changed) {
        return 1;
    }
    const int result =
        scePowerSetClockFrequency(net->cpu_mhz_before, net->cpu_mhz_before, net->bus_mhz_before);
    if (result < 0) {
        if (net->failed_call == NULL) {
            step(net, "scePowerSetClockFrequency (restore)", result);
        }
        return 0;
    }
    net->clock_changed = 0;
    return 1;
}

skiff_err skiff_psp_net_unload(skiff_psp_net *net) {
    if (net == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    clear_failure(net);
    if (net->stage >= SKIFF_PSP_NET_APCTL) {
        int state = PSP_NET_APCTL_STATE_DISCONNECTED;
        if (!step(net, "sceNetApctlGetState", sceNetApctlGetState(&state))) {
            return SKIFF_ERR_NET_UNAVAILABLE;
        }
        net->apctl_state = state;
        if (state != PSP_NET_APCTL_STATE_DISCONNECTED) {
            step(net, "skiff_psp_net_unload (still connected)", -1);
            return SKIFF_ERR_NET_UNAVAILABLE;
        }
    }
    const int ok =
        (net->stage < SKIFF_PSP_NET_APCTL ||
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
    const int clock_restored = restore_clock(net);
    return ok && clock_restored ? SKIFF_OK : SKIFF_ERR_NET_UNAVAILABLE;
}
