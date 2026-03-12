/*
 * ps3recomp - cellRudp HLE
 *
 * Sony's reliable UDP protocol for game networking.
 * Offline stub — context management works, operations return errors.
 */

#ifndef PS3RECOMP_CELL_RUDP_H
#define PS3RECOMP_CELL_RUDP_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_RUDP_ERROR_NOT_INITIALIZED       0x80770001
#define CELL_RUDP_ERROR_ALREADY_INITIALIZED   0x80770002
#define CELL_RUDP_ERROR_INVALID_ARGUMENT      0x80770003
#define CELL_RUDP_ERROR_OUT_OF_MEMORY         0x80770004
#define CELL_RUDP_ERROR_NOT_CONNECTED         0x80770005
#define CELL_RUDP_ERROR_CTX_NOT_FOUND         0x80770006
#define CELL_RUDP_ERROR_TIMEOUT               0x80770007
#define CELL_RUDP_ERROR_WOULDBLOCK            0x80770008

/* Constants */
#define CELL_RUDP_CTX_MAX           8
#define CELL_RUDP_MAX_PAYLOAD       1200

/* Context state */
#define CELL_RUDP_STATE_IDLE        0
#define CELL_RUDP_STATE_CONNECTING  1
#define CELL_RUDP_STATE_CONNECTED   2
#define CELL_RUDP_STATE_CLOSING     3

/* Types */
typedef u32 CellRudpCtxId;

typedef struct CellRudpOption {
    u32 maxRetransmit;
    u32 timeoutMs;
    u32 keepAliveMs;
    u32 reserved[4];
} CellRudpOption;

typedef void (*CellRudpEventHandler)(CellRudpCtxId ctxId, u32 event,
                                       s32 errorCode, void* arg);

/* Functions */
s32 cellRudpInit(u32 maxContexts);
s32 cellRudpEnd(void);

s32 cellRudpCreateContext(const CellRudpOption* option,
                            CellRudpEventHandler handler,
                            void* arg, CellRudpCtxId* ctxId);
s32 cellRudpDestroyContext(CellRudpCtxId ctxId);

s32 cellRudpBind(CellRudpCtxId ctxId, u16 port);
s32 cellRudpConnect(CellRudpCtxId ctxId, u32 addr, u16 port);
s32 cellRudpClose(CellRudpCtxId ctxId);

s32 cellRudpSend(CellRudpCtxId ctxId, const void* data, u32 size);
s32 cellRudpRecv(CellRudpCtxId ctxId, void* data, u32 size, u32* received);

s32 cellRudpGetState(CellRudpCtxId ctxId, s32* state);
s32 cellRudpFlush(CellRudpCtxId ctxId);
s32 cellRudpPoll(CellRudpCtxId ctxId, s32 events, s32* revents, u32 timeoutMs);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_RUDP_H */
