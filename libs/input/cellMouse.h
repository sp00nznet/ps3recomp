/*
 * ps3recomp - cellMouse HLE
 *
 * Mouse input: initialization, reading movement and button state.
 */

#ifndef PS3RECOMP_CELL_MOUSE_H
#define PS3RECOMP_CELL_MOUSE_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/

#define CELL_MOUSE_ERROR_FATAL              (CELL_ERROR_BASE_MOUSE | 0x01)
#define CELL_MOUSE_ERROR_INVALID_PARAMETER  (CELL_ERROR_BASE_MOUSE | 0x02)
#define CELL_MOUSE_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_MOUSE | 0x03)
#define CELL_MOUSE_ERROR_UNINITIALIZED      (CELL_ERROR_BASE_MOUSE | 0x04)
#define CELL_MOUSE_ERROR_NO_DEVICE          (CELL_ERROR_BASE_MOUSE | 0x05)

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define CELL_MOUSE_MAX_MICE             2
#define CELL_MOUSE_MAX_DATA_LIST_NUM    8

/* Button flags */
#define CELL_MOUSE_BUTTON_1             (1 << 0)  /* Left */
#define CELL_MOUSE_BUTTON_2             (1 << 1)  /* Right */
#define CELL_MOUSE_BUTTON_3             (1 << 2)  /* Middle */
#define CELL_MOUSE_BUTTON_4             (1 << 3)  /* Side 1 */
#define CELL_MOUSE_BUTTON_5             (1 << 4)  /* Side 2 */

/* Connection status */
#define CELL_MOUSE_STATUS_DISCONNECTED  0
#define CELL_MOUSE_STATUS_CONNECTED     1

/* Tablet mode */
#define CELL_MOUSE_MODE_RELATIVE        0
#define CELL_MOUSE_MODE_ABSOLUTE        1

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/* Single mouse data sample */
typedef struct CellMouseData {
    u8  update;         /* Non-zero if data has changed since last read */
    u8  buttons;        /* CELL_MOUSE_BUTTON_* flags */
    s8  x_axis;         /* X movement delta */
    s8  y_axis;         /* Y movement delta */
    s8  wheel;          /* Scroll wheel delta */
    s8  tilt;           /* Tilt wheel delta */
} CellMouseData;

/* Mouse data list (buffered) */
typedef struct CellMouseDataList {
    u32          list_num;
    CellMouseData list[CELL_MOUSE_MAX_DATA_LIST_NUM];
} CellMouseDataList;

/* Mouse info */
typedef struct CellMouseInfo {
    u32 max_connect;
    u32 now_connect;
    u32 info;
    u32 vendor_id[CELL_MOUSE_MAX_MICE];
    u32 product_id[CELL_MOUSE_MAX_MICE];
    u32 status[CELL_MOUSE_MAX_MICE];
} CellMouseInfo;

/* Raw data (for tablet mode) */
typedef struct CellMouseRawData {
    s32 len;
    u8  data[64];
} CellMouseRawData;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellMouseInit(u32 max_connect);
s32 cellMouseEnd(void);
s32 cellMouseGetData(u32 port_no, CellMouseData* data);
s32 cellMouseGetDataList(u32 port_no, CellMouseDataList* data);
s32 cellMouseGetInfo(CellMouseInfo* info);
s32 cellMouseSetTabletMode(u32 port_no, u32 mode);
s32 cellMouseClearBuf(u32 port_no);

/* Internal: feed mouse events from host input system */
void cellMouse_moveEvent(u32 port, s8 dx, s8 dy);
void cellMouse_buttonEvent(u32 port, u8 button_mask, int pressed);
void cellMouse_wheelEvent(u32 port, s8 wheel);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_MOUSE_H */
