/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/dolphin/gx.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; FIFO writes go to fzgx_gx_fifo_*.
 * There is no GXWGFifo object: a direct store to the write-gather port cannot be observed in C,
 * so such code fails to compile until it uses the GX_WRITE_* macros or a GX call. */

#ifndef DOLPHIN_GX_H
#define DOLPHIN_GX_H

#include "dolphin/types.h"
#include "dolphin/hw_regs.h"

typedef union PPCWGPipe {
    u8 u8;
    u16 u16;
    u32 u32;
    u64 u64;
    s8 s8;
    s16 s16;
    s32 s32;
    s64 s64;
    f32 f32;
    f64 f64;
} PPCWGPipe;
typedef PPCWGPipe WGPipe;

#define GX_WRITE_U8(v) fzgx_gx_fifo_u8((u8)(v))
#define GX_WRITE_U16(v) fzgx_gx_fifo_u16((u16)(v))
#define GX_WRITE_U32(v) fzgx_gx_fifo_u32((u32)(v))
#define GX_WRITE_F32(v) fzgx_gx_fifo_f32((f32)(v))

/* BP (raster) register write: command byte 0x61 followed by the register word. */
#define GX_WRITE_RAS_REG(value)          \
    do {                                 \
        fzgx_gx_fifo_u8(0x61);           \
        fzgx_gx_fifo_u32((u32)(value));  \
    } while (0)

/* XF register write: command byte 0x10, then (count - 1) << 16 | address, then the data words. */
#define GX_WRITE_XF_REG_HDR(addr, count)                        \
    do {                                                        \
        fzgx_gx_fifo_u8(0x10);                                  \
        fzgx_gx_fifo_u32((u32)((((count) - 1) << 16) | (addr))); \
    } while (0)

#endif
