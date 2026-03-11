/*
 * ps3recomp - cellSysmodule HLE stub
 *
 * Module loading / unloading system.  On real PS3 hardware these calls
 * load PRX modules from flash or disc.  In the recompiler we track
 * load state and wire up HLE function tables.
 */

#ifndef PS3RECOMP_CELL_SYSMODULE_H
#define PS3RECOMP_CELL_SYSMODULE_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Module ID constants
 * -----------------------------------------------------------------------*/
#define CELL_SYSMODULE_NET                  0x0000
#define CELL_SYSMODULE_HTTP                 0x0001
#define CELL_SYSMODULE_HTTP_UTIL            0x0002
#define CELL_SYSMODULE_SSL                  0x0003
#define CELL_SYSMODULE_HTTPS                0x0004
#define CELL_SYSMODULE_VDEC                 0x0005
#define CELL_SYSMODULE_ADEC                 0x0006
#define CELL_SYSMODULE_DMUX                 0x0007
#define CELL_SYSMODULE_VPOST                0x0008
#define CELL_SYSMODULE_RTC                  0x0009
#define CELL_SYSMODULE_SPURS                0x000A
#define CELL_SYSMODULE_OVIS                 0x000B
#define CELL_SYSMODULE_SHEAP                0x000C
#define CELL_SYSMODULE_SYNC                 0x000D
#define CELL_SYSMODULE_SYNC2                0x000E
#define CELL_SYSMODULE_FS                   0x000F
#define CELL_SYSMODULE_JPGDEC               0x0010
#define CELL_SYSMODULE_GCM_SYS              0x0011
#define CELL_SYSMODULE_AUDIO                0x0012
#define CELL_SYSMODULE_PAMF                 0x0013
#define CELL_SYSMODULE_ATRAC3PLUS           0x0014
#define CELL_SYSMODULE_NETCTL               0x0015
#define CELL_SYSMODULE_SYSUTIL              0x0016
#define CELL_SYSMODULE_SYSUTIL_NP           0x0017
#define CELL_SYSMODULE_IO                   0x0018
#define CELL_SYSMODULE_PNGDEC               0x0019
#define CELL_SYSMODULE_FONT                 0x001A
#define CELL_SYSMODULE_FREETYPE             0x001B
#define CELL_SYSMODULE_USBD                 0x001C
#define CELL_SYSMODULE_SAIL                 0x001D
#define CELL_SYSMODULE_L10N                 0x001E
#define CELL_SYSMODULE_RESC                 0x001F
#define CELL_SYSMODULE_DAISY                0x0020
#define CELL_SYSMODULE_KEY2CHAR             0x0021
#define CELL_SYSMODULE_MIC                  0x0022
#define CELL_SYSMODULE_AVCONF_EXT           0x0023
#define CELL_SYSMODULE_USERINFO             0x0024
#define CELL_SYSMODULE_SYSUTIL_SAVEDATA     0x0025
#define CELL_SYSMODULE_SUBDISPLAY           0x0026
#define CELL_SYSMODULE_SYSUTIL_REC          0x0027
#define CELL_SYSMODULE_VIDEO_EXPORT         0x0028
#define CELL_SYSMODULE_SYSUTIL_GAME_EXEC    0x0029
#define CELL_SYSMODULE_SYSUTIL_NP2          0x002A
#define CELL_SYSMODULE_SYSUTIL_AP           0x002B
#define CELL_SYSMODULE_SYSUTIL_NP_CLANS     0x002C
#define CELL_SYSMODULE_SYSUTIL_OSK_EXT      0x002D
#define CELL_SYSMODULE_VDEC_DIVX            0x002E
#define CELL_SYSMODULE_JPGENC               0x002F
#define CELL_SYSMODULE_SYSUTIL_GAME         0x0030
#define CELL_SYSMODULE_BGDL                 0x0031
#define CELL_SYSMODULE_FREETYPE_TT          0x0032
#define CELL_SYSMODULE_SYSUTIL_VIDEO_UPLOAD 0x0033
#define CELL_SYSMODULE_SYSUTIL_SYSCONF_EXT  0x0034
#define CELL_SYSMODULE_FIBER                0x0035
#define CELL_SYSMODULE_SYSUTIL_NP_COMMERCE2 0x0036
#define CELL_SYSMODULE_SYSUTIL_NP_TUS       0x0037
#define CELL_SYSMODULE_VOICE                0x0038
#define CELL_SYSMODULE_ADEC_CELP8           0x0039
#define CELL_SYSMODULE_CELP8ENC             0x003A
#define CELL_SYSMODULE_SYSUTIL_LICENSEAREA  0x003B
#define CELL_SYSMODULE_SYSUTIL_MUSIC2       0x003C
#define CELL_SYSMODULE_SYSUTIL_SCREENSHOT   0x003E
#define CELL_SYSMODULE_SYSUTIL_MUSIC_DECODE 0x003F
#define CELL_SYSMODULE_SPURS_JQ             0x0040
#define CELL_SYSMODULE_PNGENC               0x0041
#define CELL_SYSMODULE_SYSUTIL_MUSIC_DECODE2 0x0043
#define CELL_SYSMODULE_IMEJP               0x0044
#define CELL_SYSMODULE_SYSUTIL_NP_UTIL      0x0046
#define CELL_SYSMODULE_RUDP                 0x0050
#define CELL_SYSMODULE_SYSUTIL_NP_SNS       0x0051
#define CELL_SYSMODULE_GEM                  0x0052

/* Aliases used in game code */
#define CELL_SYSMODULE_GCM          CELL_SYSMODULE_GCM_SYS

/* Max module ID for tracking array */
#define CELL_SYSMODULE_MAX_ID       0x0100

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* NID: 0x26A6E12B */
s32 cellSysmoduleLoadModule(u16 id);

/* NID: 0x112A5EE9 */
s32 cellSysmoduleUnloadModule(u16 id);

/* NID: 0x5A59E258 */
s32 cellSysmoduleIsLoaded(u16 id);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SYSMODULE_H */
