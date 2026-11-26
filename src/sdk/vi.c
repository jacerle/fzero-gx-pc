/*
 * Native VI library. Retrace timing comes from the host through fzgx_vi_wait_for_retrace
 * (host frame via the retrace hook, then 59.94 Hz pacing). The host renderer owns the display mode, scan-out
 * and blanking, so the video-encoder programming calls (VIConfigure, VIFlush, VISetBlack) have
 * no host counterpart.
 */

#include "sdk/sdk_internal.h"

#include "fzgx_port/platform_api.h"

static u32 s_retrace_count;
static VIRetraceCallback s_pre_cb;
static VIRetraceCallback s_post_cb;
static void *s_next_fb;

void VIInit(void) {
    s_retrace_count = 0;
    s_pre_cb = NULL;
    s_post_cb = NULL;
    s_next_fb = NULL;
}

void VIConfigure(const GXRenderModeObj *rm) {
    (void)rm;
}

void VIFlush(void) {
}

void VISetNextFrameBuffer(void *fb) {
    s_next_fb = fb;
}

void *VIGetNextFrameBuffer(void) {
    return s_next_fb;
}

void VISetBlack(BOOL black) {
    (void)black;
}

void VIWaitForRetrace(void) {
    fzgx_vi_wait_for_retrace();
    /* Retail retrace interrupt order: count, pre-retrace callback, register update, post callback. */
    s_retrace_count++;
    if (s_pre_cb != NULL) {
        s_pre_cb(s_retrace_count);
    }
    fzgx_sdk_pump();
    if (s_post_cb != NULL) {
        s_post_cb(s_retrace_count);
    }
}

u32 VIGetRetraceCount(void) {
    return s_retrace_count;
}

u32 VIGetNextField(void) {
    /* Interlaced fields alternate per retrace: 1 = above (odd), 0 = below (even). */
    return (s_retrace_count + 1u) & 1u;
}

u32 VIGetCurrentLine(void) {
    return 0; /* called right after retrace waits: the beam is at the top */
}

u32 VIGetTvFormat(void) {
    return VI_NTSC; /* USA GFZE01 */
}

u32 VIGetDTVStatus(void) {
    return 1; /* component cable present: the host always outputs progressive */
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback) {
    const VIRetraceCallback old = s_pre_cb;
    s_pre_cb = callback;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback) {
    const VIRetraceCallback old = s_post_cb;
    s_post_cb = callback;
    return old;
}
