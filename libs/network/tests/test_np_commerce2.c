/*
 * test_np_commerce2 - sceNpCommerce2CreateCtx's arguments.
 *
 * The SDK signature is (version, npId, handler, arg, ctx_id): the new context
 * id goes to the FIFTH argument, a guest pointer. The call below is made the
 * way the flat HLE adapter makes it, through a function pointer taking five
 * integer arguments (r3..r7), with guest addresses in the pointer slots. With
 * the three-parameter declaration the id was written over the title's handler
 * descriptor (third argument) and the title's own ctx_id variable never got set.
 *
 *   cc -std=gnu17 -I include -o test_np_commerce2 libs/network/tests/test_np_commerce2.c
 */
#include "../sceNpCommerce2.c"

#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t addr) { (void)addr; }
void spu_linelock_lock(uint32_t addr) { (void)addr; }
void spu_linelock_release(uint32_t addr) { (void)addr; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; }

typedef int32_t (*create_ctx5_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

#define NPID     0x1000u
#define HANDLER  0x2000u
#define CTX_ID   0x3000u

int main(void)
{
    static uint8_t mem[0x10000];
    vm_base = mem;
    ppu_vm_size = sizeof mem;

    create_ctx5_t create = (create_ctx5_t)(void*)sceNpCommerce2CreateCtx;
    sceNpCommerce2Init();

    memset(mem + HANDLER, 0xAA, 16);      /* the title's handler descriptor */
    memset(mem + CTX_ID, 0xBB, 4);        /* the title's ctx_id variable */

    assert(create(2, NPID, HANDLER, 0, CTX_ID) == CELL_OK);
    for (int i = 0; i < 16; i++)
        assert(mem[HANDLER + i] == 0xAA);                 /* handler untouched */
    assert(vm_read32(CTX_ID) == 0);                       /* first id, stored big-endian */

    assert(create(2, NPID, HANDLER, 0, CTX_ID) == CELL_OK);
    assert(vm_read32(CTX_ID) == 1);                       /* a second context gets the next id */
    for (int i = 0; i < 16; i++)
        assert(mem[HANDLER + i] == 0xAA);

    /* no id pointer: invalid argument, nothing written */
    assert(create(2, NPID, HANDLER, 0, 0) == (int32_t)SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT);
    for (int i = 0; i < 16; i++)
        assert(mem[HANDLER + i] == 0xAA);

    printf("test_np_commerce2: all passed\n");
    return 0;
}
