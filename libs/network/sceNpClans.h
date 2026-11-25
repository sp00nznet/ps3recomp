/*
 * ps3recomp - sceNpClans HLE
 *
 * PSN clan system: create/join/leave clans, member management.
 * Offline stub — all operations return NOT_CONNECTED.
 */

#ifndef PS3RECOMP_SCE_NP_CLANS_H
#define PS3RECOMP_SCE_NP_CLANS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (PS3 sceNpClans, base 0x80022700; verified against RPCS3) */
#define SCE_NP_CLANS_ERROR_ALREADY_INITIALIZED   (CELL_ERROR_BASE_NP_CLANS | 0x01) /* 0x80022701 */
#define SCE_NP_CLANS_ERROR_NOT_INITIALIZED       (CELL_ERROR_BASE_NP_CLANS | 0x02) /* 0x80022702 */
#define SCE_NP_CLANS_ERROR_INVALID_ARGUMENT      (CELL_ERROR_BASE_NP_CLANS | 0x03) /* 0x80022703 */
#define SCE_NP_CLANS_ERROR_NOT_CONNECTED         (CELL_ERROR_BASE_NP_CLANS | 0x04) /* 0x80022704 */
#define SCE_NP_CLANS_ERROR_OUT_OF_MEMORY         (CELL_ERROR_BASE_NP_CLANS | 0x05) /* 0x80022705 */
#define SCE_NP_CLANS_ERROR_NOT_FOUND             (CELL_ERROR_BASE_NP_CLANS | 0x06) /* 0x80022706 */

/* Types */
typedef u32 SceNpClansRequestId;
typedef u32 SceNpClansClanId;

typedef struct SceNpClansClanInfo {
    SceNpClansClanId clanId;
    char name[64];
    char tag[16];
    char description[256];
    u32 memberCount;
    u32 reserved[4];
} SceNpClansClanInfo;

typedef void (*SceNpClansCallback)(SceNpClansRequestId reqId,
                                     s32 result, void* arg);

/* Functions */
s32 sceNpClansInit(const void* commId, const void* passPhrase, u32 poolSize);
s32 sceNpClansTerm(void);

s32 sceNpClansCreateClan(const char* name, const char* tag,
                           SceNpClansRequestId* reqId);
s32 sceNpClansLeaveClan(SceNpClansClanId clanId, SceNpClansRequestId* reqId);
s32 sceNpClansJoinClan(SceNpClansClanId clanId, SceNpClansRequestId* reqId);

s32 sceNpClansGetClanInfo(SceNpClansClanId clanId, SceNpClansClanInfo* info,
                            SceNpClansRequestId* reqId);
s32 sceNpClansSearchByName(const char* name, SceNpClansRequestId* reqId);

s32 sceNpClansGetMemberList(SceNpClansClanId clanId, SceNpClansRequestId* reqId);

s32 sceNpClansAbortRequest(SceNpClansRequestId reqId);
s32 sceNpClansCheckCallback(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_CLANS_H */
