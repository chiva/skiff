#ifndef SKIFF_PSP_BACKLIGHT_H
#define SKIFF_PSP_BACKLIGHT_H

#include <stdint.h>

/*
 * The firmware's backlight-off time, Backlight Auto-Off in the XMB's Power Save Settings, through
 * sceImpose (user mode): shortened while a download runs (skiff_ui_backlight_off_s()) and the
 * player's put back after. The firmware still turns the screen off and lights it again on a button;
 * nothing here touches the brightness. A power cycle brings the player's time back too (hardware
 * row B4), so a crash leaves the short time only until then.
 */

/* player_s when the firmware could not say, or refused a change: the backlight is left alone. */
#define SKIFF_PSP_BACKLIGHT_UNKNOWN (-1)

typedef struct skiff_psp_backlight {
    /* The player's off time in seconds (0: never off), or SKIFF_PSP_BACKLIGHT_UNKNOWN. */
    int player_s;
    /* The off time in force: what Skiff set, or the player's. */
    int set_s;
} skiff_psp_backlight;

/* Reads the player's off time. */
void skiff_psp_backlight_start(skiff_psp_backlight *backlight);

/*
 * Sets the off time for a download running or not, calling the firmware only when it changes:
 * 1 when it changed it, 0 when nothing was to change, or the firmware's negative error, after which
 * the backlight is left alone.
 */
int skiff_psp_backlight_update(skiff_psp_backlight *backlight, int downloading);

/* The off time in force in milliseconds, 0 when it is never or unknown. */
uint32_t skiff_psp_backlight_off_ms(const skiff_psp_backlight *backlight);

#endif
