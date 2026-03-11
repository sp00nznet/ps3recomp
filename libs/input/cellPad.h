/*
 * ps3recomp - cellPad HLE stub
 *
 * Gamepad input: initialization, reading button/analog state.
 */

#ifndef PS3RECOMP_CELL_PAD_H
#define PS3RECOMP_CELL_PAD_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define CELL_PAD_MAX_PORT_NUM       7
#define CELL_PAD_MAX_CODES          64
#define CELL_MAX_PADS               CELL_PAD_MAX_PORT_NUM

/* Button bitmask constants (digital buttons, in button[2] low word) */
#define CELL_PAD_CTRL_SELECT        (1 << 0)
#define CELL_PAD_CTRL_L3            (1 << 1)
#define CELL_PAD_CTRL_R3            (1 << 2)
#define CELL_PAD_CTRL_START         (1 << 3)
#define CELL_PAD_CTRL_UP            (1 << 4)
#define CELL_PAD_CTRL_RIGHT         (1 << 5)
#define CELL_PAD_CTRL_DOWN          (1 << 6)
#define CELL_PAD_CTRL_LEFT          (1 << 7)
#define CELL_PAD_CTRL_L2            (1 << 8)
#define CELL_PAD_CTRL_R2            (1 << 9)
#define CELL_PAD_CTRL_L1            (1 << 10)
#define CELL_PAD_CTRL_R1            (1 << 11)
#define CELL_PAD_CTRL_TRIANGLE      (1 << 12)
#define CELL_PAD_CTRL_CIRCLE        (1 << 13)
#define CELL_PAD_CTRL_CROSS         (1 << 14)
#define CELL_PAD_CTRL_SQUARE        (1 << 15)

/* Port setting flags */
#define CELL_PAD_SETTING_PRESS_ON       (1 << 1)
#define CELL_PAD_SETTING_SENSOR_ON      (1 << 2)
#define CELL_PAD_SETTING_PRESS_OFF      0
#define CELL_PAD_SETTING_SENSOR_OFF     0

/* Device type */
#define CELL_PAD_DEV_TYPE_STANDARD      0
#define CELL_PAD_DEV_TYPE_BD_REMOCON    4

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/*
 * CellPadData -- returned by cellPadGetData.
 *
 * Layout of the button[] array (each element is u16):
 *   [0] = data length (number of valid entries following)
 *   [1] = reserved
 *   [2] = digital buttons bitmask (CELL_PAD_CTRL_*)
 *   [3] = digital buttons bitmask continued
 *   [4] = right analog stick X (0-255, center=128)
 *   [5] = right analog stick Y (0-255, center=128)
 *   [6] = left  analog stick X (0-255, center=128)
 *   [7] = left  analog stick Y (0-255, center=128)
 *   [8..23] = pressure-sensitive button values (0-255 each)
 *   [24..27] = sensor data (accel X, Y, Z, gyro)
 */
typedef struct CellPadData {
    s32 len;                          /* number of valid button entries */
    u16 button[CELL_PAD_MAX_CODES];   /* button/analog/sensor data */
} CellPadData;

/* Per-port info returned by cellPadGetInfo2 */
typedef struct CellPadInfo2 {
    u32 max_connect;
    u32 now_connect;
    u32 system_info;
    u32 port_status[CELL_PAD_MAX_PORT_NUM];
    u32 port_setting[CELL_PAD_MAX_PORT_NUM];
    u32 device_capability[CELL_PAD_MAX_PORT_NUM];
    u32 device_type[CELL_PAD_MAX_PORT_NUM];
} CellPadInfo2;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* NID: 0x1CF98800 */
s32 cellPadInit(u32 max_connect);

/* NID: 0x4D9B75D5 */
s32 cellPadEnd(void);

/* NID: 0x8B72CBA1 */
s32 cellPadGetData(u32 port_no, CellPadData* data);

/* NID: 0x8B8013DA */
s32 cellPadGetInfo2(CellPadInfo2* info);

/* NID: 0xA703A51D */
s32 cellPadSetPortSetting(u32 port_no, u32 port_setting);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PAD_H */
