#ifndef PS3RECOMP_PPU_FS_MEMORY_H
#define PS3RECOMP_PPU_FS_MEMORY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional host VM policy, installed before guest threads start. It may commit
 * a valid reserved guest page, but must never change committed protections. */
typedef int (*ppu_fs_guest_page_preparer)(uint32_t guest_address);
void ppu_fs_set_guest_page_preparer(ppu_fs_guest_page_preparer prepare);
#ifdef __cplusplus
}
#endif
#endif
