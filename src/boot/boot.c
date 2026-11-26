#include "boot/boot.h"

#include "fzgx_port/dolphin_sdk.h"
#include "sdk/sdk_internal.h"

/* PADSetAnalogMode argument used by retail fn_800058D8. */
#define FZGX_RETAIL_PAD_ANALOG_MODE 3u

const FzgxGameEntry fzgx_main_module_entry = NULL;

FzgxBootResult fzgx_boot_main(FzgxGameEntry entry) {
    if (!DVDCheckDisk()) {
        return FZGX_BOOT_NO_DISC;
    }
    /* A previous run's completions target memory of a game that no longer runs. */
    fzgx_sdk_reset_runtime();

    /* Retail order from fn_80005660 / fn_800058D8; CARDInit is the game's own (main_rel _prolog). */
    OSInit();
    DVDInit();
    VIInit();
    PADInit();
    PADSetAnalogMode(FZGX_RETAIL_PAD_ANALOG_MODE);

    if (entry == NULL) {
        return FZGX_BOOT_NO_ENTRY;
    }
    entry();
    return FZGX_BOOT_ENTRY_RETURNED;
}
