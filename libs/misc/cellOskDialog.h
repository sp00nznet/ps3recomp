/*
 * ps3recomp - cellOskDialog HLE
 *
 * On-screen keyboard dialog: load/unload, get input text.
 */

#ifndef PS3RECOMP_CELL_OSK_DIALOG_H
#define PS3RECOMP_CELL_OSK_DIALOG_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_OSK_ERROR_NOT_INITIALIZED          0x8002B301
#define CELL_OSK_ERROR_ALREADY_INITIALIZED      0x8002B302
#define CELL_OSK_ERROR_INVALID_PARAMETER        0x8002B303
#define CELL_OSK_ERROR_DIALOG_NOT_LOADED        0x8002B304
#define CELL_OSK_ERROR_DIALOG_ALREADY_LOADED    0x8002B305
#define CELL_OSK_ERROR_ABORT                    0x8002B306
#define CELL_OSK_ERROR_UNKNOWN                  0x8002B3FF

/* ---------------------------------------------------------------------------
 * Dialog result codes
 * -----------------------------------------------------------------------*/
#define CELL_OSK_DIALOG_RESULT_OK               0
#define CELL_OSK_DIALOG_RESULT_CANCELED         1
#define CELL_OSK_DIALOG_RESULT_ABORT            2

/* ---------------------------------------------------------------------------
 * Input field types
 * -----------------------------------------------------------------------*/
#define CELL_OSK_DIALOG_INPUT_FIELD_RESULT_OK           0
#define CELL_OSK_DIALOG_INPUT_FIELD_RESULT_CANCELED     1
#define CELL_OSK_DIALOG_INPUT_FIELD_RESULT_ABORT        2
#define CELL_OSK_DIALOG_INPUT_FIELD_RESULT_NO_INPUT_TEXT 3

/* ---------------------------------------------------------------------------
 * Layout modes
 * -----------------------------------------------------------------------*/
#define CELL_OSK_PANEL_MODE_DEFAULT             0
#define CELL_OSK_PANEL_MODE_DEFAULT_NO_JP       1
#define CELL_OSK_PANEL_MODE_ALPHABET_FULL       2
#define CELL_OSK_PANEL_MODE_NUMERAL_FULL        3
#define CELL_OSK_PANEL_MODE_NUMERAL             4
#define CELL_OSK_PANEL_MODE_URL                 5
#define CELL_OSK_PANEL_MODE_EMAIL               6
#define CELL_OSK_PANEL_MODE_PASSWORD            7

/* ---------------------------------------------------------------------------
 * Continuous mode
 * -----------------------------------------------------------------------*/
#define CELL_OSK_CONTINUOUS_MODE_NONE           0
#define CELL_OSK_CONTINUOUS_MODE_SEND           1
#define CELL_OSK_CONTINUOUS_MODE_NEWLINE        2

/* ---------------------------------------------------------------------------
 * Max sizes
 * -----------------------------------------------------------------------*/
#define CELL_OSK_MAX_TEXT_LENGTH                 512
#define CELL_OSK_MAX_TITLE_LENGTH               64

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/* UTF-16LE character (PS3 uses big-endian u16, but for HLE we use host) */
typedef u16 CellOskDialogChar;

typedef struct CellOskDialogInputFieldInfo {
    CellOskDialogChar*  message;        /* prompt text (UTF-16) */
    CellOskDialogChar*  init_text;      /* initial text (UTF-16) */
    u32                 limit_length;   /* max input chars */
} CellOskDialogInputFieldInfo;

typedef struct CellOskDialogCallbackReturnParam {
    s32                 result;         /* CELL_OSK_DIALOG_INPUT_FIELD_RESULT_* */
    u32                 numCharsResultString;
    CellOskDialogChar*  pResultString;
} CellOskDialogCallbackReturnParam;

typedef struct CellOskDialogPoint {
    float x;
    float y;
} CellOskDialogPoint;

typedef struct CellOskDialogParam {
    u32                 allowOskPanelFlg;
    u32                 firstViewPanel;
    CellOskDialogPoint  controlPoint;
    s32                 prohibitFlgs;
} CellOskDialogParam;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellOskDialogInit(u32 container);
s32 cellOskDialogEnd(void);

s32 cellOskDialogLoadAsync(u32 container,
                           const CellOskDialogParam* dialogParam,
                           const CellOskDialogInputFieldInfo* inputFieldInfo);
s32 cellOskDialogUnloadAsync(CellOskDialogCallbackReturnParam* result);

s32 cellOskDialogGetSize(u32* width, u32* height);
s32 cellOskDialogAbort(void);

s32 cellOskDialogSetInitialInputDevice(u32 device);
s32 cellOskDialogSetInitialKeyLayout(u32 layout);
s32 cellOskDialogSetLayoutMode(u32 mode);
s32 cellOskDialogSetSeparateWindowOption(u32 option);

s32 cellOskDialogGetInputText(CellOskDialogCallbackReturnParam* result);

/* Set a default response string (call before LoadAsync if desired) */
void cellOskDialogSetDefaultResponse(const char* text);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_OSK_DIALOG_H */
