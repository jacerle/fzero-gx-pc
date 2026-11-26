/*
 * Native CARD library (src/sdk/card.c) over a host GCI folder.
 *
 * Entered from tests/test_main.cpp with the synthetic GFZE01 disc mounted, Memory Card slot 0
 * on a GCI folder holding one other game's 2-block file ("GXXE"/"ZZ" "other.dat") and slot 1
 * not mounted. Leaves "fzgx_renamed.dat" (2 blocks: block 0 = card_pattern seed 3, block 1 =
 * seed 11; banner C8, icon 0 C8 at 0, icon speed 3, comment at 5120) for the C++ side to check.
 */

#include "fzgx_port/dolphin_sdk.h"
#include "sdk/sdk_internal.h"

#include <assert.h>
#include <string.h>

void fzgx_test_sdk_card(void);

#define SECTOR 8192
#define USER_BLOCKS 2043

static u8 s_work[CARD_WORKAREA_SIZE];
static u8 s_buf[2 * SECTOR];
static u8 s_read[2 * SECTOR];

static int s_cb_count;
static s32 s_cb_chan;
static s32 s_cb_result;

static void test_callback(s32 chan, s32 result) {
    s_cb_count++;
    s_cb_chan = chan;
    s_cb_result = result;
}

static void card_pattern(u8 *p, u32 n, u32 seed) {
    u32 i;
    for (i = 0; i < n; i++) {
        p[i] = (u8)(i * 7u + seed);
    }
}

/* An async command returned READY: its callback must only run from the next pump. */
static void expect_deferred(int before, s32 chan, s32 expected) {
    assert(s_cb_count == before);
    fzgx_sdk_pump();
    assert(s_cb_count == before + 1);
    assert(s_cb_chan == chan);
    assert(s_cb_result == expected);
    assert(CARDGetResultCode(chan) == expected);
}

static void expect_free(s32 chan, s32 blocks, s32 files) {
    s32 bytes = -1;
    s32 freeFiles = -1;
    assert(CARDFreeBlocks(chan, &bytes, &freeFiles) == CARD_RESULT_READY);
    assert(bytes == blocks * SECTOR);
    assert(freeFiles == files);
}

void fzgx_test_sdk_card(void) {
    CARDFileInfo fi;
    CARDFileInfo fi2;
    CARDStat st;
    s32 memSize = 0;
    s32 sectorSize = 0;
    s32 fileNo;
    u64 serial = 0;
    int before;

    DVDInit();
    assert(memcmp(DVDGetCurrentDiskID()->gameName, "GFZE", 4) == 0);
    assert(memcmp(DVDGetCurrentDiskID()->company, "01", 2) == 0);

    CARDInit();
    assert(CARDGetResultCode(0) == CARD_RESULT_NOCARD);
    assert(CARDGetResultCode(1) == CARD_RESULT_NOCARD);
    assert(CARDGetResultCode(2) == CARD_RESULT_FATAL_ERROR);
    assert(CARDProbeEx(0, &memSize, &sectorSize) == CARD_RESULT_READY);
    assert(memSize == 128 && sectorSize == SECTOR);
    assert(CARDProbeEx(1, NULL, NULL) == CARD_RESULT_NOCARD);

    /* Slot 1 has no card. */
    before = s_cb_count;
    assert(CARDMountAsync(1, s_work, NULL, test_callback) == CARD_RESULT_NOCARD);
    assert(CARDOpen(1, "fzgx_test.dat", &fi) == CARD_RESULT_NOCARD);
    assert(fi.chan == -1);
    assert(CARDFreeBlocks(1, NULL, NULL) == CARD_RESULT_NOCARD);
    assert(CARDUnmount(1) == CARD_RESULT_NOCARD);
    fzgx_sdk_pump();
    assert(s_cb_count == before);

    /* Async mount: BUSY (and other commands refused) until the deferred completion runs. */
    fzgx_sdk_set_interrupts_enabled(FALSE);
    assert(CARDMountAsync(0, s_work, NULL, test_callback) == CARD_RESULT_READY);
    assert(s_cb_count == before);
    assert(CARDGetResultCode(0) == CARD_RESULT_BUSY);
    assert(CARDFreeBlocks(0, NULL, NULL) == CARD_RESULT_BUSY);
    assert(CARDUnmount(0) == CARD_RESULT_BUSY);
    fzgx_sdk_set_interrupts_enabled(TRUE); /* re-enabling pumps */
    assert(s_cb_count == before + 1 && s_cb_chan == 0 && s_cb_result == CARD_RESULT_READY);
    assert(CARDGetResultCode(0) == CARD_RESULT_READY);
    assert(CARDGetSerialNo(0, &serial) == CARD_RESULT_READY && serial != 0);

    /* The other game's file occupies fileNo 0 and 2 blocks, but is not accessible. */
    expect_free(0, USER_BLOCKS - 2, CARD_MAX_FILE - 1);
    assert(CARDOpen(0, "other.dat", &fi) == CARD_RESULT_NOFILE);
    assert(CARDGetStatus(0, 0, &st) == CARD_RESULT_NOPERM);
    assert(CARDFastOpen(0, 0, &fi) == CARD_RESULT_NOPERM);
    assert(CARDFastDelete(0, 0) == CARD_RESULT_NOPERM);
    assert(CARDGetStatus(0, 5, &st) == CARD_RESULT_NOFILE);
    assert(CARDGetStatus(0, CARD_MAX_FILE, &st) == CARD_RESULT_FATAL_ERROR);

    /* Create. */
    assert(CARDCreate(0, "fzgx_test.dat", 2 * SECTOR, &fi) == CARD_RESULT_READY);
    assert(fi.chan == 0 && fi.fileNo == 1 && fi.offset == 0);
    assert(CARDCreate(0, "fzgx_test.dat", SECTOR, &fi2) == CARD_RESULT_EXIST);
    assert(CARDGetResultCode(0) == CARD_RESULT_EXIST);
    assert(CARDCreate(0, "fzgx_odd.dat", 1000, &fi2) == CARD_RESULT_FATAL_ERROR);
    assert(CARDCreate(0, "fzgx_name_longer_than_32_chars.dat", SECTOR, &fi2) == CARD_RESULT_NAMETOOLONG);
    assert(CARDCreate(0, "fzgx_huge.dat", (u32)USER_BLOCKS * SECTOR, &fi2) == CARD_RESULT_INSSPACE);
    expect_free(0, USER_BLOCKS - 4, CARD_MAX_FILE - 2);

    /* Write / read, alignment and range errors. */
    card_pattern(s_buf, sizeof(s_buf), 3);
    assert(CARDWrite(&fi, s_buf, 2 * SECTOR, 0) == CARD_RESULT_READY);
    assert(CARDWrite(&fi, s_buf, CARD_READ_SIZE, 0) == CARD_RESULT_FATAL_ERROR);
    assert(CARDWrite(&fi, s_buf, SECTOR, 2 * SECTOR) == CARD_RESULT_LIMIT);
    assert(CARDRead(&fi, s_read, CARD_READ_SIZE, 100) == CARD_RESULT_FATAL_ERROR);
    assert(CARDRead(&fi, s_read, CARD_READ_SIZE, 2 * SECTOR) == CARD_RESULT_LIMIT);
    assert(CARDRead(&fi, s_read, 2 * SECTOR, CARD_READ_SIZE) == CARD_RESULT_LIMIT);
    memset(s_read, 0, sizeof(s_read));
    assert(CARDRead(&fi, s_read, 2 * CARD_READ_SIZE, SECTOR + CARD_READ_SIZE) == CARD_RESULT_READY);
    assert(memcmp(s_read, s_buf + SECTOR + CARD_READ_SIZE, 2 * CARD_READ_SIZE) == 0);
    assert(CARDClose(&fi) == CARD_RESULT_READY && fi.chan == -1);
    assert(CARDRead(&fi, s_read, CARD_READ_SIZE, 0) == CARD_RESULT_FATAL_ERROR);

    /* Status of a fresh file: no icon/banner/comment. */
    assert(CARDGetStatus(0, 1, &st) == CARD_RESULT_READY);
    assert(strcmp(st.fileName, "fzgx_test.dat") == 0);
    assert(memcmp(st.gameName, "GFZE", 4) == 0 && memcmp(st.company, "01", 2) == 0);
    assert(st.length == 2 * SECTOR);
    assert(st.iconAddr == 0xFFFFFFFFu && st.commentAddr == 0xFFFFFFFFu);
    assert(st.bannerFormat == 0 && st.iconFormat == 0 && st.iconSpeed == 0);
    assert(st.offsetBanner == 0xFFFFFFFFu && st.offsetIconTlut == 0xFFFFFFFFu && st.offsetData == 0);

    /* Set status (async): C8 banner + C8 icon 0 at 0, comment after the image data. */
    st.bannerFormat = 1;
    st.iconAddr = 0;
    st.iconFormat = 0x0001;
    st.iconSpeed = 0x0003;
    st.commentAddr = 5120;
    before = s_cb_count;
    assert(CARDSetStatusAsync(0, 1, &st, test_callback) == CARD_RESULT_READY);
    expect_deferred(before, 0, CARD_RESULT_READY);
    assert(st.offsetBanner == 0 && st.offsetBannerTlut == 3072);
    assert(st.offsetIcon[0] == 3584 && st.offsetIcon[1] == 0xFFFFFFFFu);
    assert(st.offsetIconTlut == 4608 && st.offsetData == 5120);
    st.iconAddr = CARD_READ_SIZE;
    assert(CARDSetStatus(0, 1, &st) == CARD_RESULT_FATAL_ERROR);
    memset(&st, 0, sizeof(st));
    assert(CARDGetStatus(0, 1, &st) == CARD_RESULT_READY);
    assert(st.bannerFormat == 1 && st.iconAddr == 0 && st.iconFormat == 1 && st.iconSpeed == 3);
    assert(st.commentAddr == 5120 && st.offsetData == 5120);

    /* Remount: the directory comes back from the .gci files, current game's files first. */
    assert(CARDUnmount(0) == CARD_RESULT_READY);
    assert(CARDGetResultCode(0) == CARD_RESULT_NOCARD);
    assert(CARDOpen(0, "fzgx_test.dat", &fi) == CARD_RESULT_NOCARD);
    assert(CARDMount(0, s_work, NULL) == CARD_RESULT_READY);
    expect_free(0, USER_BLOCKS - 4, CARD_MAX_FILE - 2);
    assert(CARDOpen(0, "fzgx_test.dat", &fi) == CARD_RESULT_READY);
    fileNo = fi.fileNo;
    assert(fileNo == 0);

    memset(s_read, 0, sizeof(s_read));
    before = s_cb_count;
    fzgx_sdk_set_interrupts_enabled(FALSE);
    assert(CARDReadAsync(&fi, s_read, 2 * SECTOR, 0, test_callback) == CARD_RESULT_READY);
    assert(s_cb_count == before);
    assert(CARDGetResultCode(0) == CARD_RESULT_BUSY);
    fzgx_sdk_set_interrupts_enabled(TRUE);
    assert(s_cb_count == before + 1 && s_cb_result == CARD_RESULT_READY);
    assert(memcmp(s_read, s_buf, sizeof(s_buf)) == 0);

    memset(&st, 0, sizeof(st));
    assert(CARDGetStatus(0, fileNo, &st) == CARD_RESULT_READY);
    assert(strcmp(st.fileName, "fzgx_test.dat") == 0 && st.length == 2 * SECTOR);
    assert(st.bannerFormat == 1 && st.iconAddr == 0 && st.iconFormat == 1 && st.iconSpeed == 3);
    assert(st.commentAddr == 5120 && st.offsetData == 5120);

    /* Partial write of block 1 keeps block 0. */
    card_pattern(s_buf, SECTOR, 11);
    before = s_cb_count;
    assert(CARDWriteAsync(&fi, s_buf, SECTOR, SECTOR, test_callback) == CARD_RESULT_READY);
    expect_deferred(before, 0, CARD_RESULT_READY);
    assert(CARDRead(&fi, s_read, 2 * SECTOR, 0) == CARD_RESULT_READY);
    assert(memcmp(s_read + SECTOR, s_buf, SECTOR) == 0);
    card_pattern(s_buf, SECTOR, 3);
    assert(memcmp(s_read, s_buf, SECTOR) == 0);
    assert(CARDClose(&fi) == CARD_RESULT_READY);

    /* Rename. */
    assert(CARDCreate(0, "fzgx_tmp.dat", SECTOR, &fi2) == CARD_RESULT_READY);
    assert(fi2.fileNo == 2);
    assert(CARDClose(&fi2) == CARD_RESULT_READY);
    before = s_cb_count;
    assert(CARDRenameAsync(0, "fzgx_test.dat", "fzgx_renamed.dat", test_callback) == CARD_RESULT_READY);
    expect_deferred(before, 0, CARD_RESULT_READY);
    assert(CARDOpen(0, "fzgx_test.dat", &fi) == CARD_RESULT_NOFILE);
    assert(CARDOpen(0, "fzgx_renamed.dat", &fi) == CARD_RESULT_READY && fi.fileNo == fileNo);
    assert(CARDClose(&fi) == CARD_RESULT_READY);
    assert(CARDRename(0, "fzgx_renamed.dat", "fzgx_tmp.dat") == CARD_RESULT_EXIST);
    assert(CARDRename(0, "fzgx_missing.dat", "fzgx_x.dat") == CARD_RESULT_NOFILE);
    assert(CARDRename(0, "other.dat", "fzgx_x.dat") == CARD_RESULT_NOFILE);
    assert(CARDRename(0, "", "fzgx_x.dat") == CARD_RESULT_FATAL_ERROR);
    memset(&st, 0, sizeof(st));
    assert(CARDGetStatus(0, fileNo, &st) == CARD_RESULT_READY);
    assert(strcmp(st.fileName, "fzgx_renamed.dat") == 0 && st.commentAddr == 5120);

    /* Delete; fileNo of the remaining files is stable and freed numbers are reused. */
    before = s_cb_count;
    assert(CARDDeleteAsync(0, "fzgx_tmp.dat", test_callback) == CARD_RESULT_READY);
    expect_deferred(before, 0, CARD_RESULT_READY);
    assert(CARDOpen(0, "fzgx_tmp.dat", &fi2) == CARD_RESULT_NOFILE);
    assert(CARDDelete(0, "fzgx_tmp.dat") == CARD_RESULT_NOFILE);
    assert(CARDCreate(0, "fzgx_tmp2.dat", SECTOR, &fi2) == CARD_RESULT_READY && fi2.fileNo == 2);
    before = s_cb_count;
    assert(CARDFastDeleteAsync(0, fi2.fileNo, test_callback) == CARD_RESULT_READY);
    expect_deferred(before, 0, CARD_RESULT_READY);
    assert(CARDGetStatus(0, 2, &st) == CARD_RESULT_NOFILE);
    assert(CARDFastDelete(0, CARD_MAX_FILE) == CARD_RESULT_FATAL_ERROR);
    assert(CARDFastOpen(0, fileNo, &fi) == CARD_RESULT_READY && fi.chan == 0 && fi.fileNo == fileNo);
    assert(CARDClose(&fi) == CARD_RESULT_READY);
    expect_free(0, USER_BLOCKS - 4, CARD_MAX_FILE - 2);

    /* Check. */
    assert(CARDCheck(0) == CARD_RESULT_READY);
    before = s_cb_count;
    assert(CARDCheckAsync(0, test_callback) == CARD_RESULT_READY);
    expect_deferred(before, 0, CARD_RESULT_READY);

    assert(CARDUnmount(0) == CARD_RESULT_READY);
    assert(CARDGetResultCode(0) == CARD_RESULT_NOCARD);
}
