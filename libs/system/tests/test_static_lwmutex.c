#include "libs/system/sysPrxForUser.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
u8 *vm_base;
unsigned int ppu_active_lr(void) { return 0; }
static sys_lwmutex_t_hle *guest = (void *)0x1000;
static void *contender(void *arg) {
    (void)arg;
    assert(sys_lwmutex_trylock(guest) == CELL_EBUSY);
    return NULL;
}
int main(void) {
    vm_base = calloc(1, 0x10000);
    sys_lwmutex_t_hle *m = (void *)(vm_base + 0x1000);
    /* Static guest initializer: recursive flag in big-endian byte order. */
    m->attribute = 0x10000000;
    assert(sys_lwmutex_lock(guest, 0) == CELL_OK);
    assert(sys_lwmutex_trylock(guest) == CELL_OK);
    assert(sys_lwmutex_unlock(guest) == CELL_OK);
    pthread_t t;
    assert(pthread_create(&t, NULL, contender, NULL) == 0);
    pthread_join(t, NULL);
    assert(sys_lwmutex_unlock(guest) == CELL_OK);
    assert(sys_lwmutex_destroy(guest) == CELL_OK);
    puts("static recursive guest mutex and competing thread: PASS");
    return 0;
}
