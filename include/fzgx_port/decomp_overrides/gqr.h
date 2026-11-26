/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/gqr.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; mfspr/mtspr GQRn act on the emulated GQRs that
 * the quantized kernels of psvec_quant.h read. */

#ifndef GQR_H
#define GQR_H

#include "types.h"
#include "fzgx_port/ppc_state.h"

static inline u32 gqr_get2(void) { return fzgx_gqr[2]; }
static inline u32 gqr_get5(void) { return fzgx_gqr[5]; }
static inline u32 gqr_get6(void) { return fzgx_gqr[6]; }
static inline u32 gqr_get7(void) { return fzgx_gqr[7]; }

static inline void gqr_put2(u32 v) { fzgx_gqr[2] = v; }
static inline void gqr_put5(u32 v) { fzgx_gqr[5] = v; }
static inline void gqr_put6(u32 v) { fzgx_gqr[6] = v; }
static inline void gqr_put7(u32 v) { fzgx_gqr[7] = v; }

/* The image decoder's dequantization formats. */
static inline void gqr_set2_decode(void) { fzgx_gqr[2] = 0x00040004u; }
static inline void gqr_set5_decode(void) { fzgx_gqr[5] = 0x00070007u; }
static inline void gqr_set6_decode(void) { fzgx_gqr[6] = 0x3D043D04u; }
static inline void gqr_set7_decode(void) { fzgx_gqr[7] = 0x01040104u; }

#endif  // GQR_H
