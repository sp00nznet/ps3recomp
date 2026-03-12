/*
 * ps3recomp - cellUsbd HLE
 *
 * USB device access. Stub — init/term work, device enumeration
 * returns empty, transfers return NOT_FOUND.
 */

#ifndef PS3RECOMP_CELL_USBD_H
#define PS3RECOMP_CELL_USBD_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_USBD_ERROR_NOT_INITIALIZED     0x80110001
#define CELL_USBD_ERROR_ALREADY_INITIALIZED 0x80110002
#define CELL_USBD_ERROR_INVALID_ARGUMENT    0x80110003
#define CELL_USBD_ERROR_NO_DEVICE           0x80110004
#define CELL_USBD_ERROR_PIPE_NOT_FOUND      0x80110005
#define CELL_USBD_ERROR_TRANSFER            0x80110006

/* Constants */
#define CELL_USBD_MAX_PIPES    16

/* Transfer types */
#define CELL_USBD_TRANSFER_CONTROL     0
#define CELL_USBD_TRANSFER_ISOCHRONOUS 1
#define CELL_USBD_TRANSFER_BULK        2
#define CELL_USBD_TRANSFER_INTERRUPT   3

/* Types */
typedef u32 CellUsbdDeviceId;
typedef u32 CellUsbdPipeId;

typedef struct CellUsbdDeviceInfo {
    CellUsbdDeviceId deviceId;
    u16 vendorId;
    u16 productId;
    u8  deviceClass;
    u8  deviceSubClass;
    u8  deviceProtocol;
    u8  reserved;
} CellUsbdDeviceInfo;

typedef void (*CellUsbdLddOpsAttachCb)(CellUsbdDeviceId deviceId);
typedef void (*CellUsbdLddOpsDetachCb)(CellUsbdDeviceId deviceId);

typedef struct CellUsbdLddOps {
    const char* name;
    CellUsbdLddOpsAttachCb attach;
    CellUsbdLddOpsDetachCb detach;
} CellUsbdLddOps;

/* Functions */
s32 cellUsbdInit(void);
s32 cellUsbdEnd(void);

s32 cellUsbdRegisterLdd(const CellUsbdLddOps* ops);
s32 cellUsbdUnregisterLdd(const CellUsbdLddOps* ops);

s32 cellUsbdGetDeviceList(CellUsbdDeviceInfo* list, u32 maxDevices,
                            u32* numDevices);

s32 cellUsbdOpenPipe(CellUsbdDeviceId deviceId, u32 endpoint,
                       CellUsbdPipeId* pipeId);
s32 cellUsbdClosePipe(CellUsbdPipeId pipeId);

s32 cellUsbdBulkTransfer(CellUsbdPipeId pipeId, void* data,
                           u32 length, u32* transferred);
s32 cellUsbdControlTransfer(CellUsbdPipeId pipeId, u8 requestType,
                              u8 request, u16 value, u16 index,
                              void* data, u32 length, u32* transferred);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_USBD_H */
