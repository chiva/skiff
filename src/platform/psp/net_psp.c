#include "net_psp.h"

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <pspwlan.h>
#include <stdio.h>

enum {
    /* sceNetInit's memory pool and its callout and interrupt threads, as pspsdk's samples set them.
     */
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

skiff_err skiff_psp_net_load(skiff_psp_net *net) {
    if (net == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    net->stage = SKIFF_PSP_NET_NONE;
    net->apctl_state = PSP_NET_APCTL_STATE_DISCONNECTED;
    clear_failure(net);
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
    if (!step(net, "sceNetApctlConnect", sceNetApctlConnect(profile))) {
        return SKIFF_ERR_NET_WIFI_JOIN;
    }
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
        } else if (progressed) {
            step(net, "sceNetApctlConnect (join failed)", -1);
            return SKIFF_ERR_NET_WIFI_JOIN;
        }
        sceKernelDelayThread(APCTL_POLL_US);
    }
    step(net, "sceNetApctlConnect (timed out)", -1);
    return SKIFF_ERR_NET_WIFI_JOIN;
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

skiff_err skiff_psp_net_unload(skiff_psp_net *net) {
    if (net == NULL) {
        return SKIFF_ERR_INVALID_ARG;
    }
    clear_failure(net);
    int ok = 1;
    if (net->stage >= SKIFF_PSP_NET_APCTL) {
        ok &= step(net, "sceNetApctlTerm", sceNetApctlTerm());
    }
    if (net->stage >= SKIFF_PSP_NET_RESOLVER) {
        ok &= step(net, "sceNetResolverTerm", sceNetResolverTerm());
    }
    if (net->stage >= SKIFF_PSP_NET_INET) {
        ok &= step(net, "sceNetInetTerm", sceNetInetTerm());
    }
    if (net->stage >= SKIFF_PSP_NET_NET) {
        ok &= step(net, "sceNetTerm", sceNetTerm());
    }
    if (net->stage >= SKIFF_PSP_NET_INET_MODULE) {
        ok &= step(net, "sceUtilityUnloadNetModule(INET)",
                   sceUtilityUnloadNetModule(PSP_NET_MODULE_INET));
    }
    if (net->stage >= SKIFF_PSP_NET_COMMON_MODULE) {
        ok &= step(net, "sceUtilityUnloadNetModule(COMMON)",
                   sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON));
    }
    net->stage = SKIFF_PSP_NET_NONE;
    return ok ? SKIFF_OK : SKIFF_ERR_NET_UNAVAILABLE;
}
