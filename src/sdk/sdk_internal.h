#ifndef FZGX_SDK_INTERNAL_H
#define FZGX_SDK_INTERNAL_H

/*
 * Shared runtime of the native Dolphin SDK replacement.
 *
 * Retail SDK completions (DVD transfers, CARD commands) arrive later from interrupt handlers,
 * and game code is written for that: it issues an async request, records state, then polls.
 * The native layer performs the host I/O immediately but defers the completion callback to
 * fzgx_sdk_pump(), which runs at the points where retail code could observe an interrupt:
 * VIWaitForRetrace, status polls, and OSRestoreInterrupts/OSEnableInterrupts re-enabling.
 * Pumping is a no-op while interrupts are disabled, matching retail critical sections.
 */

#include "fzgx_port/dolphin_sdk.h"

typedef void (*FzgxSdkDeferredFn)(void *ctx, s32 arg0, s32 arg1);

/* Queues fn(ctx, arg0, arg1) for the next pump. Panics on queue overflow. */
void fzgx_sdk_defer(FzgxSdkDeferredFn fn, void *ctx, s32 arg0, s32 arg1);
/* Runs queued completions in FIFO order, including ones queued while pumping. */
void fzgx_sdk_pump(void);

/* Drops queued completions and re-enables interrupts: a cold boot of the SDK runtime. */
void fzgx_sdk_reset_runtime(void);

/* Interrupt-enable flag behind OSDisableInterrupts / OSRestoreInterrupts. */
BOOL fzgx_sdk_interrupts_enabled(void);
/* Sets the flag and returns the previous value; re-enabling pumps pending completions. */
BOOL fzgx_sdk_set_interrupts_enabled(BOOL enabled);

#endif /* FZGX_SDK_INTERNAL_H */
