#ifndef SKIFF_PSP_LIFECYCLE_H
#define SKIFF_PSP_LIFECYCLE_H

/*
 * The system's callbacks into Skiff, on one callback thread: the HOME menu's exit, and power events
 * (suspend and resume). Callbacks only record what happened; the main loop and the download's stop
 * hook poll it. Also keeps a long download from being cut by auto-sleep.
 */

/* How often skiff_psp_keep_awake() resets the auto-sleep timer at most: far below the shortest
 * Auto Sleep setting, and rare enough to cost nothing when called for every chunk. */
#define SKIFF_PSP_KEEP_AWAKE_INTERVAL_US (5LL * 1000 * 1000)

/* Registers the exit and power callbacks, and returns once they are (or after 100 ms). */
void skiff_psp_install_callbacks(void);

int skiff_psp_exit_requested(void);

typedef struct skiff_psp_power_events {
    /* Suspends begun (PSP_POWER_CB_SUSPENDING) and resumes finished (PSP_POWER_CB_RESUME_COMPLETE)
     * since the callbacks were installed. A change means the network connection is gone. */
    int suspends;
    int resumes;
    /* The PSP_POWER_CB_* flags of the last event, for logs. */
    int last_info;
    /* What scePowerRegisterCallback() returned: a slot, or a negative firmware error. */
    int registration;
} skiff_psp_power_events;

skiff_psp_power_events skiff_psp_power_events_now(void);

/* Resets the auto-sleep timer (scePowerTick(PSP_POWER_TICK_SUSPEND)) at most once per
 * SKIFF_PSP_KEEP_AWAKE_INTERVAL_US. The backlight still dims and turns off as set. */
void skiff_psp_keep_awake(void);

#endif
