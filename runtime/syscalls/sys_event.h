/*
 * ps3recomp - Event queue and event flag syscalls
 */

#ifndef SYS_EVENT_H
#define SYS_EVENT_H

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

/* ---------------------------------------------------------------------------
 * Event structure
 * -----------------------------------------------------------------------*/
typedef struct sys_event_t {
    uint64_t source;
    uint64_t data1;
    uint64_t data2;
    uint64_t data3;
} sys_event_t;

/* ---------------------------------------------------------------------------
 * Event Queues
 * -----------------------------------------------------------------------*/
#define SYS_EVENT_QUEUE_MAX       128
#define SYS_EVENT_QUEUE_BUF_MAX   127   /* max events in queue buffer */

/* Queue types */
#define SYS_PPU_QUEUE   1
#define SYS_SPU_QUEUE   2

typedef struct sys_event_queue_info {
    int      active;
    uint32_t protocol;
    int32_t  type;
    char     name[8];
    uint64_t key;

    /* Circular buffer */
    sys_event_t  buffer[SYS_EVENT_QUEUE_BUF_MAX];
    int          capacity;
    int          head;
    int          tail;
    int          count;

#ifdef _WIN32
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE not_empty;
#else
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
#endif

} sys_event_queue_info;

extern sys_event_queue_info g_sys_event_queues[SYS_EVENT_QUEUE_MAX];

/* ---------------------------------------------------------------------------
 * Event Ports
 * -----------------------------------------------------------------------*/
#define SYS_EVENT_PORT_MAX  256

#define SYS_EVENT_PORT_LOCAL  1

typedef struct sys_event_port_info {
    int      active;
    int32_t  type;
    uint64_t name;
    int32_t  connected_queue;  /* queue index+1, 0 = not connected */
} sys_event_port_info;

extern sys_event_port_info g_sys_event_ports[SYS_EVENT_PORT_MAX];

/* ---------------------------------------------------------------------------
 * Event Flags
 * -----------------------------------------------------------------------*/
#define SYS_EVENT_FLAG_MAX  256

/* Wait modes */
#define SYS_EVENT_FLAG_WAIT_AND        0x01
#define SYS_EVENT_FLAG_WAIT_OR         0x02
#define SYS_EVENT_FLAG_WAIT_CLEAR      0x10
#define SYS_EVENT_FLAG_WAIT_CLEAR_ALL  0x20

typedef struct sys_event_flag_info {
    int      active;
    uint32_t protocol;
    uint32_t type;       /* single / multi waiter */
    char     name[8];
    uint64_t pattern;    /* 64-bit flag word */

#ifdef _WIN32
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cv;
#else
    pthread_mutex_t lock;
    pthread_cond_t  cv;
#endif

} sys_event_flag_info;

extern sys_event_flag_info g_sys_event_flags[SYS_EVENT_FLAG_MAX];

/* ---------------------------------------------------------------------------
 * Syscall handlers - Event Queues
 * -----------------------------------------------------------------------*/
int64_t sys_event_queue_create(ppu_context* ctx);
int64_t sys_event_queue_destroy(ppu_context* ctx);
int64_t sys_event_queue_receive(ppu_context* ctx);
int64_t sys_event_queue_tryreceive(ppu_context* ctx);
int64_t sys_event_queue_drain(ppu_context* ctx);

/* Event Ports */
int64_t sys_event_port_create(ppu_context* ctx);
int64_t sys_event_port_destroy(ppu_context* ctx);
int64_t sys_event_port_connect_local(ppu_context* ctx);
int64_t sys_event_port_disconnect(ppu_context* ctx);
int64_t sys_event_port_send(ppu_context* ctx);

/* Event Flags */
int64_t sys_event_flag_create(ppu_context* ctx);
int64_t sys_event_flag_destroy(ppu_context* ctx);
int64_t sys_event_flag_wait(ppu_context* ctx);
int64_t sys_event_flag_trywait(ppu_context* ctx);
int64_t sys_event_flag_set(ppu_context* ctx);
int64_t sys_event_flag_clear(ppu_context* ctx);
int64_t sys_event_flag_get(ppu_context* ctx);

/* Registration */
void sys_event_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_EVENT_H */
