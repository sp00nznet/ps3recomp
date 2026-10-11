/*
 * ps3recomp - SPU images: parsing an SPU ELF into segments, the lv2 kernel's
 * image objects (_sys_spu_image_import / close / get_segments), and loading a
 * segment list into a local store.
 *
 * sys_spu_image   { u32 type; u32 entry_point; u32 segs (EA); s32 nsegs; }
 *   type 0 (USER):   entry and the segment list are in the descriptor
 *   type 1 (KERNEL): entry_point is a kernel image object id; segs/nsegs 0
 * sys_spu_segment { s32 type; u32 ls; u32 size; union { u32 addr; u64 pad; }; }
 *   (0x18; the union is 8-aligned, so addr is the u32 at +0x10 and +0x14 is 0)
 *   type 1 COPY (addr = source EA), 2 FILL (addr = 32-bit fill pattern),
 *   4 INFO
 * Layouts as liblv2 writes them (tests/conformance/spurs/t_sysprx).
 */
#ifndef PS3RECOMP_LV2_SPU_IMAGE_H
#define PS3RECOMP_LV2_SPU_IMAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_SPU_SEG_COPY 1u
#define LV2_SPU_SEG_FILL 2u
#define LV2_SPU_SEG_INFO 4u
#define LV2_SPU_MAX_SEGS 32

typedef struct lv2_spu_seg_s { uint32_t type, ls, size, addr; } lv2_spu_seg;

/* Parse the SPU ELF at guest `src` (bounded by `limit` bytes, 0 = no bound)
 * into segments: one COPY per loadable segment, followed by a FILL for its
 * zero-filled tail. Returns the count, or a negative CELL error (ENOEXEC for
 * anything that is not an SPU ELF, an SCE-wrapped one included). */
int32_t lv2_spu_elf_segments(uint32_t src, uint32_t limit, lv2_spu_seg* out, int max,
                             uint32_t* entry);
/* Bytes of the ELF its loadable segments span (offset + filesz, maximum). */
uint32_t lv2_spu_elf_span(uint32_t src);

/* The kernel side (syscalls 157 / 158 / 159). */
int32_t lv2_spu_image_import(uint32_t img, uint32_t src, uint32_t size, uint32_t arg);
int32_t lv2_spu_image_close(uint32_t img);
int32_t lv2_spu_image_get_segments(uint32_t img, uint32_t segs, int32_t nseg);

/* Resolve a descriptor (either type) to its entry, a copy of its segments and
 * the EA of the ELF it came from (0 if unknown). Returns 0, or a CELL error
 * (ESRCH: no such kernel image; EINVAL: malformed). */
int32_t lv2_spu_image_resolve(uint32_t img, uint32_t* entry, lv2_spu_seg* segs, uint32_t* nsegs,
                              uint32_t* src);

/* liblv2's sys_spu_image_import remembers which ELF a USER descriptor was
 * parsed from (the lifted-code registry is keyed by the ELF's bytes). */
void     lv2_spu_image_note_source(uint32_t img, uint32_t src);
uint32_t lv2_spu_image_source(uint32_t img);

/* Deploy segments into a 256 KB local store. */
void lv2_spu_load_segments(const lv2_spu_seg* segs, uint32_t nsegs, uint8_t* ls);

#ifdef __cplusplus
}
#endif

#endif
