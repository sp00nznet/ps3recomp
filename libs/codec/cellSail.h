/*
 * ps3recomp - cellSail HLE
 *
 * High-level media player wrapping PAMF/demux/decode pipeline.
 * Stub — player lifecycle tracked, no actual media playback.
 */

#ifndef PS3RECOMP_CELL_SAIL_H
#define PS3RECOMP_CELL_SAIL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_SAIL_ERROR_NOT_INITIALIZED       0x80610701
#define CELL_SAIL_ERROR_ALREADY_INITIALIZED   0x80610702
#define CELL_SAIL_ERROR_INVALID_ARGUMENT      0x80610703
#define CELL_SAIL_ERROR_INVALID_STATE         0x80610704
#define CELL_SAIL_ERROR_NOT_SUPPORTED         0x80610705
#define CELL_SAIL_ERROR_NOT_FOUND             0x80610706
#define CELL_SAIL_ERROR_OUT_OF_MEMORY         0x80610707

/* Constants */
#define CELL_SAIL_PLAYER_MAX    4

/* Player state */
#define CELL_SAIL_PLAYER_STATE_CLOSED     0
#define CELL_SAIL_PLAYER_STATE_INITIALIZED 1
#define CELL_SAIL_PLAYER_STATE_BOOT_TRANSITION 2
#define CELL_SAIL_PLAYER_STATE_RUNNING    3
#define CELL_SAIL_PLAYER_STATE_PAUSE      4
#define CELL_SAIL_PLAYER_STATE_FINISHED   5

/* Stream types */
#define CELL_SAIL_STREAM_VIDEO   0
#define CELL_SAIL_STREAM_AUDIO   1
#define CELL_SAIL_STREAM_USER    2

/* Types */
typedef u32 CellSailPlayerHandle;

typedef struct CellSailPlayerAttribute {
    s32 threadPriority;
    u32 threadStackSize;
    u32 reserved[4];
} CellSailPlayerAttribute;

typedef struct CellSailPlayerResource {
    void* memAddr;
    u32 memSize;
    u32 reserved;
} CellSailPlayerResource;

typedef struct CellSailStreamInfo {
    u32 type;
    u32 codecType;
    u32 width;
    u32 height;
    u32 sampleRate;
    u32 channels;
    u32 reserved[4];
} CellSailStreamInfo;

typedef void (*CellSailPlayerCallback)(CellSailPlayerHandle handle,
                                         s32 event, s32 param,
                                         void* arg);

/* Functions */
s32 cellSailInit(void);
s32 cellSailTerm(void);

s32 cellSailPlayerCreate(const CellSailPlayerAttribute* attr,
                           const CellSailPlayerResource* resource,
                           CellSailPlayerCallback callback,
                           void* callbackArg,
                           CellSailPlayerHandle* handle);
s32 cellSailPlayerDestroy(CellSailPlayerHandle handle);

s32 cellSailPlayerBoot(CellSailPlayerHandle handle, u64 userParam);
s32 cellSailPlayerOpenStream(CellSailPlayerHandle handle, const char* path);
s32 cellSailPlayerCloseStream(CellSailPlayerHandle handle);

s32 cellSailPlayerStart(CellSailPlayerHandle handle);
s32 cellSailPlayerStop(CellSailPlayerHandle handle);
s32 cellSailPlayerPause(CellSailPlayerHandle handle);

s32 cellSailPlayerGetState(CellSailPlayerHandle handle, s32* state);
s32 cellSailPlayerGetStreamNum(CellSailPlayerHandle handle, u32* streamNum);
s32 cellSailPlayerGetStreamInfo(CellSailPlayerHandle handle, u32 streamIndex,
                                  CellSailStreamInfo* info);

s32 cellSailPlayerSetSoundAdapter(CellSailPlayerHandle handle, u32 index);
s32 cellSailPlayerSetGraphicsAdapter(CellSailPlayerHandle handle, u32 index);

s32 cellSailPlayerCancel(CellSailPlayerHandle handle);
s32 cellSailPlayerIsPaused(CellSailPlayerHandle handle);
s32 cellSailPlayerSetRepeatMode(CellSailPlayerHandle handle, s32 repeatMode);

/* Descriptor management (for multi-stream sources) */
typedef u32 CellSailDescriptorHandle;

s32 cellSailPlayerCreateDescriptor(CellSailPlayerHandle handle,
                                     s32 streamType, void* arg,
                                     CellSailDescriptorHandle* desc);
s32 cellSailPlayerDestroyDescriptor(CellSailPlayerHandle handle,
                                      CellSailDescriptorHandle desc);
s32 cellSailPlayerAddDescriptor(CellSailPlayerHandle handle,
                                  CellSailDescriptorHandle desc);
s32 cellSailPlayerRemoveDescriptor(CellSailPlayerHandle handle,
                                     CellSailDescriptorHandle desc);

s32 cellSailDescriptorCreateDatabase(CellSailDescriptorHandle desc,
                                       void* dbAddr, u32 dbSize,
                                       u64 arg);
s32 cellSailDescriptorDestroyDatabase(CellSailDescriptorHandle desc);
s32 cellSailDescriptorGetStreamType(CellSailDescriptorHandle desc, s32* type);
s32 cellSailDescriptorSetAutoSelection(CellSailDescriptorHandle desc, s32 enable);
s32 cellSailDescriptorGetUri(CellSailDescriptorHandle desc, char* uri, u32 maxLen);

/* Memory allocator */
typedef struct CellSailMemAllocator {
    void* (*alloc)(void* arg, u32 boundary, u32 size);
    void  (*free)(void* arg, u32 boundary, void* ptr);
    void* arg;
} CellSailMemAllocator;

s32 cellSailMemAllocatorInitialize(CellSailMemAllocator* allocator,
                                     void* (*allocFunc)(void*, u32, u32),
                                     void  (*freeFunc)(void*, u32, void*),
                                     void* arg);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SAIL_H */
