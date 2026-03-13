/*
 * ps3recomp - cellFreeType HLE
 *
 * Low-level FreeType2 library wrapper. Provides raw FreeType API access.
 * Stub — init/end lifecycle, library version query.
 */

#ifndef PS3RECOMP_CELL_FREETYPE_H
#define PS3RECOMP_CELL_FREETYPE_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_FREETYPE_ERROR_NOT_INITIALIZED     0x80540101
#define CELL_FREETYPE_ERROR_ALREADY_INITIALIZED 0x80540102
#define CELL_FREETYPE_ERROR_INVALID_ARGUMENT    0x80540103
#define CELL_FREETYPE_ERROR_NOT_SUPPORTED       0x80540104
#define CELL_FREETYPE_ERROR_OUT_OF_MEMORY       0x80540105

/* FreeType library handle */
typedef u32 CellFreeTypeLibrary;

/* Config */
typedef struct CellFreeTypeConfig {
    void* buffer;
    u32 bufferSize;
    u32 flags;
    u32 reserved[4];
} CellFreeTypeConfig;

/* Version info */
typedef struct CellFreeTypeVersion {
    s32 major;
    s32 minor;
    s32 patch;
} CellFreeTypeVersion;

/* Functions */
s32 cellFreeTypeInit(const CellFreeTypeConfig* config, CellFreeTypeLibrary* lib);
s32 cellFreeTypeEnd(CellFreeTypeLibrary lib);
s32 cellFreeTypeGetVersion(CellFreeTypeVersion* version);
s32 cellFreeTypeGetLibrary(CellFreeTypeLibrary* lib);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_FREETYPE_H */
