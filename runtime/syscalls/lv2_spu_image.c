/*
 * ps3recomp - SPU images (see lv2_spu_image.h).
 */
#include "lv2_spu_image.h"
#include "../../include/ps3emu/error_codes.h"
#include "../platform/win32_compat.h"
#include <string.h>

extern uint8_t* vm_base;
extern void spu_raw_note_image(uint32_t src_ea, uint32_t entry);  /* runtime/spu/spu_raw.c */

static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t rd16(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint16_t)(p[0] << 8 | p[1]); }
static void     wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

int32_t lv2_spu_elf_segments(uint32_t src, uint32_t limit, lv2_spu_seg* out, int max, uint32_t* entry)
{
    if (!src) return (int32_t)CELL_EFAULT;
    const uint8_t* e = vm_base + src;
    if (!(e[0] == 0x7F && e[1] == 'E' && e[2] == 'L' && e[3] == 'F') ||
        e[4] != 1 || e[5] != 2 || rd16(src + 0x12) != 23)          /* ELF32, big-endian, EM_SPU */
        return (int32_t)CELL_ENOEXEC;
    const uint32_t phoff = rd32(src + 0x1C);
    const uint16_t phentsize = rd16(src + 0x2A), phnum = rd16(src + 0x2C);
    if (!phnum || phentsize < 0x20) return (int32_t)CELL_ENOEXEC;
    if (limit && phoff + (uint32_t)phnum * phentsize > limit) return (int32_t)CELL_ENOEXEC;
    int n = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint32_t ph = src + phoff + (uint32_t)i * phentsize;
        if (rd32(ph) != 1) continue;                                /* PT_LOAD */
        const uint32_t off = rd32(ph + 0x04), va = rd32(ph + 0x08);
        const uint32_t filesz = rd32(ph + 0x10), memsz = rd32(ph + 0x14);
        if (limit && off + filesz > limit) return (int32_t)CELL_ENOEXEC;
        if (filesz) {
            if (n >= max) return (int32_t)CELL_ENOEXEC;
            out[n++] = (lv2_spu_seg){ LV2_SPU_SEG_COPY, va, filesz, src + off };
        }
        if (memsz > filesz) {
            if (n >= max) return (int32_t)CELL_ENOEXEC;
            out[n++] = (lv2_spu_seg){ LV2_SPU_SEG_FILL, va + filesz, memsz - filesz, 0 };
        }
    }
    if (entry) *entry = rd32(src + 0x18);
    return n;
}

uint32_t lv2_spu_elf_span(uint32_t src)
{
    const uint32_t phoff = rd32(src + 0x1C);
    const uint16_t phentsize = rd16(src + 0x2A), phnum = rd16(src + 0x2C);
    uint32_t span = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint32_t ph = src + phoff + (uint32_t)i * phentsize;
        const uint32_t end = rd32(ph + 0x04) + rd32(ph + 0x10);
        if (end > span) span = end;
    }
    return span;
}

/* ---- kernel image objects ---------------------------------------------------- */

#define MAX_IMAGES 64
static struct {
    uint32_t    id;            /* 0 = free */
    uint32_t    entry, nsegs, src;
    lv2_spu_seg segs[LV2_SPU_MAX_SEGS];
} s_img[MAX_IMAGES];
static uint32_t s_next_id = 0x22000100u;
static SRWLOCK s_lock = SRWLOCK_INIT;

int32_t lv2_spu_image_import(uint32_t img, uint32_t src, uint32_t size, uint32_t arg)
{
    (void)arg;
    if (!img || !src) return (int32_t)CELL_EFAULT;
    lv2_spu_seg segs[LV2_SPU_MAX_SEGS];
    uint32_t entry = 0;
    const int32_t n = lv2_spu_elf_segments(src, size, segs, LV2_SPU_MAX_SEGS, &entry);
    if (n < 0) return n;
    spu_raw_note_image(src, entry);           /* a raw SPU is identified by its image */
    AcquireSRWLockExclusive(&s_lock);
    int k = 0;
    while (k < MAX_IMAGES && s_img[k].id) k++;
    if (k == MAX_IMAGES) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_ENOMEM; }
    s_img[k].id = s_next_id++;
    s_img[k].entry = entry;
    s_img[k].nsegs = (uint32_t)n;
    s_img[k].src = src;
    memcpy(s_img[k].segs, segs, sizeof(lv2_spu_seg) * (size_t)n);
    const uint32_t id = s_img[k].id;
    ReleaseSRWLockExclusive(&s_lock);
    wr32(img + 0x0, 1);                       /* SYS_SPU_IMAGE_TYPE_KERNEL */
    wr32(img + 0x4, id);
    wr32(img + 0x8, 0);
    wr32(img + 0xC, 0);
    return CELL_OK;
}

static int find_image(uint32_t id)
{
    for (int k = 0; k < MAX_IMAGES; k++)
        if (id && s_img[k].id == id) return k;
    return -1;
}

int32_t lv2_spu_image_close(uint32_t img)
{
    if (!img) return (int32_t)CELL_EFAULT;
    if (rd32(img) != 1) return (int32_t)CELL_EINVAL;
    AcquireSRWLockExclusive(&s_lock);
    const int k = find_image(rd32(img + 4));
    if (k >= 0) s_img[k].id = 0;
    ReleaseSRWLockExclusive(&s_lock);
    return k >= 0 ? CELL_OK : (int32_t)CELL_ESRCH;
}

int32_t lv2_spu_image_get_segments(uint32_t img, uint32_t segs, int32_t nseg)
{
    if (!img || !segs) return (int32_t)CELL_EFAULT;
    if (rd32(img) != 1) return (int32_t)CELL_EINVAL;
    AcquireSRWLockExclusive(&s_lock);
    const int k = find_image(rd32(img + 4));
    if (k < 0) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_ESRCH; }
    for (int32_t i = 0; i < nseg && (uint32_t)i < s_img[k].nsegs; i++) {
        const uint32_t s = segs + 0x18u * (uint32_t)i;
        wr32(s + 0x00, s_img[k].segs[i].type);
        wr32(s + 0x04, s_img[k].segs[i].ls);
        wr32(s + 0x08, s_img[k].segs[i].size);
        wr32(s + 0x0C, 0);
        wr32(s + 0x10, s_img[k].segs[i].addr);
        wr32(s + 0x14, 0);
    }
    ReleaseSRWLockExclusive(&s_lock);
    return CELL_OK;
}

/* ---- descriptors ------------------------------------------------------------------ */

#define MAX_SOURCES 64
static struct { uint32_t img, src; } s_src[MAX_SOURCES];

void lv2_spu_image_note_source(uint32_t img, uint32_t src)
{
    AcquireSRWLockExclusive(&s_lock);
    int free_slot = -1;
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (s_src[i].img == img) { s_src[i].src = src; ReleaseSRWLockExclusive(&s_lock); return; }
        if (!s_src[i].img && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) { s_src[free_slot].img = img; s_src[free_slot].src = src; }
    ReleaseSRWLockExclusive(&s_lock);
}

uint32_t lv2_spu_image_source(uint32_t img)
{
    uint32_t src = 0;
    AcquireSRWLockExclusive(&s_lock);
    for (int i = 0; i < MAX_SOURCES; i++)
        if (s_src[i].img == img) { src = s_src[i].src; break; }
    ReleaseSRWLockExclusive(&s_lock);
    return src;
}

int32_t lv2_spu_image_resolve(uint32_t img, uint32_t* entry, lv2_spu_seg* segs, uint32_t* nsegs,
                              uint32_t* src)
{
    if (!img) return (int32_t)CELL_EFAULT;
    const uint32_t type = rd32(img);
    if (type == 1) {
        AcquireSRWLockExclusive(&s_lock);
        const int k = find_image(rd32(img + 4));
        if (k < 0) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_ESRCH; }
        *entry = s_img[k].entry;
        *nsegs = s_img[k].nsegs;
        memcpy(segs, s_img[k].segs, sizeof(lv2_spu_seg) * s_img[k].nsegs);
        *src = s_img[k].src;
        ReleaseSRWLockExclusive(&s_lock);
        return CELL_OK;
    }
    if (type != 0) return (int32_t)CELL_EINVAL;
    const uint32_t e = rd32(img + 4), list = rd32(img + 8);
    const int32_t n = (int32_t)rd32(img + 12);
    if (e > 0x3FFFC || n <= 0 || n > LV2_SPU_MAX_SEGS) return (int32_t)CELL_EINVAL;
    for (int32_t i = 0; i < n; i++) {
        const uint32_t s = list + 0x18u * (uint32_t)i;
        segs[i] = (lv2_spu_seg){ rd32(s), rd32(s + 4), rd32(s + 8), rd32(s + 0x10) };
    }
    *entry = e;
    *nsegs = (uint32_t)n;
    *src = lv2_spu_image_source(img);
    return CELL_OK;
}

void lv2_spu_load_segments(const lv2_spu_seg* segs, uint32_t nsegs, uint8_t* ls)
{
    for (uint32_t i = 0; i < nsegs; i++) {
        const uint32_t at = segs[i].ls & 0x3FFFF;
        uint32_t size = segs[i].size;
        if (at + size > 0x40000) size = 0x40000 - at;
        if (segs[i].type == LV2_SPU_SEG_COPY) {
            memcpy(ls + at, vm_base + segs[i].addr, size);
        } else if (segs[i].type == LV2_SPU_SEG_FILL) {
            const uint32_t v = segs[i].addr;
            for (uint32_t o = 0; o + 4 <= size; o += 4) {
                ls[at + o] = v >> 24; ls[at + o + 1] = v >> 16; ls[at + o + 2] = v >> 8; ls[at + o + 3] = v;
            }
        }
    }
}
