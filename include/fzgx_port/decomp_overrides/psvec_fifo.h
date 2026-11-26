/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/psvec_fifo.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; FIFO uploads go to fzgx_gx_fifo_*.
 * `dest` is the write-gather port (&GXWGFifo) on the console; the native FIFO has one port, so it is unused. */

#ifndef PSVEC_FIFO_H
#define PSVEC_FIFO_H

#include "types.h"


static inline void fzgx_write_f32s_to_fifo(const f32 *src, int count) {
    int i;
    for (i = 0; i < count; i++) {
        fzgx_gx_fifo_f32(src[i]);
    }
}

/* Twelve f32 from mtx. */
static inline void WriteMTXPS4x3(const void *mtx, volatile void *dest) {
    (void)dest;
    fzgx_write_f32s_to_fifo((const f32 *)mtx, 12);
}

/* Eight f32 from mtx. */
static inline void WriteMTXPS4x2(const void *mtx, volatile void *dest) {
    (void)dest;
    fzgx_write_f32s_to_fifo((const f32 *)mtx, 8);
}

/* Columns 0..2 of each row of a 3x4 matrix. */
static inline void WriteMTXPS3x3from3x4(const void *mtx, volatile void *dest) {
    const f32 *m = (const f32 *)mtx;
    (void)dest;
    fzgx_write_f32s_to_fifo(m + 0, 3);
    fzgx_write_f32s_to_fifo(m + 4, 3);
    fzgx_write_f32s_to_fifo(m + 8, 3);
}

/* Three zero words, the color word at +0xC, then twelve f32 from +0x10. */
static inline void WriteLightObjPS(const void *src, volatile void *dest) {
    const u8 *s = (const u8 *)src;
    u32 color;
    (void)dest;
    fzgx_copy_bytes(&color, s + 12, sizeof(color));
    fzgx_gx_fifo_u32(0);
    fzgx_gx_fifo_u32(0);
    fzgx_gx_fifo_u32(0);
    fzgx_gx_fifo_u32(color);
    fzgx_write_f32s_to_fifo((const f32 *)(s + 16), 12);
}

#endif  // PSVEC_FIFO_H
