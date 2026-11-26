/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/dolphin/hw_regs.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; console addresses become host objects.
 * Deliberately absent: GX_FIFO_BASE, VI_REG_LITERAL / VI_DTV_STAT and the FZGX_ADDR_* map
 * (sdk_addresses.h) -- there is no memory-mapped hardware, so code using them must move to an
 * SDK interface and fails to compile until it does. */

#ifndef DOLPHIN_HW_REGS_H
#define DOLPHIN_HW_REGS_H

#include "dolphin/types.h"
#include "fzgx_port/ppc_state.h"

/* The locked-cache window. A pointer, not the console's integer address: code that stores it
 * in a u32 must not compile on a 64-bit host. */
#define LC_BASE ((u8 *)fzgx_locked_cache)

/* OS low-memory globals, as ordinary objects (src/decomp_port/ppc_state.c). */
extern u32 __OSBusClock; /* 162 MHz */
extern u32 __OSTVMode;   /* VI_NTSC */
#define OS_BUS_CLOCK 162000000u
#define OS_TIMER_CLOCK (OS_BUS_CLOCK / 4)

#endif
