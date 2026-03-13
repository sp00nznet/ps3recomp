/*
 * ps3recomp - cellFontFT HLE implementation
 *
 * FreeType-based font rendering. Uses the same fallback metrics strategy as
 * cellFont — if no real font data is loaded, returns reasonable default metrics.
 * Glyph rendering produces empty bitmaps (no real rasterization without FreeType).
 */

#include "cellFontFT.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

#define MAX_FT_FONTS 16

typedef struct {
    int in_use;
    float size;
    /* We don't have actual FreeType, so we store enough to return metrics */
    int from_file;
    char path[256];
} FTFontSlot;

static int s_initialized = 0;
static FTFontSlot s_fonts[MAX_FT_FONTS];

/* API */

s32 cellFontFTInit(const CellFontFTConfig* config, CellFontFTLibrary* lib)
{
    printf("[cellFontFT] Init()\n");
    (void)config;

    if (s_initialized)
        return (s32)CELL_FONTFT_ERROR_ALREADY_INITIALIZED;

    memset(s_fonts, 0, sizeof(s_fonts));
    s_initialized = 1;

    if (lib) *lib = 1;
    return CELL_OK;
}

s32 cellFontFTEnd(CellFontFTLibrary lib)
{
    printf("[cellFontFT] End()\n");
    (void)lib;

    memset(s_fonts, 0, sizeof(s_fonts));
    s_initialized = 0;
    return CELL_OK;
}

static int ft_alloc_slot(void)
{
    for (int i = 0; i < MAX_FT_FONTS; i++) {
        if (!s_fonts[i].in_use) return i;
    }
    return -1;
}

s32 cellFontFTOpenFontFile(CellFontFTLibrary lib, const char* path,
                           u32 index, CellFontFT* font)
{
    (void)lib; (void)index;
    printf("[cellFontFT] OpenFontFile(%s)\n", path ? path : "(null)");

    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;
    if (!path || !font) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    int slot = ft_alloc_slot();
    if (slot < 0) return (s32)CELL_FONTFT_ERROR_OUT_OF_MEMORY;

    s_fonts[slot].in_use = 1;
    s_fonts[slot].size = 16.0f; /* default size */
    s_fonts[slot].from_file = 1;
    strncpy(s_fonts[slot].path, path, sizeof(s_fonts[slot].path) - 1);
    s_fonts[slot].path[sizeof(s_fonts[slot].path) - 1] = '\0';

    memset(font, 0, sizeof(*font));
    font->handle = (u32)slot;
    font->type = 1; /* file-based */
    font->scale = 1.0f;

    return CELL_OK;
}

s32 cellFontFTOpenFontMemory(CellFontFTLibrary lib, const void* data,
                             u32 dataSize, u32 index, CellFontFT* font)
{
    (void)lib; (void)data; (void)dataSize; (void)index;
    printf("[cellFontFT] OpenFontMemory(size=%u)\n", dataSize);

    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;
    if (!data || !font) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    int slot = ft_alloc_slot();
    if (slot < 0) return (s32)CELL_FONTFT_ERROR_OUT_OF_MEMORY;

    s_fonts[slot].in_use = 1;
    s_fonts[slot].size = 16.0f;
    s_fonts[slot].from_file = 0;

    memset(font, 0, sizeof(*font));
    font->handle = (u32)slot;
    font->type = 2; /* memory-based */
    font->scale = 1.0f;

    return CELL_OK;
}

s32 cellFontFTCloseFont(CellFontFT* font)
{
    if (!font) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;
    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;

    u32 h = font->handle;
    if (h >= MAX_FT_FONTS || !s_fonts[h].in_use)
        return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    s_fonts[h].in_use = 0;
    memset(font, 0, sizeof(*font));
    return CELL_OK;
}

s32 cellFontFTSetFontSize(CellFontFT* font, float size)
{
    if (!font) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;
    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;

    u32 h = font->handle;
    if (h >= MAX_FT_FONTS || !s_fonts[h].in_use)
        return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    s_fonts[h].size = size;
    font->scale = size / 16.0f;
    return CELL_OK;
}

s32 cellFontFTGetFontMetrics(const CellFontFT* font, CellFontFTFontMetrics* metrics)
{
    if (!font || !metrics) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;
    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;

    u32 h = font->handle;
    if (h >= MAX_FT_FONTS || !s_fonts[h].in_use)
        return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    float size = s_fonts[h].size;

    /* Fallback metrics based on typical proportions */
    metrics->ascender   = size * 0.8f;
    metrics->descender  = size * -0.2f;
    metrics->lineHeight = size * 1.2f;
    metrics->maxAdvance = size * 0.6f;

    return CELL_OK;
}

s32 cellFontFTGetGlyphMetrics(const CellFontFT* font, u32 charCode,
                              CellFontFTGlyphMetrics* metrics)
{
    if (!font || !metrics) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;
    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;

    u32 h = font->handle;
    if (h >= MAX_FT_FONTS || !s_fonts[h].in_use)
        return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    (void)charCode;
    float size = s_fonts[h].size;

    /* Fallback glyph metrics */
    metrics->width     = size * 0.5f;
    metrics->height    = size * 0.8f;
    metrics->hBearingX = 0.0f;
    metrics->hBearingY = size * 0.8f;
    metrics->hAdvance  = size * 0.6f;
    metrics->vBearingX = size * -0.25f;
    metrics->vBearingY = size * 0.1f;
    metrics->vAdvance  = size * 1.2f;

    return CELL_OK;
}

s32 cellFontFTRenderGlyph(const CellFontFT* font, u32 charCode,
                          CellFontFTGlyphImage* image)
{
    if (!font || !image) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;
    if (!s_initialized) return (s32)CELL_FONTFT_ERROR_NOT_INITIALIZED;

    u32 h = font->handle;
    if (h >= MAX_FT_FONTS || !s_fonts[h].in_use)
        return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;

    (void)charCode;
    float size = s_fonts[h].size;

    /* Produce an empty glyph bitmap (no real rasterization without FreeType) */
    u32 w = (u32)(size * 0.5f);
    u32 hh = (u32)(size * 0.8f);
    if (w < 1) w = 1;
    if (hh < 1) hh = 1;

    image->width  = w;
    image->height = hh;
    image->pitch  = w;
    image->format = 0; /* 8-bit alpha */

    /* If caller provided a buffer, zero it */
    if (image->buffer) {
        memset(image->buffer, 0, (size_t)(w * hh));
    }

    return CELL_OK;
}

s32 cellFontFTGetCharGlyphCode(const CellFontFT* font, u32 charCode, u32* glyphCode)
{
    (void)font;
    if (!glyphCode) return (s32)CELL_FONTFT_ERROR_INVALID_ARGUMENT;
    /* Direct mapping: char code = glyph code (no cmap lookup without FreeType) */
    *glyphCode = charCode;
    return CELL_OK;
}

s32 cellFontFTGetKerning(const CellFontFT* font, u32 leftChar, u32 rightChar,
                         float* kernX, float* kernY)
{
    (void)font; (void)leftChar; (void)rightChar;
    /* No kerning data without FreeType */
    if (kernX) *kernX = 0.0f;
    if (kernY) *kernY = 0.0f;
    return CELL_OK;
}
