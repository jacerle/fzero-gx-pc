/* Native PAD library: the 4 controller ports come from the host InputManager. */

#include "sdk/sdk_internal.h"

#include "fzgx_port/platform_api.h"

#include <string.h>

/* SDL3 caps one rumble request at 0xFFFF ms; PAD_MOTOR_RUMBLE runs until PAD_MOTOR_STOP. */
#define FZGX_PAD_RUMBLE_HOLD_MS 0xFFFFu

BOOL PADInit(void) {
    u32 chan;
    for (chan = 0; chan < PAD_MAX_CONTROLLERS; chan++) {
        fzgx_pad_set_rumble((int32_t)chan, 0.0f, 0.0f, 0);
    }
    return TRUE;
}

u32 PADRead(PADStatus *status) {
    u32 motor = 0;
    u32 chan;
    for (chan = 0; chan < PAD_MAX_CONTROLLERS; chan++, status++) {
        FzgxPadSnapshot pad;
        fzgx_pad_read((int32_t)chan, &pad);
        memset(status, 0, sizeof(*status));
        if (!pad.connected) {
            status->err = PAD_ERR_NO_CONTROLLER;
            continue;
        }
        /* Host buttons already use the PAD_BUTTON_* / PAD_TRIGGER_* bit layout; the host has
         * no origin to report, so PAD_USE_ORIGIN stays clear as after retail's origin read. */
        status->button = (u16)(pad.buttons & ~PAD_USE_ORIGIN);
        status->stickX = pad.stick_x;
        status->stickY = pad.stick_y;
        status->substickX = pad.substick_x;
        status->substickY = pad.substick_y;
        status->triggerLeft = pad.trigger_l;
        status->triggerRight = pad.trigger_r;
        status->err = PAD_ERR_NONE;
        /* Retail reports every connected standard controller as rumble-capable. */
        motor |= PAD_CHAN0_BIT >> chan;
    }
    return motor;
}

BOOL PADReset(u32 mask) {
    (void)mask; /* host sticks are calibrated by InputManager; there is no origin to re-read */
    return TRUE;
}

BOOL PADRecalibrate(u32 mask) {
    (void)mask;
    return TRUE;
}

void PADControlMotor(s32 chan, u32 command) {
    if (chan < 0 || chan >= PAD_MAX_CONTROLLERS) {
        return;
    }
    if (command == PAD_MOTOR_RUMBLE) {
        fzgx_pad_set_rumble(chan, 1.0f, 1.0f, FZGX_PAD_RUMBLE_HOLD_MS);
    } else {
        fzgx_pad_set_rumble(chan, 0.0f, 0.0f, 0);
    }
}

void PADSetSpec(u32 spec) {
    (void)spec; /* selects retail SI response decoding; host input is already decoded */
}

void PADSetAnalogMode(u32 mode) {
    (void)mode; /* selects retail SI packing of triggers / analog A-B; host input is unpacked */
}
