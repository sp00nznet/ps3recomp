/*
 * ps3recomp - cellGameExec HLE
 *
 * Game execution control: boot other games, exit to XMB, get boot parameters.
 */

#ifndef PS3RECOMP_CELL_GAME_EXEC_H
#define PS3RECOMP_CELL_GAME_EXEC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Boot type constants
 * -----------------------------------------------------------------------*/
#define CELL_GAME_GAMETYPE_DISC            1
#define CELL_GAME_GAMETYPE_HDD             2
#define CELL_GAME_GAMETYPE_GAMEDATA        3
#define CELL_GAME_GAMETYPE_HOME            4

/* Exit types */
#define CELL_GAME_EXIT_NORMAL              0
#define CELL_GAME_EXIT_HOMEBREW            1

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

typedef struct CellGameExecBootParam {
    u32  type;
    char titleId[16];
    char dirName[32];
} CellGameExecBootParam;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* Set the next game to boot (returns to XMB or loads another title) */
s32 cellGameSetExitParam(const CellGameExecBootParam* param);
s32 cellGameGetExitParam(CellGameExecBootParam* param);

/* Exit current game */
void cellGameExitToShelf(void);

/* Get boot information */
s32 cellGameGetBootGameInfo(u32* type, char* dirName, u32* execData);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GAME_EXEC_H */
