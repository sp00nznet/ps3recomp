/*
 * ps3recomp - cellFont HLE
 *
 * Font system: TTF loading, glyph rendering, metrics.
 * Uses stb_truetype.h if available (place in include/).
 */

#ifndef PS3RECOMP_CELL_FONT_H
#define PS3RECOMP_CELL_FONT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/

#define CELL_FONT_ERROR_FATAL               (CELL_ERROR_BASE_FONT | 0x01)
#define CELL_FONT_ERROR_INVALID_PARAMETER   (CELL_ERROR_BASE_FONT | 0x02)
#define CELL_FONT_ERROR_UNINITIALIZED       (CELL_ERROR_BASE_FONT | 0x03)
#define CELL_FONT_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_FONT | 0x04)
#define CELL_FONT_ERROR_ALLOCATION_FAILED   (CELL_ERROR_BASE_FONT | 0x05)
#define CELL_FONT_ERROR_NO_SUPPORT_FUNCTION (CELL_ERROR_BASE_FONT | 0x06)
#define CELL_FONT_ERROR_OPEN_FAILED         (CELL_ERROR_BASE_FONT | 0x07)
#define CELL_FONT_ERROR_NOT_FOUND           (CELL_ERROR_BASE_FONT | 0x08)
#define CELL_FONT_ERROR_FILE_DECOMPRESSION  (CELL_ERROR_BASE_FONT | 0x09)
#define CELL_FONT_ERROR_NO_SUPPORT_FONTSET  (CELL_ERROR_BASE_FONT | 0x0A)
#define CELL_FONT_ERROR_RENDERER            (CELL_ERROR_BASE_FONT | 0x10)
#define CELL_FONT_ERROR_RENDERER_ALREADY_BIND (CELL_ERROR_BASE_FONT | 0x11)
#define CELL_FONT_ERROR_RENDERER_UNBIND     (CELL_ERROR_BASE_FONT | 0x12)

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define CELL_FONT_MAX_OPENED        8
#define CELL_FONT_MAX_RENDERERS     4

/* Font type constants */
#define CELL_FONT_TYPE_RODIN_SANS_SERIF_LATIN      0x00000000
#define CELL_FONT_TYPE_RODIN_SANS_SERIF_LIGHT_LATIN 0x00000001
#define CELL_FONT_TYPE_RODIN_SANS_SERIF_BOLD_LATIN  0x00000002
#define CELL_FONT_TYPE_RODIN_SANS_SERIF_LATIN2     0x00000018
#define CELL_FONT_TYPE_SEURAT_MARU_GOTHIC_LATIN    0x00000100
#define CELL_FONT_TYPE_SEURAT_MARU_GOTHIC_LATIN2   0x00000118

/* Open mode flags */
#define CELL_FONT_OPEN_MODE_DEFAULT         0

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/* Opaque library handle */
typedef u32 CellFontLibrary;

/* Font handle */
typedef struct CellFont {
    u32 handle;         /* internal index */
    u32 type;           /* font type constant */
    float scale_x;
    float scale_y;
} CellFont;

/* Font type descriptor */
typedef struct CellFontType {
    u32 type;
    u32 map;
} CellFontType;

/* Glyph metrics */
typedef struct CellFontGlyphMetrics {
    float width;
    float height;
    float h_bearing_x;
    float h_bearing_y;
    float h_advance;
    float v_bearing_x;
    float v_bearing_y;
    float v_advance;
} CellFontGlyphMetrics;

/* Horizontal layout info */
typedef struct CellFontHorizontalLayout {
    float baseLineY;
    float lineHeight;
    float effectHeight;
} CellFontHorizontalLayout;

/* Render surface (destination for glyph rendering) */
typedef struct CellFontRenderSurface {
    u8*  buffer;        /* pixel buffer */
    s32  widthByte;     /* bytes per row */
    s32  pixelSizeByte; /* bytes per pixel (1 for alpha-only) */
    s32  width;         /* surface width in pixels */
    s32  height;        /* surface height in pixels */
} CellFontRenderSurface;

/* Image glyph output */
typedef struct CellFontGlyphImage {
    u8*  buffer;
    s32  widthByte;
    s32  pixelSizeByte;
    s32  width;
    s32  height;
} CellFontGlyphImage;

/* Renderer handle */
typedef u32 CellFontRenderer;

/* Init configuration */
typedef struct CellFontConfig {
    u32  fc_size;       /* buffer size for font cache */
    void* FileCache;
    u32  userFontEntryMax;
} CellFontConfig;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellFontInit(const CellFontConfig* config);
s32 cellFontEnd(void);

s32 cellFontOpenFontFile(CellFont* font, const char* fontPath, u32 subNum, s32 uniqueId);
s32 cellFontOpenFontMemory(CellFont* font, const void* data, u32 dataSize, u32 subNum, s32 uniqueId);
s32 cellFontOpenFontset(CellFontLibrary lib, CellFontType* fontType, CellFont* font);
s32 cellFontCloseFont(CellFont* font);

s32 cellFontSetFontOpenMode(u32 openMode);

s32 cellFontCreateRenderer(CellFontLibrary lib, CellFontRenderer* renderer);
s32 cellFontDestroyRenderer(CellFontRenderer renderer);
s32 cellFontBindRenderer(CellFont* font, CellFontRenderer renderer);
s32 cellFontUnbindRenderer(CellFont* font);

s32 cellFontSetScalePixel(CellFont* font, float w, float h);
s32 cellFontSetEffectWeight(CellFont* font, float weight);
s32 cellFontSetEffectSlant(CellFont* font, float slant);

s32 cellFontGetHorizontalLayout(CellFont* font, CellFontHorizontalLayout* layout);

s32 cellFontRenderCharGlyphImage(CellFont* font, u32 code,
                                  CellFontRenderSurface* surface,
                                  float x, float y,
                                  CellFontGlyphMetrics* metrics,
                                  CellFontGlyphImage* image);

s32 cellFontGetRenderCharGlyphMetrics(CellFont* font, u32 code,
                                       CellFontGlyphMetrics* metrics);

s32 cellFontGetRenderCharGlyphMetricsVertical(CellFont* font, u32 code,
                                               CellFontGlyphMetrics* metrics);

s32 cellFontSetupRenderScalePixel(CellFont* font, float w, float h);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_FONT_H */
