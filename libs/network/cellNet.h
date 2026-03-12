/*
 * ps3recomp - cellNet HLE
 *
 * Network core: initialization, memory pool management, and
 * network thread control. Required before using any network module.
 */

#ifndef PS3RECOMP_CELL_NET_H
#define PS3RECOMP_CELL_NET_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_NET_ERROR_NOT_INITIALIZED       0x80130101
#define CELL_NET_ERROR_ALREADY_INITIALIZED   0x80130102
#define CELL_NET_ERROR_INVALID_ARG           0x80130103
#define CELL_NET_ERROR_NO_MEMORY             0x80130104

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

typedef struct CellNetCtlInitParam {
    u32 size;
    u32 flags;
} CellNetCtlInitParam;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* Core network init/term */
s32 cellNetCtlInit(void);
s32 cellNetCtlTerm(void);

/* Network pool management */
s32 sys_net_initialize_network_ex(u32 poolSize);
s32 sys_net_finalize_network(void);

/* Cell network init (high-level) */
s32 cellNetInitialize(void);
s32 cellNetFinalize(void);

/* DNS resolver */
s32 cellNetResolverCreate(u32* resolver_id);
s32 cellNetResolverDestroy(u32 resolver_id);
s32 cellNetResolverStartAsynDNS(u32 resolver_id, const char* hostname,
                                  u32* addr, u32 timeout);
s32 cellNetResolverPollDNS(u32 resolver_id, s32* result);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_NET_H */
