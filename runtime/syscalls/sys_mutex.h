/*
 * ps3recomp - Kernel mutex syscalls
 *
 * Real mutex using host OS primitives.
 */

#ifndef SYS_MUTEX_H
#define SYS_MUTEX_H

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
  #include <time.h>
  #include <errno.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_MUTEX_MAX  256

/* Protocol types */
#define SYS_SYNC_FIFO            0x1
#define SYS_SYNC_PRIORITY        0x2
#define SYS_SYNC_PRIORITY_INHERIT 0x3

/* Mutex flags */
#define SYS_SYNC_RECURSIVE       0x10
#define SYS_SYNC_NOT_RECURSIVE   0x20

/* PS3 mutex attribute struct (as seen in guest memory) */
typedef struct sys_mutex_attribute_t {
    uint32_t protocol;
    uint32_t recursive;
    uint32_t pshared;
    uint32_t adaptive;
    uint64_t key;
    int32_t  flags;
    uint32_t pad;
    char     name[8];
} sys_mutex_attribute_t;

/* Internal mutex descriptor */
typedef struct sys_mutex_info {
    int      active;
    char     name[8];
    uint32_t protocol;
    int      recursive;
    uint64_t owner_tid;   /* thread_id of current owner (for recursive/error checking) */
    int      lock_count;  /* recursive lock count */

#ifdef _WIN32
    CRITICAL_SECTION cs;
    HANDLE           wait_handle;  /* for timed waits */
#else
    pthread_mutex_t  mtx;
#endif

} sys_mutex_info;

extern sys_mutex_info g_sys_mutexes[SYS_MUTEX_MAX];

/* Syscall handlers */
int64_t sys_mutex_create(ppu_context* ctx);
int64_t sys_mutex_destroy(ppu_context* ctx);
int64_t sys_mutex_lock(ppu_context* ctx);
int64_t sys_mutex_trylock(ppu_context* ctx);
int64_t sys_mutex_unlock(ppu_context* ctx);

/* Registration */
void sys_mutex_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_MUTEX_H */
