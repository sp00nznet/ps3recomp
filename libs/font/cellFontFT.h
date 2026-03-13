/*
 * ps3recomp - cellFontFT HLE
 *
 * FreeType-based font rendering. Alternative to cellFont's stb_truetype backend.
 * Stub — wraps cellFont functions since both serve the same purpose.
 * Games typically use one or the other, not both.
 */

#ifndef PS3RECOMP_CELL_FONTFT_H
#define PS3RECOMP_CELL_FONTFT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (shared with cellFont) */
#define CELL_FONTFT_ERROR_NOT_INITIALIZED     0x80540001
#define CELL_FONTFT_ERROR_ALREADY_INITIALIZED 0x80540002
#define CELL_FONTFT_ERROR_INVALID_ARGUMENT    0x80540003
#define CELL_FONTFT_ERROR_FONT_NOT_FOUND      0x80540004
#define CELL_FONTFT_ERROR_INVALID_FONT_DATA   0x80540005
#define CELL_FONTFT_ERROR_OUT_OF_MEMORY       0x80540006

/* FreeType library handle */
typedef u32 CellFontFTLibrary;

/* Config */
typedef struct CellFontFTConfig {
    void* buffer;
    u32 bufferSize;
    u32 flags;
    u32 reserved[4];
} CellFontFTConfig;

/* Font handle */
typedef struct CellFontFT {
    u32 handle;
    u32 type;
    float scale;
    u32 reserved[4];
} CellFontFT;

/* Glyph metrics */
typedef struct CellFontFTGlyphMetrics {
    float width;
    float height;
    float hBearingX;
    float hBearingY;
    float hAdvance;
    float vBearingX;
    float vBearingY;
    float vAdvance;
} CellFontFTGlyphMetrics;

/* Font metrics */
typedef struct CellFontFTFontMetrics {
    float ascender;
    float descender;
    float lineHeight;
    float maxAdvance;
} CellFontFTFontMetrics;

/* Glyph image */
typedef struct CellFontFTGlyphImage {
    void* buffer;
    u32 width;
    u32 height;
    u32 pitch;
    u32 format;
} CellFontFTGlyphImage;

/* Functions */
s32 cellFontFTInit(const CellFontFTConfig* config, CellFontFTLibrary* lib);
s32 cellFontFTEnd(CellFontFTLibrary lib);

s32 cellFontFTOpenFontFile(CellFontFTLibrary lib, const char* path,
                           u32 index, CellFontFT* font);
s32 cellFontFTOpenFontMemory(CellFontFTLibrary lib, const void* data,
                             u32 dataSize, u32 index, CellFontFT* font);
s32 cellFontFTCloseFont(CellFontFT* font);

s32 cellFontFTSetFontSize(CellFontFT* font, float size);
s32 cellFontFTGetFontMetrics(const CellFontFT* font, CellFontFTFontMetrics* metrics);
s32 cellFontFTGetGlyphMetrics(const CellFontFT* font, u32 charCode,
                              CellFontFTGlyphMetrics* metrics);
s32 cellFontFTRenderGlyph(const CellFontFT* font, u32 charCode,
                          CellFontFTGlyphImage* image);

s32 cellFontFTGetCharGlyphCode(const CellFontFT* font, u32 charCode, u32* glyphCode);
s32 cellFontFTGetKerning(const CellFontFT* font, u32 leftChar, u32 rightChar,
                         float* kernX, float* kernY);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_FONTFT_H */
