#include "sdk/sdk_internal.h"

#define FZGX_SDK_QUEUE_CAPACITY 256u

typedef struct DeferredCall {
    FzgxSdkDeferredFn fn;
    void *ctx;
    s32 arg0;
    s32 arg1;
} DeferredCall;

static DeferredCall s_queue[FZGX_SDK_QUEUE_CAPACITY];
static u32 s_head;  /* next entry to run */
static u32 s_count; /* queued entries */
static BOOL s_interrupts_enabled = TRUE;
static BOOL s_pumping;

void fzgx_sdk_defer(FzgxSdkDeferredFn fn, void *ctx, s32 arg0, s32 arg1) {
    DeferredCall *slot;
    if (s_count == FZGX_SDK_QUEUE_CAPACITY) {
        OSPanic(__FILE__, __LINE__, "deferred SDK completion queue overflow (%u entries)\n",
                (unsigned)FZGX_SDK_QUEUE_CAPACITY);
    }
    slot = &s_queue[(s_head + s_count) % FZGX_SDK_QUEUE_CAPACITY];
    slot->fn = fn;
    slot->ctx = ctx;
    slot->arg0 = arg0;
    slot->arg1 = arg1;
    s_count++;
}

void fzgx_sdk_pump(void) {
    /* Callbacks may poll SDK status functions, which pump again: run each entry exactly once. */
    if (!s_interrupts_enabled || s_pumping) {
        return;
    }
    s_pumping = TRUE;
    while (s_count != 0 && s_interrupts_enabled) {
        const DeferredCall call = s_queue[s_head];
        s_head = (s_head + 1) % FZGX_SDK_QUEUE_CAPACITY;
        s_count--;
        call.fn(call.ctx, call.arg0, call.arg1);
    }
    s_pumping = FALSE;
}

void fzgx_sdk_reset_runtime(void) {
    s_head = 0;
    s_count = 0;
    s_interrupts_enabled = TRUE;
}

BOOL fzgx_sdk_interrupts_enabled(void) {
    return s_interrupts_enabled;
}

BOOL fzgx_sdk_set_interrupts_enabled(BOOL enabled) {
    const BOOL previous = s_interrupts_enabled;
    s_interrupts_enabled = enabled ? TRUE : FALSE;
    if (s_interrupts_enabled && !previous) {
        fzgx_sdk_pump();
    }
    return previous;
}
