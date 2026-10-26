/*
 * ps3recomp - host path of a file under /dev_flash
 *
 * PS3_DEV_FLASH names the host directory that /dev_flash maps into. It may name
 * several, separated by ';' on Windows and ':' elsewhere (the separator of PATH
 * on that host), and the first directory that holds the file wins. That lets a
 * directory of stand-ins for a few firmware files sit in front of a full
 * dev_flash dump, or a directory of stand-ins be used on its own.
 *
 * The cellFs layer (runtime/ppu/ppu_fs.cpp) and the raw sys_fs syscalls
 * (runtime/syscalls/sys_fs.c) both translate /dev_flash paths and have to agree,
 * so both call this.
 */
#ifndef PS3RECOMP_PS3_DEV_FLASH_H
#define PS3RECOMP_PS3_DEV_FLASH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes into `out` (size `cap`) the host path of `rel`, the part of a guest path
 * after "/dev_flash/", for the root list `roots` (PS3_DEV_FLASH). The first root
 * under which `rel` exists is used; when none has it, the first root, so that
 * the caller's open fails with the error a missing file gives. The parts are
 * joined with '/'; the caller converts separators to its host's convention. */
void ps3_dev_flash_path(char* out, size_t cap, const char* roots, const char* rel);

#ifdef __cplusplus
}
#endif

#endif
