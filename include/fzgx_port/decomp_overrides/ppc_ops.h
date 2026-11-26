/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/ppc_ops.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; cache and time-base instructions get host meaning. */

#ifndef PPC_OPS_H
#define PPC_OPS_H

#include "types.h"
#include "fzgx_port/platform_api.h"

/* dcbi discards a cache line so a later read sees DMA-written memory. Host memory is coherent
 * with every writer the port has, so there is nothing to discard. */
static inline void ppc_dcbi(void *base, s32 offset) {
    (void)base;
    (void)offset;
}

/* The 64-bit time base, which OSGetTime reads: the same clock as the native OSGetTime. */
static inline s64 ppc_get_time_base(void) {
    return (s64)fzgx_os_time_ticks();
}

#endif  // PPC_OPS_H
