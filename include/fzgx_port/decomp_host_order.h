#ifndef FZGX_PORT_DECOMP_HOST_ORDER_H
#define FZGX_PORT_DECOMP_HOST_ORDER_H

/*
 * Disc-to-host byte order fixups for decomp TUs that read big-endian disc structures with
 * native loads. Called from downstream overlay patches (patches/decomp/) at the point where a
 * TU first takes ownership of a loaded buffer.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Converts a loaded U8 archive (.arc: 0x55AA382D header + FST node table) to host order in
 * place, as arc.c (ARCInitHandle) expects. The string table and file data are untouched.
 * Idempotent: a buffer already in host order, or not an archive, is left as is.
 */
void fzgx_arc_to_host_order(void *archive);

#ifdef __cplusplus
}
#endif

#endif /* FZGX_PORT_DECOMP_HOST_ORDER_H */
