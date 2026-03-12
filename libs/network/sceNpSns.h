/*
 * ps3recomp - sceNpSns HLE
 *
 * Social networking service integration (Facebook, Twitter).
 * Offline stub — all operations return NOT_CONNECTED.
 */

#ifndef PS3RECOMP_SCE_NP_SNS_H
#define PS3RECOMP_SCE_NP_SNS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define SCE_NP_SNS_ERROR_NOT_INITIALIZED     0x80024501
#define SCE_NP_SNS_ERROR_ALREADY_INITIALIZED 0x80024502
#define SCE_NP_SNS_ERROR_INVALID_ARGUMENT    0x80024503
#define SCE_NP_SNS_ERROR_NOT_CONNECTED       0x80024504
#define SCE_NP_SNS_ERROR_NOT_SUPPORTED       0x80024505
#define SCE_NP_SNS_ERROR_OUT_OF_MEMORY       0x80024506

/* SNS Provider IDs */
#define SCE_NP_SNS_FACEBOOK   1
#define SCE_NP_SNS_TWITTER    2

/* Types */
typedef u32 SceNpSnsAccountId;

typedef struct SceNpSnsFbFeedMessage {
    char message[512];
    char linkUrl[256];
    char linkTitle[128];
    char linkDesc[256];
    char imageUrl[256];
} SceNpSnsFbFeedMessage;

/* Functions */
s32 sceNpSnsInit(u32 poolSize);
s32 sceNpSnsTerm(void);

s32 sceNpSnsFbCheckThisUserAccount(SceNpSnsAccountId* accountId);
s32 sceNpSnsFbGetLongAccessToken(char* token, u32 tokenSize);
s32 sceNpSnsFbPostFeedMessage(const SceNpSnsFbFeedMessage* message);

s32 sceNpSnsCheckCallback(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_SNS_H */
