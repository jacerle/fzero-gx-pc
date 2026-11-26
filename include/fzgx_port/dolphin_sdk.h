#ifndef FZGX_PORT_DOLPHIN_SDK_H
#define FZGX_PORT_DOLPHIN_SDK_H

/*
 * Dolphin SDK surface implemented natively by src/sdk/ (R4 platform boundary): the retail
 * OS / DVD / VI / PAD / CARD libraries drive GameCube hardware, so the port replaces them with
 * host-backed implementations behind include/fzgx_port/platform_api.h.
 *
 * Types the decomp already publishes come from its headers; the rest use the retail SDK
 * layouts as reconstructed in external/fzero-gx-decomp (dvdfs.c, Pad.c, VI.c). C only:
 * the decomp's card.h is not valid C++.
 */

#include "fzgx_port/mwcc_shim.h"

#include <dolphin/card.h>
#include <dolphin/dvd.h>
#include <dolphin/os/OSTime.h>

/* ---------------------------------------------------------------- OS */

#define OS_BUS_CLOCK 162000000u
#define OS_TIMER_CLOCK (OS_BUS_CLOCK / 4)
#define OSTicksToMilliseconds(ticks) ((ticks) / (OS_TIMER_CLOCK / 1000))
#define OSMillisecondsToTicks(msec) ((msec) * (OS_TIMER_CLOCK / 1000))

/* Size of the host-backed MEM1 arena handed out by OSGetArenaLo/Hi after OSInit. */
#define FZGX_OS_ARENA_SIZE (24u * 1024u * 1024u)

void OSInit(void);
void OSReport(const char *msg, ...);
void OSPanic(const char *file, int line, const char *msg, ...);
void *OSGetArenaLo(void);
void *OSGetArenaHi(void);
void OSSetArenaLo(void *newLo);
void OSSetArenaHi(void *newHi);
BOOL OSDisableInterrupts(void);
BOOL OSEnableInterrupts(void);
BOOL OSRestoreInterrupts(BOOL level);
void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime *td);

/* ---------------------------------------------------------------- DVD */

#define DVD_STATE_FATAL_ERROR -1
#define DVD_STATE_END 0
#define DVD_STATE_BUSY 1
#define DVD_STATE_WAITING 2
#define DVD_STATE_NO_DISK 4
#define DVD_STATE_CANCELED 10

#define DVD_RESULT_GOOD 0
#define DVD_RESULT_FATAL_ERROR -1
#define DVD_RESULT_IGNORED -2
#define DVD_RESULT_CANCELED -3

typedef struct DVDFileInfo DVDFileInfo;
typedef void (*DVDCallback)(s32 result, DVDFileInfo *fileInfo);

struct DVDFileInfo {
    DVDCommandBlock cb;
    u32 startAddr; /* logical disc offset of the file */
    u32 length;
    DVDCallback callback;
};

void DVDInit(void);
s32 DVDConvertPathToEntrynum(const char *pathPtr);
BOOL DVDFastOpen(s32 entrynum, DVDFileInfo *fileInfo);
BOOL DVDOpen(const char *fileName, DVDFileInfo *fileInfo);
BOOL DVDClose(DVDFileInfo *fileInfo);
BOOL DVDChangeDir(const char *dirName);
BOOL DVDGetCurrentDir(char *path, u32 maxlen);
s32 DVDReadPrio(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset, s32 prio);
s32 DVDRead(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset);
BOOL DVDReadAsyncPrio(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset, DVDCallback callback,
                      s32 prio);
BOOL DVDReadAsync(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset, DVDCallback callback);
BOOL DVDReadAbsAsyncPrio(DVDCommandBlock *block, void *addr, s32 length, s32 offset,
                         DVDCBCallback callback, s32 prio);
s32 DVDGetCommandBlockStatus(const DVDCommandBlock *block);
s32 DVDGetFileInfoStatus(const DVDFileInfo *fileInfo);
s32 DVDGetDriveStatus(void);
u32 DVDGetTransferredSize(DVDCommandBlock *block);
DVDDiskID *DVDGetCurrentDiskID(void);
BOOL DVDCheckDisk(void);

/* ---------------------------------------------------------------- VI */

typedef struct _GXRenderModeObj GXRenderModeObj;
typedef void (*VIRetraceCallback)(u32 retraceCount);

#define VI_NTSC 0
#define VI_PAL 1
#define VI_MPAL 2

void VIInit(void);
void VIConfigure(const GXRenderModeObj *rm);
void VIFlush(void);
void VISetNextFrameBuffer(void *fb);
void *VIGetNextFrameBuffer(void);
void VISetBlack(BOOL black);
void VIWaitForRetrace(void);
u32 VIGetRetraceCount(void);
u32 VIGetNextField(void);
u32 VIGetCurrentLine(void);
u32 VIGetTvFormat(void);
u32 VIGetDTVStatus(void);
VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback);
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback);

/* ---------------------------------------------------------------- PAD */

#define PAD_MAX_CONTROLLERS 4

#define PAD_BUTTON_LEFT 0x0001
#define PAD_BUTTON_RIGHT 0x0002
#define PAD_BUTTON_DOWN 0x0004
#define PAD_BUTTON_UP 0x0008
#define PAD_TRIGGER_Z 0x0010
#define PAD_TRIGGER_R 0x0020
#define PAD_TRIGGER_L 0x0040
#define PAD_USE_ORIGIN 0x0080
#define PAD_BUTTON_A 0x0100
#define PAD_BUTTON_B 0x0200
#define PAD_BUTTON_X 0x0400
#define PAD_BUTTON_Y 0x0800
#define PAD_BUTTON_MENU 0x1000
#define PAD_BUTTON_START 0x1000

#define PAD_ERR_NONE 0
#define PAD_ERR_NO_CONTROLLER -1
#define PAD_ERR_NOT_READY -2
#define PAD_ERR_TRANSFER -3

#define PAD_MOTOR_STOP 0
#define PAD_MOTOR_RUMBLE 1
#define PAD_MOTOR_STOP_HARD 2

#define PAD_CHAN0_BIT 0x80000000u

typedef struct PADStatus {
    u16 button;
    s8 stickX;
    s8 stickY;
    s8 substickX;
    s8 substickY;
    u8 triggerLeft;
    u8 triggerRight;
    u8 analogA;
    u8 analogB;
    s8 err;
} PADStatus;

BOOL PADInit(void);
u32 PADRead(PADStatus *status);
BOOL PADReset(u32 mask);
BOOL PADRecalibrate(u32 mask);
void PADControlMotor(s32 chan, u32 command);
void PADSetSpec(u32 spec);
void PADSetAnalogMode(u32 mode);
/* Retail stick/trigger clamp; compiled from the decomp's ClampStick.c TU. */
void PADClamp(PADStatus *status);

/* ---------------------------------------------------------------- CARD */
/* Declarations beyond <dolphin/card.h> for the native CARD layer (src/sdk/card.c). */

#define CARD_RESULT_READY 0
#define CARD_RESULT_BUSY -1
#define CARD_RESULT_WRONGDEVICE -2
#define CARD_RESULT_NOCARD -3
#define CARD_RESULT_NOFILE -4
#define CARD_RESULT_IOERROR -5
#define CARD_RESULT_BROKEN -6
#define CARD_RESULT_EXIST -7
#define CARD_RESULT_NOENT -8
#define CARD_RESULT_INSSPACE -9
#define CARD_RESULT_NOPERM -10
#define CARD_RESULT_LIMIT -11
#define CARD_RESULT_NAMETOOLONG -12
#define CARD_RESULT_ENCODING -13
#define CARD_RESULT_CANCELED -14
#define CARD_RESULT_FATAL_ERROR -128

#define CARD_READ_SIZE 512
#define CARD_FILENAME_MAX 32
#define CARD_MAX_FILE 127

/* Size of the work area CARDMount/CARDMountAsync take (unused by the native layer). */
#define CARD_WORKAREA_SIZE (5 * 8 * 1024)

void CARDInit(void);
s32 CARDMount(s32 chan, void *workArea, CARDCallback detachCallback);
s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo *fileInfo);
s32 CARDRead(CARDFileInfo *fileInfo, void *buf, s32 length, s32 offset);
s32 CARDWrite(CARDFileInfo *fileInfo, const void *buf, s32 length, s32 offset);
s32 CARDDelete(s32 chan, const char *fileName);
s32 CARDFastDelete(s32 chan, s32 fileNo);
s32 CARDRename(s32 chan, const char *oldName, const char *newName);
s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat *stat);
s32 CARDFormat(s32 chan);
s32 CARDFormatAsync(s32 chan, CARDCallback callback);

#endif /* FZGX_PORT_DOLPHIN_SDK_H */
