/* fzero-gx-pc native replacement for external/fzero-gx-decomp/include/dolphin/os/OSFastCast.h
 * (R4 platform boundary: the port supplies the hardware/asm kernel headers). Same API and
 * include guard as the original; the single-lane quantized psq_st/psq_l casts become C with the
 * formats OSInitFastCast programs (GQR2..5 = u8, u16, s8, s16, scale 0): float to integer
 * truncates toward zero and saturates. */

#ifndef DOLPHIN_OS_OSFASTCAST_H
#define DOLPHIN_OS_OSFASTCAST_H

#include "dolphin/types.h"
#include "fzgx_port/ppc_state.h"

#define OS_GQR_F32 0x0000
#define OS_GQR_U8 0x0004
#define OS_GQR_U16 0x0005
#define OS_GQR_S8 0x0006
#define OS_GQR_S16 0x0007

#define OS_FASTCAST_U8 2
#define OS_FASTCAST_U16 3
#define OS_FASTCAST_S8 4
#define OS_FASTCAST_S16 5

static inline u8 __OSf32tou8(f32 inF) {
    u8 out;
    fzgx_ps_store_elem(&out, inF, OS_GQR_U8, 0);
    return out;
}

static inline u16 __OSf32tou16(f32 inF) {
    u16 out;
    fzgx_ps_store_elem((u8 *)&out, inF, OS_GQR_U16, 0);
    return out;
}

static inline s8 __OSf32tos8(f32 inF) {
    s8 out;
    fzgx_ps_store_elem((u8 *)&out, inF, OS_GQR_S8, 0);
    return out;
}

static inline s16 __OSf32tos16(f32 inF) {
    s16 out;
    fzgx_ps_store_elem((u8 *)&out, inF, OS_GQR_S16, 0);
    return out;
}

static inline f32 __OSu8tof32(u8 *in) {
    return (f32)*in;
}

static inline f32 __OSu16tof32(u16 *in) {
    return (f32)*in;
}

static inline f32 __OSs8tof32(s8 *in) {
    return (f32)*in;
}

static inline f32 __OSs16tof32(s16 *in) {
    return (f32)*in;
}

#endif  // DOLPHIN_OS_OSFASTCAST_H
