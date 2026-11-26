#ifndef FZGX_PORT_MWCC_SHIM_H
#define FZGX_PORT_MWCC_SHIM_H

/*
 * Portable 64-bit C11/C++23 compatibility shim for Metrowerks CodeWarrior (MWCC)
 * constructs still present in external/fzero-gx-decomp during upstream R1-R7 cleanup.
 *
 * Key difference from decomp's original types.h:
 *   On 64-bit LP64 Linux, `long` is 64-bit, whereas GameCube expects 32-bit s32/u32.
 *   Compiling decomp TUs with this header pre-included keeps s32/u32 exact 32-bit widths.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TYPES_H
#define TYPES_H
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;
typedef int32_t BOOL;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#endif /* TYPES_H */

/* MWCC storage-class / pragma / qualifier compatibility */
#ifndef __MWERKS__
#ifndef register
#define register
#endif
#endif

/* Endian helpers (host is little-endian on x86_64; disc/card/GX data is big-endian) */
static inline uint16_t fzgx_bswap16(uint16_t v) {
#if defined(_MSC_VER)
    return _byteswap_ushort(v);
#else
    return __builtin_bswap16(v);
#endif
}

static inline uint32_t fzgx_bswap32(uint32_t v) {
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}

static inline uint64_t fzgx_bswap64(uint64_t v) {
#if defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}

static inline uint16_t fzgx_be16(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
}

static inline uint32_t fzgx_be32(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

/* MWCC math / PowerPC intrinsic shims */
static inline double fzgx_fabs(double x) { return fabs(x); }
static inline float fzgx_fabsf(float x) { return fabsf(x); }
static inline double fzgx_frsqrte(double x) { return (x > 0.0) ? (1.0 / sqrt(x)) : 0.0; }
static inline float fzgx_fres(float x) { return (x != 0.0f) ? (1.0f / x) : 0.0f; }

static inline uint32_t fzgx_rotl32(uint32_t val, uint32_t shift) {
    shift &= 31u;
    return (shift == 0u) ? val : ((val << shift) | (val >> (32u - shift)));
}

static inline uint32_t fzgx_rlwinm(uint32_t rs, uint32_t sh, uint32_t mb, uint32_t me) {
    uint32_t rot = fzgx_rotl32(rs, sh);
    uint32_t mask_b = 0xFFFFFFFFu >> mb;
    uint32_t mask_e = 0xFFFFFFFFu << (31u - me);
    uint32_t mask = (mb <= me) ? (mask_b & mask_e) : (mask_b | mask_e);
    return rot & mask;
}

/* Paired-single 3D vector helper shims matching external/fzero-gx-decomp/include/psvec.h */
static inline void fzgx_psvec_sub(const float* a, const float* b, float* ab) {
    ab[0] = a[0] - b[0];
    ab[1] = a[1] - b[1];
    ab[2] = a[2] - b[2];
}

static inline void fzgx_psvec_add(const float* a, const float* b, float* ab) {
    ab[0] = a[0] + b[0];
    ab[1] = a[1] + b[1];
    ab[2] = a[2] + b[2];
}

static inline void fzgx_psvec_scale(const float* a, float s, float* dst) {
    dst[0] = a[0] * s;
    dst[1] = a[1] * s;
    dst[2] = a[2] * s;
}

/* GX FIFO write routing hooks (wired to fzgx_gx_fifo_* in platform_api.h) */
void fzgx_gx_fifo_u8(uint8_t v);
void fzgx_gx_fifo_u16(uint16_t v);
void fzgx_gx_fifo_u32(uint32_t v);
void fzgx_gx_fifo_f32(float v);

#ifdef __cplusplus
}
#endif

#endif /* FZGX_PORT_MWCC_SHIM_H */
