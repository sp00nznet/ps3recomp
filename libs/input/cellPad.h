/*
 * ps3recomp - cellPad HLE
 *
 * Gamepad input: initialization, reading button/analog state.
 * Backend: XInput on Windows, SDL2 GameController elsewhere.
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

/* PS button (in button[3] high byte) */
#define CELL_PAD_CTRL_PS            (1 << 0)

/* Port setting flags */
#define CELL_PAD_SETTING_PRESS_ON       (1 << 1)
#define CELL_PAD_SETTING_SENSOR_ON      (1 << 2)
#define CELL_PAD_SETTING_PRESS_OFF      0
#define CELL_PAD_SETTING_SENSOR_OFF     0

/* Port status flags */
#define CELL_PAD_STATUS_DISCONNECTED    0
#define CELL_PAD_STATUS_CONNECTED       1
#define CELL_PAD_STATUS_ASSIGN_CHANGES  2

/* Device type */
#define CELL_PAD_DEV_TYPE_STANDARD      0
#define CELL_PAD_DEV_TYPE_BD_REMOCON    4
#define CELL_PAD_DEV_TYPE_LDD           5

/* Device capability flags */
#define CELL_PAD_CAPABILITY_PS3_CONFORMITY  (1 << 0)
#define CELL_PAD_CAPABILITY_PRESS_MODE      (1 << 1)
#define CELL_PAD_CAPABILITY_SENSOR_MODE     (1 << 2)
#define CELL_PAD_CAPABILITY_HP_ANALOG_STICK (1 << 3)
#define CELL_PAD_CAPABILITY_ACTUATOR        (1 << 4)

/* Standard data lengths */
#define CELL_PAD_LEN_NO_CHANGE          0
#define CELL_PAD_LEN_CHANGE_DEFAULT     8
#define CELL_PAD_LEN_CHANGE_PRESS_ON    24
#define CELL_PAD_LEN_CHANGE_SENSOR_ON   28

/* Pressure-sensitive button offsets in button[] array */
#define CELL_PAD_BTN_OFFSET_DIGITAL1    2
#define CELL_PAD_BTN_OFFSET_DIGITAL2    3
#define CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X  4
#define CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y  5
#define CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X   6
#define CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y   7
#define CELL_PAD_BTN_OFFSET_PRESS_RIGHT     8
#define CELL_PAD_BTN_OFFSET_PRESS_LEFT      9
#define CELL_PAD_BTN_OFFSET_PRESS_UP        10
#define CELL_PAD_BTN_OFFSET_PRESS_DOWN      11
#define CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE  12
#define CELL_PAD_BTN_OFFSET_PRESS_CIRCLE    13
#define CELL_PAD_BTN_OFFSET_PRESS_CROSS     14
#define CELL_PAD_BTN_OFFSET_PRESS_SQUARE    15
#define CELL_PAD_BTN_OFFSET_PRESS_L1        16
#define CELL_PAD_BTN_OFFSET_PRESS_R1        17
#define CELL_PAD_BTN_OFFSET_PRESS_L2        18
#define CELL_PAD_BTN_OFFSET_PRESS_R2        19
#define CELL_PAD_BTN_OFFSET_SENSOR_X        20
#define CELL_PAD_BTN_OFFSET_SENSOR_Y        21
#define CELL_PAD_BTN_OFFSET_SENSOR_Z        22
#define CELL_PAD_BTN_OFFSET_SENSOR_G        23

/* Actuator (rumble) parameters */
#define CELL_PAD_ACTUATOR_MAX           2
#define CELL_PAD_ACTUATOR_PARAM_SMALL   0
#define CELL_PAD_ACTUATOR_PARAM_LARGE   1

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
 *   [3] = digital buttons bitmask continued (PS button, etc.)
 *   [4] = right analog stick X (0-255, center=128)
 *   [5] = right analog stick Y (0-255, center=128)
 *   [6] = left  analog stick X (0-255, center=128)
 *   [7] = left  analog stick Y (0-255, center=128)
 *   [8..19] = pressure-sensitive button values (0-255 each)
 *   [20..23] = sensor data (accel X, Y, Z, gyro)
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

/* Capability info for a specific port */
typedef struct CellPadCapabilityInfo {
    u32 info[CELL_PAD_MAX_CODES];
} CellPadCapabilityInfo;

/* Actuator (vibration) parameter */
typedef struct CellPadActParam {
    u8 motor[CELL_PAD_ACTUATOR_MAX];
} CellPadActParam;

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

/* NID: 0x578E3C98 */
s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info);

/* NID: 0xBE5C3E81 */
s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param);

/* NID: 0x3733EA3C */
s32 cellPadClearBuf(u32 port_no);

/* Internal: call once per frame to update pad state from host input */
void cellPad_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PAD_H */
