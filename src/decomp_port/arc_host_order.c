#include "fzgx_port/decomp_host_order.h"

#define FZGX_U8_ARC_MAGIC 0x55AA382Du

void fzgx_arc_to_host_order(void *archive) {
    u32 *header = (u32 *)archive;
    u32 *nodes;
    u32 node_count;
    u32 i;

    if (header == NULL || header[0] == FZGX_U8_ARC_MAGIC || fzgx_bswap32(header[0]) != FZGX_U8_ARC_MAGIC) {
        return;
    }
    /* magic, FST (node table) offset, FST size incl. strings, file data offset */
    for (i = 0; i < 4; i++) {
        header[i] = fzgx_bswap32(header[i]);
    }
    /* Each node is three words: type << 24 | name offset, data offset / parent, size / next.
     * The root node's third word is the node count. */
    nodes = (u32 *)((u8 *)archive + header[1]);
    node_count = fzgx_bswap32(nodes[2]);
    for (i = 0; i < node_count * 3u; i++) {
        nodes[i] = fzgx_bswap32(nodes[i]);
    }
}
