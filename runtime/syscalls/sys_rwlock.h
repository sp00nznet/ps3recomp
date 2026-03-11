/*
 * ps3recomp - Reader-writer lock syscalls
 */

#ifndef SYS_RWLOCK_H
#define SYS_RWLOCK_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"

#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_RWLOCK_MAX  256

typedef struct sys_rwlock_info {
    int      active;
    uint32_t protocol;
    char     name[8];

#ifdef _WIN32
    SRWLOCK  srw;
    /* Track exclusive ownership for proper unlock dispatch */
    volatile LONG  readers;
    volatile LONG  writer;
#else
    pthread_rwlock_t rwl;
#endif

} sys_rwlock_info;

extern sys_rwlock_info g_sys_rwlocks[SYS_RWLOCK_MAX];

/* Syscall handlers */
int64_t sys_rwlock_create(ppu_context* ctx);
int64_t sys_rwlock_destroy(ppu_context* ctx);
int64_t sys_rwlock_rlock(ppu_context* ctx);
int64_t sys_rwlock_tryrlock(ppu_context* ctx);
int64_t sys_rwlock_runlock(ppu_context* ctx);
int64_t sys_rwlock_wlock(ppu_context* ctx);
int64_t sys_rwlock_trywlock(ppu_context* ctx);
int64_t sys_rwlock_wunlock(ppu_context* ctx);

/* Registration */
void sys_rwlock_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_RWLOCK_H */
