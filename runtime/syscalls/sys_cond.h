/*
 * ps3recomp - Condition variable syscalls
 */

#ifndef SYS_COND_H
#define SYS_COND_H

#include "lv2_syscall_table.h"
#include "sys_mutex.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"

#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <pthread.h>
  #include <time.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_COND_MAX  256

typedef struct sys_cond_info {
    int      active;
    uint32_t mutex_id;   /* associated mutex */
    char     name[8];

#ifdef _WIN32
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t     cv;
#endif

} sys_cond_info;

extern sys_cond_info g_sys_conds[SYS_COND_MAX];

/* Syscall handlers */
int64_t sys_cond_create(ppu_context* ctx);
int64_t sys_cond_destroy(ppu_context* ctx);
int64_t sys_cond_wait(ppu_context* ctx);
int64_t sys_cond_signal(ppu_context* ctx);
int64_t sys_cond_signal_all(ppu_context* ctx);

/* Registration */
void sys_cond_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_COND_H */
