#include "backlight_psp.h"

#include <pspimpose.h>

#include "skiff/ui.h"

#define MS_PER_S 1000U

void skiff_psp_backlight_start(skiff_psp_backlight *backlight) {
    const int player_s = sceImposeGetBacklightOffTime();
    backlight->player_s = player_s >= 0 ? player_s : SKIFF_PSP_BACKLIGHT_UNKNOWN;
    backlight->set_s = backlight->player_s;
}

int skiff_psp_backlight_update(skiff_psp_backlight *backlight, int downloading) {
    if (backlight->player_s == SKIFF_PSP_BACKLIGHT_UNKNOWN) {
        return 0;
    }
    const int wanted_s = skiff_ui_backlight_off_s(backlight->player_s, downloading);
    if (wanted_s == backlight->set_s) {
        return 0;
    }
    const int result = sceImposeSetBacklightOffTime(wanted_s);
    if (result < 0) {
        backlight->player_s = SKIFF_PSP_BACKLIGHT_UNKNOWN;
        return result;
    }
    backlight->set_s = wanted_s;
    return 1;
}

uint32_t skiff_psp_backlight_off_ms(const skiff_psp_backlight *backlight) {
    return backlight->set_s > 0 ? (uint32_t)backlight->set_s * MS_PER_S : 0;
}
