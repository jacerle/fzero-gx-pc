/* Native Gekko state and OS low-memory globals behind include/fzgx_port/decomp_overrides/. */

#include "fzgx_port/ppc_state.h"

_Alignas(32) u8 fzgx_locked_cache[FZGX_LOCKED_CACHE_SIZE];
u32 fzgx_gqr[8];

/* OS low-memory globals (0x800000F8, 0x800000CC on the console). */
u32 __OSBusClock = 162000000u;
u32 __OSTVMode = 0; /* VI_NTSC: USA GFZE01 */
