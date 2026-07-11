/*
 * ps3recomp - cellSysmodule HLE stub implementation
 */

#include "cellSysmodule.h"
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Track which modules have been "loaded". IDs come in two ranges: 0x00xx
 * (core) and 0xF0xx (extended: NP_TROPHY=0xF035, GIFDEC=0xF010, ...). The old
 * `id >= MAX_ID -> ERROR_UNKNOWN` check rejected every 0xF0xx load — LBP's
 * trophy-module load failed at boot. Map both ranges into one table. */
static int s_module_loaded[2 * CELL_SYSMODULE_MAX_ID];
static int sysmodule_slot(u16 id)
{
    if (id < CELL_SYSMODULE_MAX_ID) return (int)id;
    if ((id & 0xFF00) == 0xF000 && (id & 0xFF) < CELL_SYSMODULE_MAX_ID)
        return CELL_SYSMODULE_MAX_ID + (id & 0xFF);
    return -1;
}

static const char* sysmodule_id_to_name(u16 id)
{
    switch (id) {
    case CELL_SYSMODULE_NET:           return "CELL_SYSMODULE_NET";
    case CELL_SYSMODULE_HTTP:          return "CELL_SYSMODULE_HTTP";
    case CELL_SYSMODULE_HTTP_UTIL:     return "CELL_SYSMODULE_HTTP_UTIL";
    case CELL_SYSMODULE_SSL:           return "CELL_SYSMODULE_SSL";
    case CELL_SYSMODULE_HTTPS:         return "CELL_SYSMODULE_HTTPS";
    case CELL_SYSMODULE_VDEC:          return "CELL_SYSMODULE_VDEC";
    case CELL_SYSMODULE_ADEC:          return "CELL_SYSMODULE_ADEC";
    case CELL_SYSMODULE_DMUX:          return "CELL_SYSMODULE_DMUX";
    case CELL_SYSMODULE_VPOST:         return "CELL_SYSMODULE_VPOST";
    case CELL_SYSMODULE_RTC:           return "CELL_SYSMODULE_RTC";
    case CELL_SYSMODULE_SPURS:         return "CELL_SYSMODULE_SPURS";
    case CELL_SYSMODULE_OVIS:          return "CELL_SYSMODULE_OVIS";
    case CELL_SYSMODULE_SHEAP:         return "CELL_SYSMODULE_SHEAP";
    case CELL_SYSMODULE_SYNC:          return "CELL_SYSMODULE_SYNC";
    case CELL_SYSMODULE_SYNC2:         return "CELL_SYSMODULE_SYNC2";
    case CELL_SYSMODULE_FS:            return "CELL_SYSMODULE_FS";
    case CELL_SYSMODULE_JPGDEC:        return "CELL_SYSMODULE_JPGDEC";
    case CELL_SYSMODULE_GCM_SYS:       return "CELL_SYSMODULE_GCM_SYS";
    case CELL_SYSMODULE_AUDIO:         return "CELL_SYSMODULE_AUDIO";
    case CELL_SYSMODULE_PAMF:          return "CELL_SYSMODULE_PAMF";
    case CELL_SYSMODULE_ATRAC3PLUS:    return "CELL_SYSMODULE_ATRAC3PLUS";
    case CELL_SYSMODULE_NETCTL:        return "CELL_SYSMODULE_NETCTL";
    case CELL_SYSMODULE_SYSUTIL:       return "CELL_SYSMODULE_SYSUTIL";
    case CELL_SYSMODULE_SYSUTIL_NP:    return "CELL_SYSMODULE_SYSUTIL_NP";
    case CELL_SYSMODULE_IO:            return "CELL_SYSMODULE_IO";
    case CELL_SYSMODULE_PNGDEC:        return "CELL_SYSMODULE_PNGDEC";
    case CELL_SYSMODULE_FONT:          return "CELL_SYSMODULE_FONT";
    case CELL_SYSMODULE_FREETYPE:      return "CELL_SYSMODULE_FREETYPE";
    case CELL_SYSMODULE_USBD:          return "CELL_SYSMODULE_USBD";
    case CELL_SYSMODULE_SAIL:          return "CELL_SYSMODULE_SAIL";
    case CELL_SYSMODULE_L10N:          return "CELL_SYSMODULE_L10N";
    case CELL_SYSMODULE_RESC:          return "CELL_SYSMODULE_RESC";
    case CELL_SYSMODULE_SYSUTIL_SAVEDATA: return "CELL_SYSMODULE_SYSUTIL_SAVEDATA";
    case CELL_SYSMODULE_SYSUTIL_GAME:  return "CELL_SYSMODULE_SYSUTIL_GAME";
    case CELL_SYSMODULE_FIBER:         return "CELL_SYSMODULE_FIBER";
    case CELL_SYSMODULE_GEM:           return "CELL_SYSMODULE_GEM";
    case CELL_SYSMODULE_CAMERA:        return "CELL_SYSMODULE_CAMERA";
    case CELL_SYSMODULE_LV2DBG:        return "CELL_SYSMODULE_LV2DBG";
    case CELL_SYSMODULE_SYSUTIL_USERINFO: return "CELL_SYSMODULE_SYSUTIL_USERINFO";
    case CELL_SYSMODULE_SYSUTIL_NP2:   return "CELL_SYSMODULE_SYSUTIL_NP2";
    case CELL_SYSMODULE_SYSUTIL_GAME_EXEC: return "CELL_SYSMODULE_SYSUTIL_GAME_EXEC";
    case CELL_SYSMODULE_SYSUTIL_NP_TROPHY: return "CELL_SYSMODULE_SYSUTIL_NP_TROPHY";
    case CELL_SYSMODULE_MIC:           return "CELL_SYSMODULE_MIC";
    case CELL_SYSMODULE_FONTFT:        return "CELL_SYSMODULE_FONTFT";
    case CELL_SYSMODULE_AVCONF_EXT:    return "CELL_SYSMODULE_AVCONF_EXT";
    case CELL_SYSMODULE_USBPSPCM:      return "CELL_SYSMODULE_USBPSPCM";
    default:                           return "UNKNOWN";
    }
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* NID: 0x26A6E12B */
/* NID: 0x63FF6FF9 -- library init/teardown are no-ops in HLE. */
s32 cellSysmoduleInitialize(void) { return CELL_OK; }
s32 cellSysmoduleFinalize(void)   { return CELL_OK; }

s32 cellSysmoduleLoadModule(u16 id)
{
    printf("[cellSysmodule] LoadModule(id=0x%04X '%s')\n",
           id, sysmodule_id_to_name(id));

    int slot = sysmodule_slot(id);
    if (slot < 0)
        return CELL_SYSMODULE_ERROR_UNKNOWN;

    if (s_module_loaded[slot])
        return CELL_OK;  /* Already loaded — return success (some games treat DUPLICATED as fatal) */

    s_module_loaded[slot] = 1;
    return CELL_OK;
}

/* NID: 0x112A5EE9 */
s32 cellSysmoduleUnloadModule(u16 id)
{
    printf("[cellSysmodule] UnloadModule(id=0x%04X '%s')\n",
           id, sysmodule_id_to_name(id));

    int slot = sysmodule_slot(id);
    if (slot < 0)
        return CELL_SYSMODULE_ERROR_UNKNOWN;

    if (!s_module_loaded[slot])
        return CELL_SYSMODULE_ERROR_UNLOADED;

    s_module_loaded[slot] = 0;
    return CELL_OK;
}

/* NID: 0x5A59E258 */
s32 cellSysmoduleIsLoaded(u16 id)
{
    printf("[cellSysmodule] IsLoaded(id=0x%04X '%s')\n",
           id, sysmodule_id_to_name(id));

    int slot = sysmodule_slot(id);
    if (slot < 0)
        return CELL_SYSMODULE_ERROR_UNKNOWN;

    return s_module_loaded[slot] ? CELL_OK : CELL_SYSMODULE_ERROR_UNLOADED;
}
