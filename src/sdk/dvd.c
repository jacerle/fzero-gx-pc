/* Native DVD library: FST lookup and file reads served from the mounted disc image. */

#include "sdk/sdk_internal.h"

#include "fzgx_port/platform_api.h"

#include <string.h>

#define FZGX_DVD_PATH_MAX 256u

static char s_current_dir[FZGX_DVD_PATH_MAX]; /* absolute, no leading/trailing '/', "" = root */
static DVDDiskID s_disk_id;
static BOOL s_disk_id_loaded;

static void load_disk_id(void) {
    /* The disc header starts with the 32-byte DVDDiskID, as retail reads into low memory. */
    if (fzgx_dvd_read_abs(0, &s_disk_id, (u32)sizeof(s_disk_id))) {
        s_disk_id_loaded = TRUE;
    }
}

void DVDInit(void) {
    s_current_dir[0] = '\0';
    s_disk_id_loaded = FALSE;
    load_disk_id();
}

DVDDiskID *DVDGetCurrentDiskID(void) {
    if (!s_disk_id_loaded) {
        load_disk_id();
    }
    return &s_disk_id;
}

/* Resolves `path` against the current directory ('/' = root, '.' and '..' segments) into
 * an absolute FST path without leading '/'. */
static BOOL resolve_path(const char *path, char *out, size_t out_cap) {
    size_t len = 0;
    const char *seg;

    if (*path == '/') {
        out[0] = '\0';
    } else {
        len = strlen(s_current_dir);
        memcpy(out, s_current_dir, len + 1);
    }
    for (seg = path; *seg != '\0';) {
        const char *end = seg;
        size_t seg_len;
        while (*end != '\0' && *end != '/') {
            end++;
        }
        seg_len = (size_t)(end - seg);
        if (seg_len == 0 || (seg_len == 1 && seg[0] == '.')) {
            /* empty or "." segment */
        } else if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (len > 0 && out[len - 1] != '/') {
                len--;
            }
            if (len > 0) {
                len--; /* drop the separator too */
            }
            out[len] = '\0';
        } else {
            const size_t sep = len > 0 ? 1u : 0u;
            if (len + sep + seg_len + 1 > out_cap) {
                return FALSE;
            }
            if (sep) {
                out[len++] = '/';
            }
            memcpy(out + len, seg, seg_len);
            len += seg_len;
            out[len] = '\0';
        }
        seg = (*end == '/') ? end + 1 : end;
    }
    return TRUE;
}

s32 DVDConvertPathToEntrynum(const char *pathPtr) {
    char abs_path[FZGX_DVD_PATH_MAX];
    if (pathPtr == NULL || !resolve_path(pathPtr, abs_path, sizeof(abs_path))) {
        return -1;
    }
    return fzgx_dvd_path_to_entry(abs_path);
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo *fileInfo) {
    bool is_dir = true;
    uint32_t disc_offset = 0;
    uint32_t length = 0;
    if (entrynum < 0 || !fzgx_dvd_entry_info(entrynum, &is_dir, &disc_offset, &length) || is_dir) {
        return FALSE;
    }
    fileInfo->startAddr = disc_offset;
    fileInfo->length = length;
    fileInfo->callback = NULL;
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDOpen(const char *fileName, DVDFileInfo *fileInfo) {
    const s32 entrynum = DVDConvertPathToEntrynum(fileName);
    if (entrynum < 0) {
        OSReport("Warning: DVDOpen(): file '%s' was not found under /%s.\n", fileName, s_current_dir);
        return FALSE;
    }
    return DVDFastOpen(entrynum, fileInfo);
}

BOOL DVDClose(DVDFileInfo *fileInfo) {
    (void)fileInfo; /* nothing is held open on the host side */
    return TRUE;
}

BOOL DVDChangeDir(const char *dirName) {
    char abs_path[FZGX_DVD_PATH_MAX];
    bool is_dir = false;
    s32 entrynum;
    if (dirName == NULL || !resolve_path(dirName, abs_path, sizeof(abs_path))) {
        return FALSE;
    }
    entrynum = fzgx_dvd_path_to_entry(abs_path);
    if (entrynum < 0 || !fzgx_dvd_entry_info(entrynum, &is_dir, NULL, NULL) || !is_dir) {
        return FALSE;
    }
    memcpy(s_current_dir, abs_path, strlen(abs_path) + 1);
    return TRUE;
}

BOOL DVDGetCurrentDir(char *path, u32 maxlen) {
    /* Retail form: "/" for the root, "/dir/sub/" otherwise. */
    const size_t len = strlen(s_current_dir);
    const size_t needed = len == 0 ? 2u : len + 3u;
    if (maxlen < needed) {
        if (maxlen > 0) {
            path[0] = '\0';
        }
        return FALSE;
    }
    path[0] = '/';
    memcpy(path + 1, s_current_dir, len);
    if (len > 0) {
        path[len + 1] = '/';
        path[len + 2] = '\0';
    } else {
        path[1] = '\0';
    }
    return TRUE;
}

/* Host read for a command block; returns bytes transferred or DVD_RESULT_FATAL_ERROR. */
static s32 transfer(DVDCommandBlock *block, void *addr, s32 length, u32 disc_offset) {
    block->addr = addr;
    block->length = (u32)length;
    block->offset = disc_offset;
    block->currTransferSize = 0;
    block->transferredSize = 0;
    if (length < 0 || !fzgx_dvd_read_abs(disc_offset, addr, (u32)length)) {
        return DVD_RESULT_FATAL_ERROR;
    }
    block->transferredSize = (u32)length;
    return length;
}

static void complete_block(void *ctx, s32 result, s32 unused) {
    DVDCommandBlock *block = (DVDCommandBlock *)ctx;
    (void)unused;
    block->state = result >= 0 ? DVD_STATE_END : DVD_STATE_FATAL_ERROR;
    if (block->callback != NULL) {
        block->callback(result, block);
    }
}

BOOL DVDReadAbsAsyncPrio(DVDCommandBlock *block, void *addr, s32 length, s32 offset,
                         DVDCBCallback callback, s32 prio) {
    s32 result;
    (void)prio;
    block->callback = callback;
    block->state = DVD_STATE_BUSY;
    result = transfer(block, addr, length, (u32)offset);
    fzgx_sdk_defer(complete_block, block, result, 0);
    return TRUE;
}

/* Retail cbForReadAsync: forwards a file read's block completion to the DVDFileInfo callback.
 * The command block is DVDFileInfo's first member. */
static void file_read_done(s32 result, DVDCommandBlock *block) {
    DVDFileInfo *fileInfo = (DVDFileInfo *)block;
    if (fileInfo->callback != NULL) {
        fileInfo->callback(result, fileInfo);
    }
}

BOOL DVDReadAsyncPrio(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset, DVDCallback callback,
                      s32 prio) {
    fileInfo->callback = callback;
    return DVDReadAbsAsyncPrio(&fileInfo->cb, addr, length, (s32)(fileInfo->startAddr + (u32)offset),
                               file_read_done, prio);
}

BOOL DVDReadAsync(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset, DVDCallback callback) {
    return DVDReadAsyncPrio(fileInfo, addr, length, offset, callback, 2);
}

s32 DVDReadPrio(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset, s32 prio) {
    DVDCommandBlock *block = &fileInfo->cb;
    s32 result;
    (void)prio;
    block->callback = NULL;
    result = transfer(block, addr, length, fileInfo->startAddr + (u32)offset);
    block->state = result >= 0 ? DVD_STATE_END : DVD_STATE_FATAL_ERROR;
    /* Retail sleeps until the transfer ends; other completions fire meanwhile. */
    fzgx_sdk_pump();
    return result;
}

s32 DVDRead(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset) {
    return DVDReadPrio(fileInfo, addr, length, offset, 2);
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock *block) {
    fzgx_sdk_pump();
    return block->state;
}

s32 DVDGetFileInfoStatus(const DVDFileInfo *fileInfo) {
    return DVDGetCommandBlockStatus(&fileInfo->cb);
}

s32 DVDGetDriveStatus(void) {
    fzgx_sdk_pump();
    return DVDCheckDisk() ? DVD_STATE_END : DVD_STATE_NO_DISK;
}

u32 DVDGetTransferredSize(DVDCommandBlock *block) {
    return block->transferredSize;
}

BOOL DVDCheckDisk(void) {
    return fzgx_dvd_entry_info(0, NULL, NULL, NULL) ? TRUE : FALSE;
}
