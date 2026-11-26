/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/ppc_lc.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; dcbz_l zeroes the 32-byte line, as on hardware. */

#ifndef PPC_LC_H
#define PPC_LC_H

#include "types.h"

static inline void ppc_dcbz_l(void *base, s32 offset) {
    fzgx_dcbz(base, offset);
}

#endif  // PPC_LC_H
