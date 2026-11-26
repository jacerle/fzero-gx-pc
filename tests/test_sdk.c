/*
 * Native Dolphin SDK (src/sdk) and natively compiled main.dol TUs (arc.c, ClampStick.c).
 * Entered from tests/test_main.cpp with the synthetic GFZE01 ISO mounted (mod override on
 * data/track.dat) and no input devices bound.
 */

#include "fzgx_port/dolphin_sdk.h"
#include "sdk/sdk_internal.h"

#include <assert.h>
#include <string.h>

void fzgx_test_native_sdk(void);
void fzgx_test_boot_entry(void);

/* arc.c has no public header yet; layouts as in external/fzero-gx-decomp/src/dol/arc.c. */
typedef struct ARCHeader ARCHeader;
typedef struct ARCHandle {
    void *archiveStartAddr;
    void *FSTStart;
    void *fileStart;
    u32 entryNum;
    char *FSTStringStart;
    u32 FSTLength;
    u32 currDir;
} ARCHandle;
typedef struct ARCFileInfo {
    ARCHandle *handle;
    u32 startOffset;
    u32 length;
} ARCFileInfo;
u32 fn_8006A3DC(ARCHeader *src, ARCHandle *dst);                       /* ARCInitHandle */
s32 fn_8006A480(ARCHandle *handle, const char *path, ARCFileInfo *af); /* ARCOpen */
BOOL fn_8006A8CC(ARCHandle *handle, char *path, u32 maxlen);           /* ARCGetCurrentDir */
void *fn_8006A998(ARCFileInfo *af);                                    /* ARCGetStartAddrInMem */
u32 fn_8006A9AC(ARCFileInfo *af);                                      /* ARCGetLength */
s32 fn_8006A9B4(ARCHandle *handle, const char *dirName);               /* ARCChangeDir */

static void put_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static void test_os(void) {
    OSCalendarTime ct;
    /* 2024-02-29 13:45:30.250: 8825 days after 2000-01-01 */
    const OSTime leap_day = ((OSTime)8825 * 86400 + 13 * 3600 + 45 * 60 + 30) * OS_TIMER_CLOCK +
                            OSMillisecondsToTicks((OSTime)250);
    BOOL level;

    OSInit();
    assert(OSGetArenaLo() != NULL);
    assert((u8 *)OSGetArenaHi() - (u8 *)OSGetArenaLo() == (ptrdiff_t)FZGX_OS_ARENA_SIZE);
    assert((uintptr_t)OSGetArenaLo() % 32 == 0);

    OSTicksToCalendarTime(leap_day, &ct);
    assert(ct.year == 2024 && ct.mon == 1 && ct.mday == 29);
    assert(ct.hour == 13 && ct.min == 45 && ct.sec == 30 && ct.msec == 250 && ct.usec == 0);
    assert(ct.wday == 4 && ct.yday == 59);

    OSTicksToCalendarTime(-1, &ct); /* one tick before the epoch */
    assert(ct.year == 1999 && ct.mon == 11 && ct.mday == 31);
    assert(ct.hour == 23 && ct.min == 59 && ct.sec == 59 && ct.msec == 999);
    assert(ct.wday == 5 && ct.yday == 364);

    OSTicksToCalendarTime(OSGetTime(), &ct);
    assert(ct.year >= 2026);

    level = OSDisableInterrupts();
    assert(level == TRUE);
    assert(OSDisableInterrupts() == FALSE);
    OSRestoreInterrupts(level);
    assert(fzgx_sdk_interrupts_enabled());
}

static s32 s_dvd_cb_result;
static s32 s_dvd_cb_calls;

static void on_dvd_read(s32 result, DVDFileInfo *fileInfo) {
    (void)fileInfo;
    s_dvd_cb_result = result;
    s_dvd_cb_calls++;
}

static void test_dvd(void) {
    DVDFileInfo fi;
    char buf[32];
    BOOL level;

    DVDInit();
    assert(memcmp(DVDGetCurrentDiskID()->gameName, "GFZE", 4) == 0);
    assert(memcmp(DVDGetCurrentDiskID()->company, "01", 2) == 0);
    assert(DVDCheckDisk());

    /* FST: 0 root, 1 data/, 2 data/track.dat, 3 readme.txt */
    assert(DVDConvertPathToEntrynum("/") == 0);
    assert(DVDConvertPathToEntrynum("/readme.txt") == 3);
    assert(DVDConvertPathToEntrynum("data/track.dat") == 2);
    assert(DVDConvertPathToEntrynum("DATA/Track.DAT") == 2);
    assert(DVDConvertPathToEntrynum("/data/../readme.txt") == 3);
    assert(DVDConvertPathToEntrynum("missing.bin") == -1);

    assert(DVDChangeDir("data"));
    assert(DVDGetCurrentDir(buf, sizeof(buf)) && strcmp(buf, "/data/") == 0);
    assert(DVDConvertPathToEntrynum("track.dat") == 2);
    assert(DVDConvertPathToEntrynum("../readme.txt") == 3);
    assert(!DVDChangeDir("track.dat"));
    assert(DVDChangeDir("/"));
    assert(DVDGetCurrentDir(buf, sizeof(buf)) && strcmp(buf, "/") == 0);

    assert(!DVDOpen("missing.bin", &fi));
    assert(DVDOpen("readme.txt", &fi));
    assert(fi.length == 5);
    memset(buf, 0, sizeof(buf));
    assert(DVDRead(&fi, buf, 5, 0) == 5 && memcmp(buf, "GFZE0", 5) == 0);
    memset(buf, 0, sizeof(buf));
    assert(DVDReadPrio(&fi, buf, 3, 2, 2) == 3 && memcmp(buf, "ZE0", 3) == 0);
    assert(DVDClose(&fi));

    /* data/track.dat is replaced by the mod override "BIG_BLUE_MOD" (disc: "MUTE_CITY_1"). */
    assert(DVDFastOpen(2, &fi));
    assert(fi.length == 12);
    memset(buf, 0, sizeof(buf));
    assert(DVDRead(&fi, buf, 12, 0) == 12 && memcmp(buf, "BIG_BLUE_MOD", 12) == 0);
    assert(!DVDFastOpen(1, &fi)); /* directory */

    /* Async completion is deferred: not inside the call, not while interrupts are disabled. */
    assert(DVDOpen("readme.txt", &fi));
    memset(buf, 0, sizeof(buf));
    s_dvd_cb_calls = 0;
    assert(DVDReadAsync(&fi, buf, 5, 0, on_dvd_read));
    assert(s_dvd_cb_calls == 0 && fi.cb.state == DVD_STATE_BUSY);
    level = OSDisableInterrupts();
    assert(DVDGetFileInfoStatus(&fi) == DVD_STATE_BUSY && s_dvd_cb_calls == 0);
    OSRestoreInterrupts(level);
    assert(s_dvd_cb_calls == 1 && s_dvd_cb_result == 5);
    assert(DVDGetFileInfoStatus(&fi) == DVD_STATE_END);
    assert(DVDGetTransferredSize(&fi.cb) == 5 && memcmp(buf, "GFZE0", 5) == 0);

    /* A status poll is a delivery point too. */
    s_dvd_cb_calls = 0;
    assert(DVDReadAsyncPrio(&fi, buf, 3, 2, on_dvd_read, 1));
    assert(DVDGetFileInfoStatus(&fi) == DVD_STATE_END && s_dvd_cb_calls == 1 && s_dvd_cb_result == 3);
}

static u32 s_pre_count;
static u32 s_post_count;

static void on_pre_retrace(u32 count) {
    s_pre_count = count;
}

static void on_post_retrace(u32 count) {
    s_post_count = count;
}

static void test_vi(void) {
    DVDFileInfo fi;
    char buf[8];

    VIInit();
    assert(VISetPreRetraceCallback(on_pre_retrace) == NULL);
    assert(VISetPostRetraceCallback(on_post_retrace) == NULL);
    assert(VIGetTvFormat() == VI_NTSC);

    /* The retrace interrupt delivers pending completions between the pre and post callbacks. */
    assert(DVDOpen("readme.txt", &fi));
    s_dvd_cb_calls = 0;
    assert(DVDReadAsync(&fi, buf, 5, 0, on_dvd_read));
    VIWaitForRetrace();
    assert(s_dvd_cb_calls == 1);
    VIWaitForRetrace();
    assert(VIGetRetraceCount() == 2 && s_pre_count == 2 && s_post_count == 2);
    assert(VISetPreRetraceCallback(NULL) == on_pre_retrace);
    assert(VISetPostRetraceCallback(NULL) == on_post_retrace);
}

static void test_pad(void) {
    PADStatus pads[PAD_MAX_CONTROLLERS];
    u32 chan;

    assert(PADInit());
    assert(PADRead(pads) == 0); /* no input devices bound */
    for (chan = 0; chan < PAD_MAX_CONTROLLERS; chan++) {
        assert(pads[chan].err == PAD_ERR_NO_CONTROLLER && pads[chan].button == 0);
    }

    /* Retail clamp (ClampStick.c): stick octagon max 72 after a 15 dead zone, triggers 30..180. */
    memset(pads, 0, sizeof(pads));
    pads[0].stickX = 100;
    pads[0].substickY = -10;
    pads[0].triggerLeft = 200;
    pads[0].triggerRight = 20;
    pads[1].err = PAD_ERR_NO_CONTROLLER;
    pads[1].stickX = 100;
    pads[2].err = PAD_ERR_NO_CONTROLLER;
    pads[3].err = PAD_ERR_NO_CONTROLLER;
    PADClamp(pads);
    assert(pads[0].stickX == 72 && pads[0].stickY == 0);
    assert(pads[0].substickY == 0);
    assert(pads[0].triggerLeft == 150 && pads[0].triggerRight == 0);
    assert(pads[1].stickX == 100); /* error status is left alone */
}

static void test_arc(void) {
    /* Big-endian U8 archive as stored on disc:
     * 0x00 header, 0x20 nodes (root, Hello.txt, sub/, sub/inner.bin), 0x50 strings, 0x80 data. */
    static const char kStrings[] = "\0Hello.txt\0sub\0inner.bin";
    static u32 words[48]; /* u32 storage keeps the buffer word-aligned like a DVD read target */
    u8 *arc = (u8 *)words;
    ARCHandle handle;
    ARCFileInfo af;
    char path[32];

    put_be32(arc + 0x00, 0x55AA382Du);
    put_be32(arc + 0x04, 0x20);
    put_be32(arc + 0x08, 4 * 12 + (u32)sizeof(kStrings));
    put_be32(arc + 0x0C, 0x80);
    put_be32(arc + 0x20, 0x01000000u); /* root dir, 4 nodes */
    put_be32(arc + 0x24, 0);
    put_be32(arc + 0x28, 4);
    put_be32(arc + 0x2C, 1);           /* file "Hello.txt" */
    put_be32(arc + 0x30, 0x80);
    put_be32(arc + 0x34, 5);
    put_be32(arc + 0x38, 0x0100000Bu); /* dir "sub", parent 0, ends at node 4 */
    put_be32(arc + 0x3C, 0);
    put_be32(arc + 0x40, 4);
    put_be32(arc + 0x44, 15);          /* file "inner.bin" */
    put_be32(arc + 0x48, 0x88);
    put_be32(arc + 0x4C, 3);
    memcpy(arc + 0x50, kStrings, sizeof(kStrings));
    memcpy(arc + 0x80, "WORLD", 5);
    memcpy(arc + 0x88, "abc", 3);

    assert(fn_8006A3DC((ARCHeader *)arc, &handle) == 1);
    assert(handle.entryNum == 4 && handle.currDir == 0);
    assert(fn_8006A3DC((ARCHeader *)arc, &handle) == 1); /* re-init of a converted buffer */

    assert(fn_8006A480(&handle, "hello.TXT", &af) == 1); /* lookup is case-insensitive */
    assert(fn_8006A9AC(&af) == 5 && memcmp(fn_8006A998(&af), "WORLD", 5) == 0);
    assert(fn_8006A480(&handle, "/sub/inner.bin", &af) == 1);
    assert(fn_8006A9AC(&af) == 3 && memcmp(fn_8006A998(&af), "abc", 3) == 0);
    assert(fn_8006A480(&handle, "sub", &af) == 0);        /* a directory is not a file */
    assert(fn_8006A480(&handle, "missing.bin", &af) == 0);

    assert(fn_8006A9B4(&handle, "sub") == 1);
    assert(fn_8006A8CC(&handle, path, sizeof(path)) && strcmp(path, "/sub/") == 0);
    assert(fn_8006A480(&handle, "inner.bin", &af) == 1 && fn_8006A9AC(&af) == 3);
    assert(fn_8006A480(&handle, "../Hello.txt", &af) == 1 && fn_8006A9AC(&af) == 5);
    assert(fn_8006A9B4(&handle, "inner.bin") == 0);
}

void fzgx_test_native_sdk(void) {
    test_os();
    test_dvd();
    test_vi();
    test_pad();
    test_arc();
}

static int s_stale_reads;
static DVDFileInfo s_boot_fi;
static char s_boot_buf[8];

static void on_stale_read(s32 result, DVDFileInfo *fileInfo) {
    (void)result;
    (void)fileInfo;
    s_stale_reads++;
}

/* Game entry for fzgx_boot_main tests: checks the SDK is freshly booted, runs 3 frames, then
 * leaves it dirty (shrunk arena, interrupts off, a completion pending) for the next boot. */
void fzgx_test_boot_entry(void) {
    PADStatus pads[PAD_MAX_CONTROLLERS];
    u32 frame;

    assert((u8 *)OSGetArenaHi() - (u8 *)OSGetArenaLo() == (ptrdiff_t)FZGX_OS_ARENA_SIZE);
    assert(fzgx_sdk_interrupts_enabled());
    assert(memcmp(DVDGetCurrentDiskID()->gameName, "GFZE", 4) == 0);
    assert(VIGetRetraceCount() == 0);

    for (frame = 0; frame < 3; frame++) {
        VIWaitForRetrace();
        PADRead(pads);
        assert(pads[0].err == PAD_ERR_NO_CONTROLLER);
    }
    assert(VIGetRetraceCount() == 3);
    assert(s_stale_reads == 0); /* a previous boot's completion never reaches this one */

    OSSetArenaLo((u8 *)OSGetArenaLo() + 0x100);
    assert(DVDOpen("readme.txt", &s_boot_fi));
    OSDisableInterrupts();
    assert(DVDReadAsync(&s_boot_fi, s_boot_buf, 5, 0, on_stale_read));
}
