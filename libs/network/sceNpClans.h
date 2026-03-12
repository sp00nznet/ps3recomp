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

/* Error codes */
#define SCE_NP_CLANS_ERROR_NOT_INITIALIZED       0x80022301
#define SCE_NP_CLANS_ERROR_ALREADY_INITIALIZED   0x80022302
#define SCE_NP_CLANS_ERROR_INVALID_ARGUMENT      0x80022303
#define SCE_NP_CLANS_ERROR_NOT_CONNECTED         0x80022304
#define SCE_NP_CLANS_ERROR_OUT_OF_MEMORY         0x80022305
#define SCE_NP_CLANS_ERROR_NOT_FOUND             0x80022306

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
