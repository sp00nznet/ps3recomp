/*
 * ps3recomp - cellGem HLE
 *
 * PlayStation Move motion controller. Stub — reports no Move attached.
 * Games handle gracefully with controller selection UI.
 */

#ifndef PS3RECOMP_CELL_GEM_H
#define PS3RECOMP_CELL_GEM_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_GEM_ERROR_NOT_INITIALIZED     0x80121801
#define CELL_GEM_ERROR_ALREADY_INITIALIZED 0x80121802
#define CELL_GEM_ERROR_INVALID_ARGUMENT    0x80121803
#define CELL_GEM_ERROR_NOT_CONNECTED       0x80121804
#define CELL_GEM_ERROR_NOT_CALIBRATED      0x80121805
#define CELL_GEM_ERROR_NO_VIDEO            0x80121806

/* Constants */
#define CELL_GEM_MAX_NUM   4

/* Status flags */
#define CELL_GEM_STATUS_DISCONNECTED   0
#define CELL_GEM_STATUS_READY          1
#define CELL_GEM_STATUS_CALIBRATING    2

/* Types */
typedef struct CellGemAttribute {
    u32 version;
    u32 maxConnect;
    void* spursAddr;
    u8  spu[8];
} CellGemAttribute;

typedef struct CellGemInfo {
    u32 maxConnect;
    u32 nowConnect;
    u32 status[CELL_GEM_MAX_NUM];
    u16 port[CELL_GEM_MAX_NUM];
} CellGemInfo;

typedef struct CellGemState {
    float pos[4];    /* x, y, z, w */
    float quat[4];   /* quaternion orientation */
    u32 padData;
    u32 extData;
    u64 timestamp;
    float temperature;
    float accel[4];
    float gyro[4];
} CellGemState;

/* Functions */
s32 cellGemInit(const CellGemAttribute* attr);
s32 cellGemEnd(void);

s32 cellGemGetInfo(CellGemInfo* info);
s32 cellGemGetState(u32 gemNum, u32 flag, u64 timestamp, CellGemState* state);

s32 cellGemIsTrackableHue(u32 hue);
s32 cellGemGetHue(u32 gemNum, u32* hue);
s32 cellGemTrackHues(const u32* reqHues, u32* resHues);

s32 cellGemCalibrate(u32 gemNum);
s32 cellGemGetStatusFlags(u32 gemNum, u64* flags);

s32 cellGemUpdateStart(const void* cameraFrame, u64 timestamp);
s32 cellGemUpdateFinish(void);

s32 cellGemReset(u32 gemNum);
s32 cellGemEnableMagnetometer(u32 gemNum, s32 enable);
s32 cellGemEnableCameraPitchAngleCorrection(s32 enable);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GEM_H */
