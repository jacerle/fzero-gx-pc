/*
 * Native Dolphin SDK CARD library over the host's Dolphin-compatible GCI folder memory cards
 * (fzgx::GciCardManager, reached through platform_api.h).
 *
 * Model: a card is inserted in a channel while the host has a GCI folder mounted for that slot
 * (and it is the same folder, by serial, as at CARDMount). Each card reports the geometry of a
 * 128 Mbit "Memory Card 2043": 8 KiB sectors, 2048 blocks of which 5 are system blocks. At
 * mount the host files become the 127-entry directory (current game's files first, then other
 * games' while they fit); fileNo is the index into that directory and stays stable across
 * create/delete until unmount. Entries keep the raw big-endian CARDDir bytes of the .gci header,
 * so every field round-trips; directory changes are written through to the .gci file.
 *
 * Commands do their host I/O immediately. Retail delivers completions from interrupts, so async
 * commands return CARD_RESULT_READY, leave the channel's result at CARD_RESULT_BUSY and deliver
 * callback(chan, result) from the next fzgx_sdk_pump(). Errors retail detects before starting
 * any I/O are returned directly, without a callback, as retail does.
 */

#include "sdk/sdk_internal.h"

#include "fzgx_port/platform_api.h"

#include <string.h>

#define CARD_NUM_CHANS 2
#define CARD_MEM_SIZE_MBITS 128
#define CARD_SECTOR_SIZE (8 * 1024)
#define CARD_NUM_BLOCKS ((u32)CARD_MEM_SIZE_MBITS * 1024u * 1024u / 8u / (u32)CARD_SECTOR_SIZE)
#define CARD_NUM_SYSTEM_BLOCKS 5u
#define CARD_USER_BLOCKS (CARD_NUM_BLOCKS - CARD_NUM_SYSTEM_BLOCKS)

/* CARDDir field offsets in the 64-byte big-endian directory entry (.gci header). */
#define DENTRY_GAME_NAME 0x00
#define DENTRY_COMPANY 0x04
#define DENTRY_BANNER_FORMAT 0x07
#define DENTRY_FILE_NAME 0x08
#define DENTRY_TIME 0x28
#define DENTRY_ICON_ADDR 0x2C
#define DENTRY_ICON_FORMAT 0x30
#define DENTRY_ICON_SPEED 0x32
#define DENTRY_PERMISSION 0x34
#define DENTRY_COPY_TIMES 0x35
#define DENTRY_START_BLOCK 0x36
#define DENTRY_LENGTH 0x38
#define DENTRY_COMMENT_ADDR 0x3C

#define CARD_ATTR_PUBLIC 0x04u
#define CARD_ADDR_NONE 0xFFFFFFFFu
#define CARD_COMMENT_SIZE 64u

typedef struct CardEntry {
    BOOL used;
    u8 dentry[FZGX_CARD_DENTRY_SIZE];
} CardEntry;

typedef struct CardControl {
    BOOL attached;
    s32 result;
    u64 serial;                 /* host folder serial at mount; a change means the card was swapped */
    CARDCallback apiCallback;   /* completion of the command in flight */
    CARDCallback extCallback;   /* detach callback given to CARDMount */
    CARDCallback detachPending; /* detach callback queued for the next pump */
    CardEntry dir[CARD_MAX_FILE];
} CardControl;

static CardControl s_cards[CARD_NUM_CHANS];
static BOOL s_initialized;

/* ---------------------------------------------------------------- directory entry helpers */

static u16 get_be16(const u8 *p) {
    return (u16)(((u32)p[0] << 8) | p[1]);
}

static u32 get_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void set_be16(u8 *p, u32 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static void set_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/* Seconds since 2000-01-01 local time, as retail OSGetTime() / OS_TIMER_CLOCK. */
static u32 card_time_now(void) {
    return (u32)(fzgx_os_time_ticks() / (int64_t)OS_TIMER_CLOCK);
}

static void card_set_file_name(u8 *dentry, const char *fileName) {
    memset(dentry + DENTRY_FILE_NAME, 0, CARD_FILENAME_MAX);
    memcpy(dentry + DENTRY_FILE_NAME, fileName, strlen(fileName));
}

/* Current game identity; NULL without a disc ID, which gives access to every file like
 * retail's __CARDDiskNone. */
static const DVDDiskID *card_disk_id(void) {
    const DVDDiskID *id = DVDGetCurrentDiskID();
    return (id != NULL && id->gameName[0] != '\0') ? id : NULL;
}

static BOOL card_is_current_game(const CardEntry *ent) {
    static const char none[6];
    const DVDDiskID *id = card_disk_id();
    const char *gameName = id ? id->gameName : none;
    const char *company = id ? id->company : none + 4;
    return memcmp(ent->dentry + DENTRY_GAME_NAME, gameName, 4) == 0 &&
           memcmp(ent->dentry + DENTRY_COMPANY, company, 2) == 0;
}

/* Retail __CARDAccess. */
static s32 card_access(const CardEntry *ent) {
    if (!ent->used) {
        return CARD_RESULT_NOFILE;
    }
    if (card_disk_id() == NULL || card_is_current_game(ent)) {
        return CARD_RESULT_READY;
    }
    return CARD_RESULT_NOPERM;
}

/* Retail __CARDIsPublic. */
static s32 card_is_public(const CardEntry *ent) {
    if (!ent->used) {
        return CARD_RESULT_NOFILE;
    }
    return (ent->dentry[DENTRY_PERMISSION] & CARD_ATTR_PUBLIC) ? CARD_RESULT_READY : CARD_RESULT_NOPERM;
}

/* Retail __CARDCompareFileName. */
static BOOL card_compare_file_name(const CardEntry *ent, const char *fileName) {
    const char *entName = (const char *)ent->dentry + DENTRY_FILE_NAME;
    int n = CARD_FILENAME_MAX;
    while (--n >= 0) {
        const char c1 = *entName++;
        const char c2 = *fileName++;
        if (c1 != c2) {
            return FALSE;
        }
        if (c2 == '\0') {
            return TRUE;
        }
    }
    return *fileName == '\0';
}

static u32 card_free_blocks(const CardControl *card) {
    u32 used = 0;
    int fileNo;
    for (fileNo = 0; fileNo < CARD_MAX_FILE; fileNo++) {
        if (card->dir[fileNo].used) {
            used += get_be16(card->dir[fileNo].dentry + DENTRY_LENGTH);
        }
    }
    return used < CARD_USER_BLOCKS ? CARD_USER_BLOCKS - used : 0;
}

/* Retail UpdateIconOffsets: banner/icon/comment image offsets inside the file's data. */
static void card_update_icon_offsets(const u8 *dentry, CARDStat *stat) {
    u32 offset = get_be32(dentry + DENTRY_ICON_ADDR);
    const u16 iconFormat = get_be16(dentry + DENTRY_ICON_FORMAT);
    BOOL iconTlut = FALSE;
    int i;
    if (offset == CARD_ADDR_NONE) {
        stat->bannerFormat = 0;
        stat->iconFormat = 0;
        stat->iconSpeed = 0;
        offset = 0;
    }
    switch (dentry[DENTRY_BANNER_FORMAT] & 3) {
    case 1: /* C8 */
        stat->offsetBanner = offset;
        offset += 96 * 32;
        stat->offsetBannerTlut = offset;
        offset += 2 * 256;
        break;
    case 2: /* RGB5A3 */
        stat->offsetBanner = offset;
        offset += 2 * 96 * 32;
        stat->offsetBannerTlut = CARD_ADDR_NONE;
        break;
    default:
        stat->offsetBanner = CARD_ADDR_NONE;
        stat->offsetBannerTlut = CARD_ADDR_NONE;
        break;
    }
    for (i = 0; i < 8; ++i) {
        switch ((iconFormat >> (2 * i)) & 3) {
        case 1: /* C8 */
            stat->offsetIcon[i] = offset;
            offset += 32 * 32;
            iconTlut = TRUE;
            break;
        case 2: /* RGB5A3 */
            stat->offsetIcon[i] = offset;
            offset += 2 * 32 * 32;
            break;
        default:
            stat->offsetIcon[i] = CARD_ADDR_NONE;
            break;
        }
    }
    if (iconTlut) {
        stat->offsetIconTlut = offset;
        offset += 2 * 256;
    } else {
        stat->offsetIconTlut = CARD_ADDR_NONE;
    }
    stat->offsetData = offset;
}

/* Writes `updated` as the entry's directory entry (also renames the host file on a name change). */
static s32 card_update_dentry(s32 chan, CardEntry *ent, const u8 *updated) {
    if (!fzgx_card_set_dentry(chan, ent->dentry, updated)) {
        return CARD_RESULT_IOERROR;
    }
    memcpy(ent->dentry, updated, FZGX_CARD_DENTRY_SIZE);
    return CARD_RESULT_READY;
}

/* Builds the directory from the host files; fileNo i gets start block 5 + i (the GCI folder has
 * no FAT, so start blocks only need to be valid and distinct). */
static void card_load_dir(CardControl *card, s32 chan) {
    const s32 count = fzgx_card_file_count(chan);
    u32 usedBlocks = 0;
    s32 fileNo = 0;
    int pass;
    memset(card->dir, 0, sizeof(card->dir));
    /* Like Dolphin's GCI folder: the current game's files first, then other games' while they fit. */
    for (pass = 0; pass < 2; pass++) {
        s32 i;
        for (i = 0; i < count && fileNo < CARD_MAX_FILE; i++) {
            CardEntry *ent = &card->dir[fileNo];
            u32 length;
            if (!fzgx_card_file_dentry(chan, i, ent->dentry) || card_is_current_game(ent) != (pass == 0)) {
                continue;
            }
            length = get_be16(ent->dentry + DENTRY_LENGTH);
            if (length == 0 || usedBlocks + length > CARD_USER_BLOCKS) {
                continue;
            }
            usedBlocks += length;
            set_be16(ent->dentry + DENTRY_START_BLOCK, CARD_NUM_SYSTEM_BLOCKS + (u32)fileNo);
            ent->used = TRUE;
            fileNo++;
        }
    }
    if (fileNo < CARD_MAX_FILE) {
        memset(&card->dir[fileNo], 0, sizeof(card->dir[fileNo])); /* scratch of a skipped file */
    }
}

/* ---------------------------------------------------------------- control block */

static void card_do_unmount(CardControl *card, s32 result) {
    if (card->attached) {
        card->attached = FALSE;
        card->result = result;
        memset(card->dir, 0, sizeof(card->dir));
    }
}

static void card_deliver_detach(void *ctx, s32 chan, s32 result) {
    CardControl *card = ctx;
    const CARDCallback callback = card->detachPending;
    card->detachPending = NULL;
    if (callback) {
        callback(chan, result);
    }
}

/* Host-side removal (slot folder unmounted or replaced): retail __CARDExtHandler. */
static void card_check_presence(CardControl *card, s32 chan) {
    if (!card->attached || (fzgx_card_slot_mounted(chan) && fzgx_card_serial(chan) == card->serial)) {
        return;
    }
    card_do_unmount(card, CARD_RESULT_NOCARD);
    if (card->extCallback) {
        card->detachPending = card->extCallback;
        fzgx_sdk_defer(card_deliver_detach, card, chan, CARD_RESULT_NOCARD);
    }
}

/* Retail __CARDGetControlBlock: claims the channel (result BUSY) for one command. */
static s32 card_get_control_block(s32 chan, CardControl **pcard) {
    CardControl *card;
    if (chan < 0 || chan >= CARD_NUM_CHANS || !s_initialized) {
        return CARD_RESULT_FATAL_ERROR;
    }
    fzgx_sdk_pump();
    card = &s_cards[chan];
    card_check_presence(card, chan);
    if (!card->attached) {
        return CARD_RESULT_NOCARD;
    }
    if (card->result == CARD_RESULT_BUSY) {
        return CARD_RESULT_BUSY;
    }
    card->result = CARD_RESULT_BUSY;
    card->apiCallback = NULL;
    *pcard = card;
    return CARD_RESULT_READY;
}

/* Retail __CARDPutControlBlock: releases the channel with `result`. */
static s32 card_put_control_block(CardControl *card, s32 result) {
    if (card->attached || card->result == CARD_RESULT_BUSY) {
        card->result = result;
    }
    return result;
}

static void card_complete(void *ctx, s32 chan, s32 result) {
    CardControl *card = ctx;
    const CARDCallback callback = card->apiCallback;
    card->apiCallback = NULL;
    if (!card->attached) {
        result = CARD_RESULT_NOCARD;
    }
    card_put_control_block(card, result);
    if (callback) {
        callback(chan, result);
    }
}

/* Finishes a command holding the control block: sync commands release it and return `result`;
 * async ones return READY and release it from the next pump, then call `callback`. */
static s32 card_end(CardControl *card, s32 chan, s32 result, BOOL async, CARDCallback callback) {
    if (!async) {
        return card_put_control_block(card, result);
    }
    card->apiCallback = callback;
    fzgx_sdk_defer(card_complete, card, chan, result);
    return CARD_RESULT_READY;
}

/* ---------------------------------------------------------------- init / mount */

void CARDInit(void) {
    int chan;
    if (s_initialized) {
        return;
    }
    for (chan = 0; chan < CARD_NUM_CHANS; chan++) {
        memset(&s_cards[chan], 0, sizeof(s_cards[chan]));
        s_cards[chan].result = CARD_RESULT_NOCARD;
    }
    s_initialized = TRUE;
}

s32 CARDProbeEx(s32 chan, s32 *memSize, s32 *sectorSize) {
    if (chan < 0 || chan >= CARD_NUM_CHANS) {
        return CARD_RESULT_FATAL_ERROR;
    }
    if (!fzgx_card_slot_mounted(chan)) {
        return CARD_RESULT_NOCARD;
    }
    if (memSize) {
        *memSize = CARD_MEM_SIZE_MBITS;
    }
    if (sectorSize) {
        *sectorSize = CARD_SECTOR_SIZE;
    }
    return CARD_RESULT_READY;
}

static s32 card_mount(s32 chan, CARDCallback detachCallback, BOOL async, CARDCallback attachCallback) {
    CardControl *card;
    if (chan < 0 || chan >= CARD_NUM_CHANS || !s_initialized) {
        return CARD_RESULT_FATAL_ERROR;
    }
    fzgx_sdk_pump();
    card = &s_cards[chan];
    if (card->result == CARD_RESULT_BUSY) {
        return CARD_RESULT_BUSY;
    }
    card_check_presence(card, chan);
    if (!fzgx_card_slot_mounted(chan)) {
        card->result = CARD_RESULT_NOCARD;
        return CARD_RESULT_NOCARD;
    }
    card->result = CARD_RESULT_BUSY;
    card->extCallback = detachCallback;
    card->attached = TRUE;
    card->serial = fzgx_card_serial(chan);
    card_load_dir(card, chan);
    return card_end(card, chan, CARD_RESULT_READY, async, attachCallback);
}

s32 CARDMountAsync(s32 chan, void *workArea, CARDCallback detachCallback, CARDCallback attachCallback) {
    (void)workArea;
    return card_mount(chan, detachCallback, TRUE, attachCallback);
}

s32 CARDMount(s32 chan, void *workArea, CARDCallback detachCallback) {
    (void)workArea;
    return card_mount(chan, detachCallback, FALSE, NULL);
}

s32 CARDUnmount(s32 chan) {
    CardControl *card;
    const s32 result = card_get_control_block(chan, &card);
    if (result < 0) {
        return result;
    }
    card_do_unmount(card, CARD_RESULT_NOCARD);
    return CARD_RESULT_READY;
}

/* The GCI folder has no system area to verify or repair. */
static s32 card_check(s32 chan, s32 *xferBytes, BOOL async, CARDCallback callback) {
    CardControl *card;
    s32 result;
    if (xferBytes) {
        *xferBytes = 0;
    }
    result = card_get_control_block(chan, &card);
    if (result < 0) {
        return result;
    }
    return card_end(card, chan, CARD_RESULT_READY, async, callback);
}

s32 CARDCheckExAsync(s32 chan, s32 *xferBytes, CARDCallback callback) {
    return card_check(chan, xferBytes, TRUE, callback);
}

s32 CARDCheckAsync(s32 chan, CARDCallback callback) {
    return card_check(chan, NULL, TRUE, callback);
}

s32 CARDCheck(s32 channel) {
    return card_check(channel, NULL, FALSE, NULL);
}

static s32 card_format(s32 chan, BOOL async, CARDCallback callback) {
    CardControl *card;
    s32 result = card_get_control_block(chan, &card);
    int fileNo;
    if (result < 0) {
        return result;
    }
    for (fileNo = 0; fileNo < CARD_MAX_FILE; fileNo++) {
        CardEntry *ent = &card->dir[fileNo];
        if (!ent->used) {
            continue;
        }
        if (fzgx_card_delete_file(chan, ent->dentry)) {
            memset(ent, 0, sizeof(*ent));
        } else {
            result = CARD_RESULT_IOERROR;
        }
    }
    return card_end(card, chan, result, async, callback);
}

s32 CARDFormatAsync(s32 chan, CARDCallback callback) {
    return card_format(chan, TRUE, callback);
}

s32 CARDFormat(s32 chan) {
    return card_format(chan, FALSE, NULL);
}

/* ---------------------------------------------------------------- status queries */

s32 CARDGetResultCode(s32 chan) {
    if (chan < 0 || chan >= CARD_NUM_CHANS) {
        return CARD_RESULT_FATAL_ERROR;
    }
    fzgx_sdk_pump();
    card_check_presence(&s_cards[chan], chan);
    return s_cards[chan].result;
}

s32 CARDFreeBlocks(s32 chan, s32 *byteNotUsed, s32 *filesNotUsed) {
    CardControl *card;
    const s32 result = card_get_control_block(chan, &card);
    if (result < 0) {
        return result;
    }
    if (byteNotUsed) {
        *byteNotUsed = (s32)(card_free_blocks(card) * CARD_SECTOR_SIZE);
    }
    if (filesNotUsed) {
        int fileNo;
        *filesNotUsed = 0;
        for (fileNo = 0; fileNo < CARD_MAX_FILE; fileNo++) {
            if (!card->dir[fileNo].used) {
                ++*filesNotUsed;
            }
        }
    }
    return card_put_control_block(card, CARD_RESULT_READY);
}

s32 CARDGetSerialNo(s32 chan, u64 *serialNo) {
    CardControl *card;
    s32 result;
    if (chan < 0 || chan >= CARD_NUM_CHANS) {
        return CARD_RESULT_FATAL_ERROR;
    }
    result = card_get_control_block(chan, &card);
    if (result < 0) {
        return result;
    }
    *serialNo = card->serial;
    return card_put_control_block(card, CARD_RESULT_READY);
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat *stat) {
    CardControl *card;
    const CardEntry *ent;
    s32 result;
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
        return CARD_RESULT_FATAL_ERROR;
    }
    result = card_get_control_block(chan, &card);
    if (result < 0) {
        return result;
    }
    ent = &card->dir[fileNo];
    result = card_access(ent);
    if (result == CARD_RESULT_NOPERM) {
        result = card_is_public(ent);
    }
    if (result >= 0) {
        memcpy(stat->gameName, ent->dentry + DENTRY_GAME_NAME, sizeof(stat->gameName));
        memcpy(stat->company, ent->dentry + DENTRY_COMPANY, sizeof(stat->company));
        stat->length = (u32)get_be16(ent->dentry + DENTRY_LENGTH) * CARD_SECTOR_SIZE;
        memcpy(stat->fileName, ent->dentry + DENTRY_FILE_NAME, CARD_FILENAME_MAX);
        stat->time = get_be32(ent->dentry + DENTRY_TIME);
        stat->bannerFormat = ent->dentry[DENTRY_BANNER_FORMAT];
        stat->iconAddr = get_be32(ent->dentry + DENTRY_ICON_ADDR);
        stat->iconFormat = get_be16(ent->dentry + DENTRY_ICON_FORMAT);
        stat->iconSpeed = get_be16(ent->dentry + DENTRY_ICON_SPEED);
        stat->commentAddr = get_be32(ent->dentry + DENTRY_COMMENT_ADDR);
        card_update_icon_offsets(ent->dentry, stat);
    }
    return card_put_control_block(card, result);
}

static s32 card_set_status(s32 chan, s32 fileNo, CARDStat *stat, BOOL async, CARDCallback callback) {
    CardControl *card;
    CardEntry *ent;
    u8 updated[FZGX_CARD_DENTRY_SIZE];
    s32 result;
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE ||
        (stat->iconAddr != CARD_ADDR_NONE && stat->iconAddr >= CARD_READ_SIZE) ||
        (stat->commentAddr != CARD_ADDR_NONE &&
         CARD_SECTOR_SIZE - CARD_COMMENT_SIZE < stat->commentAddr % CARD_SECTOR_SIZE)) {
        return CARD_RESULT_FATAL_ERROR;
    }
    result = card_get_control_block(chan, &card);
    if (result < 0) {
        return result;
    }
    ent = &card->dir[fileNo];
    result = card_access(ent);
    if (result < 0) {
        return card_put_control_block(card, result);
    }
    memcpy(updated, ent->dentry, sizeof(updated));
    updated[DENTRY_BANNER_FORMAT] = stat->bannerFormat;
    set_be32(updated + DENTRY_ICON_ADDR, stat->iconAddr);
    set_be16(updated + DENTRY_ICON_FORMAT, stat->iconFormat);
    set_be16(updated + DENTRY_ICON_SPEED, stat->iconSpeed);
    set_be32(updated + DENTRY_COMMENT_ADDR, stat->commentAddr);
    card_update_icon_offsets(updated, stat);
    if (stat->iconAddr == CARD_ADDR_NONE) {
        /* Icon 0 speed = CARD_STAT_SPEED_FAST, as retail. */
        set_be16(updated + DENTRY_ICON_SPEED, (get_be16(updated + DENTRY_ICON_SPEED) & ~3u) | 1u);
    }
    set_be32(updated + DENTRY_TIME, card_time_now());
    result = card_update_dentry(chan, ent, updated);
    return card_end(card, chan, result, async, callback);
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat *stat, CARDCallback callback) {
    return card_set_status(chan, fileNo, stat, TRUE, callback);
}

s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat *stat) {
    return card_set_status(chan, fileNo, stat, FALSE, NULL);
}

/* ---------------------------------------------------------------- open / close / create */

/* Retail __CARDGetFileNo: the current game's file named fileName. */
static s32 card_get_file_no(const CardControl *card, const char *fileName, s32 *pfileNo) {
    s32 fileNo;
    for (fileNo = 0; fileNo < CARD_MAX_FILE; fileNo++) {
        const CardEntry *ent = &card->dir[fileNo];
        if (card_access(ent) >= 0 && card_compare_file_name(ent, fileName)) {
            *pfileNo = fileNo;
            return CARD_RESULT_READY;
        }
    }
    return CARD_RESULT_NOFILE;
}

static s32 card_open_entry(CARDFileInfo *fileInfo, s32 chan, s32 fileNo, const CardEntry *ent) {
    const u16 startBlock = get_be16(ent->dentry + DENTRY_START_BLOCK);
    if (startBlock < CARD_NUM_SYSTEM_BLOCKS || startBlock >= CARD_NUM_BLOCKS) {
        return CARD_RESULT_BROKEN;
    }
    fileInfo->chan = chan;
    fileInfo->fileNo = fileNo;
    fileInfo->offset = 0;
    fileInfo->iBlock = startBlock;
    return CARD_RESULT_READY;
}

s32 CARDOpen(s32 chan, const char *fileName, CARDFileInfo *fileInfo) {
    CardControl *card;
    s32 fileNo;
    s32 result;
    fileInfo->chan = -1;
    result = card_get_control_block(chan, &card);
    if (result < 0) {
        return result;
    }
    result = card_get_file_no(card, fileName, &fileNo);
    if (result >= 0) {
        result = card_open_entry(fileInfo, chan, fileNo, &card->dir[fileNo]);
    }
    return card_put_control_block(card, result);
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo *fileInfo) {
    CardControl *card;
    const CardEntry *ent;
    s32 result;
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
        return CARD_RESULT_FATAL_ERROR;
    }
    fileInfo->chan = -1;
    result = card_get_control_block(chan, &card);
    if (result < 0) {
        return result;
    }
    ent = &card->dir[fileNo];
    result = card_access(ent);
    if (result == CARD_RESULT_NOPERM) {
        result = card_is_public(ent);
    }
    if (result >= 0) {
        result = card_open_entry(fileInfo, chan, fileNo, ent);
    }
    return card_put_control_block(card, result);
}

s32 CARDClose(CARDFileInfo *fileInfo) {
    CardControl *card;
    const s32 result = card_get_control_block(fileInfo->chan, &card);
    if (result < 0) {
        return result;
    }
    fileInfo->chan = -1;
    return card_put_control_block(card, CARD_RESULT_READY);
}

static s32 card_create(s32 chan, const char *fileName, u32 size, CARDFileInfo *fileInfo, BOOL async,
                       CARDCallback callback) {
    CardControl *card;
    CardEntry *ent;
    const DVDDiskID *id;
    u8 dentry[FZGX_CARD_DENTRY_SIZE];
    s32 result;
    s32 fileNo;
    s32 freeNo = -1;
    if (strlen(fileName) > CARD_FILENAME_MAX) {
        return CARD_RESULT_NAMETOOLONG;
    }
    result = card_get_control_block(chan, &card);
    if (result < 0) {
        return result;
    }
    if (size == 0 || size % CARD_SECTOR_SIZE != 0) {
        return card_put_control_block(card, CARD_RESULT_FATAL_ERROR);
    }
    for (fileNo = 0; fileNo < CARD_MAX_FILE; fileNo++) {
        ent = &card->dir[fileNo];
        if (!ent->used) {
            if (freeNo == -1) {
                freeNo = fileNo;
            }
        } else if (card_is_current_game(ent) && card_compare_file_name(ent, fileName)) {
            return card_put_control_block(card, CARD_RESULT_EXIST);
        }
    }
    if (freeNo == -1) {
        return card_put_control_block(card, CARD_RESULT_NOENT);
    }
    if (card_free_blocks(card) * CARD_SECTOR_SIZE < size) {
        return card_put_control_block(card, CARD_RESULT_INSSPACE);
    }

    /* Retail CARDCreateAsync + CreateCallbackFat on an erased (0xFF) directory entry. */
    id = card_disk_id();
    memset(dentry, 0xFF, sizeof(dentry));
    if (id) {
        memcpy(dentry + DENTRY_GAME_NAME, id->gameName, 4);
        memcpy(dentry + DENTRY_COMPANY, id->company, 2);
    } else {
        memset(dentry + DENTRY_GAME_NAME, 0, 6);
    }
    card_set_file_name(dentry, fileName);
    dentry[DENTRY_PERMISSION] = CARD_ATTR_PUBLIC;
    dentry[DENTRY_COPY_TIMES] = 0;
    set_be16(dentry + DENTRY_START_BLOCK, CARD_NUM_SYSTEM_BLOCKS + (u32)freeNo);
    set_be16(dentry + DENTRY_LENGTH, size / CARD_SECTOR_SIZE);
    dentry[DENTRY_BANNER_FORMAT] = 0;
    set_be32(dentry + DENTRY_ICON_ADDR, CARD_ADDR_NONE);
    set_be16(dentry + DENTRY_ICON_FORMAT, 0);
    set_be16(dentry + DENTRY_ICON_SPEED, 1); /* icon 0: CARD_STAT_SPEED_FAST */
    set_be32(dentry + DENTRY_COMMENT_ADDR, CARD_ADDR_NONE);
    set_be32(dentry + DENTRY_TIME, card_time_now());

    fileInfo->chan = chan;
    fileInfo->fileNo = freeNo;
    if (fzgx_card_create_file(chan, dentry)) {
        ent = &card->dir[freeNo];
        memcpy(ent->dentry, dentry, sizeof(dentry));
        ent->used = TRUE;
        fileInfo->offset = 0;
        fileInfo->iBlock = get_be16(dentry + DENTRY_START_BLOCK);
        result = CARD_RESULT_READY;
    } else {
        result = CARD_RESULT_IOERROR;
    }
    return card_end(card, chan, result, async, callback);
}

s32 CARDCreateAsync(s32 chan, const char *fileName, u32 size, CARDFileInfo *fileInfo, CARDCallback callback) {
    return card_create(chan, fileName, size, fileInfo, TRUE, callback);
}

s32 CARDCreate(s32 channel, const char *fileName, u32 size, CARDFileInfo *fileInfo) {
    return card_create(channel, fileName, size, fileInfo, FALSE, NULL);
}

/* ---------------------------------------------------------------- read / write */

/* Retail __CARDSeek: validates the open file and the [offset, offset + length) range. */
static s32 card_seek(CARDFileInfo *fileInfo, s32 length, s32 offset, CardControl **pcard) {
    CardControl *card;
    const CardEntry *ent;
    u32 fileSize;
    s32 result = card_get_control_block(fileInfo->chan, &card);
    if (result < 0) {
        return result;
    }
    if (fileInfo->fileNo < 0 || fileInfo->fileNo >= CARD_MAX_FILE || fileInfo->iBlock < CARD_NUM_SYSTEM_BLOCKS ||
        fileInfo->iBlock >= CARD_NUM_BLOCKS || fileInfo->offset < 0 || offset < 0 || length < 0) {
        return card_put_control_block(card, CARD_RESULT_FATAL_ERROR);
    }
    ent = &card->dir[fileInfo->fileNo];
    if (!ent->used) {
        return card_put_control_block(card, CARD_RESULT_NOFILE);
    }
    fileSize = (u32)get_be16(ent->dentry + DENTRY_LENGTH) * CARD_SECTOR_SIZE;
    if ((u32)offset >= fileSize || fileSize < (u32)offset + (u32)length) {
        return card_put_control_block(card, CARD_RESULT_LIMIT);
    }
    fileInfo->length = length;
    fileInfo->offset = offset;
    *pcard = card;
    return CARD_RESULT_READY;
}

static s32 card_read(CARDFileInfo *fileInfo, void *buf, s32 length, s32 offset, BOOL async,
                     CARDCallback callback) {
    CardControl *card;
    const CardEntry *ent;
    s32 result;
    if (((u32)offset & (CARD_READ_SIZE - 1)) != 0 || ((u32)length & (CARD_READ_SIZE - 1)) != 0) {
        return CARD_RESULT_FATAL_ERROR;
    }
    result = card_seek(fileInfo, length, offset, &card);
    if (result < 0) {
        return result;
    }
    ent = &card->dir[fileInfo->fileNo];
    result = card_access(ent);
    if (result == CARD_RESULT_NOPERM) {
        result = card_is_public(ent);
    }
    if (result < 0) {
        return card_put_control_block(card, result);
    }
    result = fzgx_card_read_data(fileInfo->chan, ent->dentry, (u32)offset, buf, (u32)length) ? CARD_RESULT_READY
                                                                                            : CARD_RESULT_IOERROR;
    return card_end(card, fileInfo->chan, result, async, callback);
}

s32 CARDReadAsync(CARDFileInfo *fileInfo, void *buf, s32 length, s32 offset, CARDCallback callback) {
    return card_read(fileInfo, buf, length, offset, TRUE, callback);
}

s32 CARDRead(CARDFileInfo *fileInfo, void *buf, s32 length, s32 offset) {
    return card_read(fileInfo, buf, length, offset, FALSE, NULL);
}

static s32 card_write(CARDFileInfo *fileInfo, const void *buf, s32 length, s32 offset, BOOL async,
                      CARDCallback callback) {
    CardControl *card;
    CardEntry *ent;
    u8 updated[FZGX_CARD_DENTRY_SIZE];
    s32 result = card_seek(fileInfo, length, offset, &card);
    if (result < 0) {
        return result;
    }
    /* Retail writes whole sectors (erase + program), so writes are sector-aligned. */
    if (((u32)offset & (CARD_SECTOR_SIZE - 1)) != 0 || ((u32)length & (CARD_SECTOR_SIZE - 1)) != 0) {
        return card_put_control_block(card, CARD_RESULT_FATAL_ERROR);
    }
    ent = &card->dir[fileInfo->fileNo];
    result = card_access(ent);
    if (result < 0) {
        return card_put_control_block(card, result);
    }
    if (fzgx_card_write_data(fileInfo->chan, ent->dentry, (u32)offset, buf, (u32)length)) {
        memcpy(updated, ent->dentry, sizeof(updated));
        set_be32(updated + DENTRY_TIME, card_time_now());
        result = card_update_dentry(fileInfo->chan, ent, updated);
    } else {
        result = CARD_RESULT_IOERROR;
    }
    return card_end(card, fileInfo->chan, result, async, callback);
}

s32 CARDWriteAsync(CARDFileInfo *fileInfo, const void *buf, s32 length, s32 offset, CARDCallback callback) {
    return card_write(fileInfo, buf, length, offset, TRUE, callback);
}

s32 CARDWrite(CARDFileInfo *fileInfo, const void *buf, s32 length, s32 offset) {
    return card_write(fileInfo, buf, length, offset, FALSE, NULL);
}

/* ---------------------------------------------------------------- delete / rename */

static s32 card_delete_entry(CardControl *card, s32 chan, s32 fileNo, BOOL async, CARDCallback callback) {
    CardEntry *ent = &card->dir[fileNo];
    s32 result = CARD_RESULT_IOERROR;
    if (fzgx_card_delete_file(chan, ent->dentry)) {
        memset(ent, 0, sizeof(*ent));
        result = CARD_RESULT_READY;
    }
    return card_end(card, chan, result, async, callback);
}

static s32 card_fast_delete(s32 chan, s32 fileNo, BOOL async, CARDCallback callback) {
    CardControl *card;
    s32 result;
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
        return CARD_RESULT_FATAL_ERROR;
    }
    result = card_get_control_block(chan, &card);
    if (result < 0) {
        return result;
    }
    result = card_access(&card->dir[fileNo]);
    if (result < 0) {
        return card_put_control_block(card, result);
    }
    return card_delete_entry(card, chan, fileNo, async, callback);
}

s32 CARDFastDeleteAsync(s32 chan, s32 fileNo, CARDCallback callback) {
    return card_fast_delete(chan, fileNo, TRUE, callback);
}

s32 CARDFastDelete(s32 chan, s32 fileNo) {
    return card_fast_delete(chan, fileNo, FALSE, NULL);
}

static s32 card_delete(s32 chan, const char *fileName, BOOL async, CARDCallback callback) {
    CardControl *card;
    s32 fileNo;
    s32 result = card_get_control_block(chan, &card);
    if (result < 0) {
        return result;
    }
    result = card_get_file_no(card, fileName, &fileNo);
    if (result < 0) {
        return card_put_control_block(card, result);
    }
    return card_delete_entry(card, chan, fileNo, async, callback);
}

s32 CARDDeleteAsync(s32 chan, const char *fileName, CARDCallback callback) {
    return card_delete(chan, fileName, TRUE, callback);
}

s32 CARDDelete(s32 chan, const char *fileName) {
    return card_delete(chan, fileName, FALSE, NULL);
}

static s32 card_rename(s32 chan, const char *oldName, const char *newName, BOOL async, CARDCallback callback) {
    CardControl *card;
    CardEntry *ent;
    u8 updated[FZGX_CARD_DENTRY_SIZE];
    s32 result;
    s32 fileNo;
    s32 oldNo = -1;
    s32 newNo = -1;
    if ((u8)*oldName == 0xFF || (u8)*newName == 0xFF || *oldName == '\0' || *newName == '\0') {
        return CARD_RESULT_FATAL_ERROR;
    }
    if (strlen(oldName) > CARD_FILENAME_MAX || strlen(newName) > CARD_FILENAME_MAX) {
        return CARD_RESULT_NAMETOOLONG;
    }
    result = card_get_control_block(chan, &card);
    if (result < 0) {
        return result;
    }
    for (fileNo = 0; fileNo < CARD_MAX_FILE; fileNo++) {
        ent = &card->dir[fileNo];
        if (!ent->used || !card_is_current_game(ent)) {
            continue;
        }
        if (card_compare_file_name(ent, oldName)) {
            oldNo = fileNo;
        }
        if (card_compare_file_name(ent, newName)) {
            newNo = fileNo;
        }
    }
    if (oldNo == -1) {
        return card_put_control_block(card, CARD_RESULT_NOFILE);
    }
    if (newNo != -1) {
        return card_put_control_block(card, CARD_RESULT_EXIST);
    }
    ent = &card->dir[oldNo];
    result = card_access(ent);
    if (result < 0) {
        return card_put_control_block(card, result);
    }
    memcpy(updated, ent->dentry, sizeof(updated));
    card_set_file_name(updated, newName);
    set_be32(updated + DENTRY_TIME, card_time_now());
    result = card_update_dentry(chan, ent, updated);
    return card_end(card, chan, result, async, callback);
}

s32 CARDRenameAsync(s32 chan, const char *old, const char *new, CARDCallback callback) {
    return card_rename(chan, old, new, TRUE, callback);
}

s32 CARDRename(s32 chan, const char *oldName, const char *newName) {
    return card_rename(chan, oldName, newName, FALSE, NULL);
}
