/* SPU event_status is updated from more than one host thread: the SPU's own
 * (a list stall raises Sn, WrEventAck clears) and a PPU or peer-SPU store that
 * kills its reservation (Lr, spu_coherency.c). Every update goes through
 * SPU_EVENT_SET / SPU_EVENT_CLEAR, which are atomic. Here one thread raises
 * and acknowledges Sn while another raises and acknowledges Lr on the same
 * context, and Sn must never vanish between being raised and being acked.
 * With a plain |= / &= (SPU_EVENT_PLAIN, the old code) it does: a raise that
 * read the word before the other side's write puts back the stale value. */
#include "spu_context.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef SPU_EVENT_PLAIN
#undef SPU_EVENT_SET
#undef SPU_EVENT_CLEAR
#define SPU_EVENT_SET(c, b)   (*(volatile uint32_t*)&(c)->event_status |= (b))
#define SPU_EVENT_CLEAR(c, b) (*(volatile uint32_t*)&(c)->event_status &= ~(uint32_t)(b))
#endif

uint8_t* vm_base;
uint32_t ppu_vm_size;

enum { ROUNDS = 5000000 };
static spu_context s_ctx;
static volatile int s_started, s_done;

static void* lr_raiser(void* arg)            /* the PPU / peer side */
{
    (void)arg;           /* Lr is 0x400 (SPU_EVENT_LR in spu_coherency.c) */
    s_started = 1;
    while (!s_done) {
        SPU_EVENT_SET(&s_ctx, 0x400u);
        SPU_EVENT_CLEAR(&s_ctx, 0x400u);
    }
    return NULL;
}

int main(void)
{
    pthread_t t;
    pthread_create(&t, NULL, lr_raiser, NULL);
    while (!s_started) ;                       /* both sides running before counting */
    unsigned lost = 0;
    for (unsigned i = 0; i < ROUNDS; i++) {   /* the SPU side: Sn raised, then acked */
        SPU_EVENT_SET(&s_ctx, 0x2u);
        for (volatile int k = 0; k < 16; k++) ;  /* the handler runs a little before it acks */
        if (!(*(volatile uint32_t*)&s_ctx.event_status & 0x2u)) lost++;
        SPU_EVENT_CLEAR(&s_ctx, 0x2u);
    }
    s_done = 1;
    pthread_join(t, NULL);
    printf("%s: Sn lost %u times in %u raises under concurrent Lr updates\n",
           lost ? "FAIL" : "PASS", lost, (unsigned)ROUNDS);
    return lost ? 1 : 0;
}
