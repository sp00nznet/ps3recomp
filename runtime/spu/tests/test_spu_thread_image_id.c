/* An SPU thread starts in the lifted image its sys_spu_image holds, not in the image that happens to own its entry
 * point. SPU programs all start near the same LS addresses, so two images can have code at one entry, and the
 * entry alone resolves to whichever registered first: the thread ran the other program's code (SoulCalibur II HD's
 * audio engine ran the wrong one). The image's own bytes say which it is.
 *
 * The registry is replaced by a table of two images (A registered first, B second, both with code at 0xD0), and
 * the checks follow what sys_spu_thread_group_start does: spu_thread_image_id, then spu_lifted_thread_setup.
 *
 * clang -std=gnu17 -I include -I runtime -I runtime/syscalls -I runtime/spu -I libs/video
 *   runtime/spu/tests/test_spu_thread_image_id.c runtime/spu/spu_lifted_thread.c -Wl,-dead_strip -pthread
 *   -o /tmp/test_spu_thread_image_id && /tmp/test_spu_thread_image_id
 */
#include "../../syscalls/lv2_register.c"
#include <assert.h>
#include <stdio.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
void SetLastError(DWORD e) { (void)e; }
uint32_t spurs_port_queue(uint32_t port) { (void)port; return 0; }
int64_t sys_event_flag_set(ppu_context* c) { (void)c; return 0; }

/* --- the registry, reduced to what the lookup needs: two images, both with code at 0xD0 --- */
#define IMG_A 1
#define IMG_B 2
#define IMG_SIZE 64
static uint64_t fp_of_image(int id) { return id == IMG_A ? 0xAAAA0000AAAAull : 0xBBBB0000BBBBull; }
uint64_t spu_workload_fingerprint(const void* data, size_t n)
{
    (void)n;
    const uint8_t c = ((const uint8_t*)data)[0];
    return c == 'A' ? fp_of_image(IMG_A) : c == 'B' ? fp_of_image(IMG_B) : 0x1234ull;   /* anything else: not registered */
}
spu_lifted_entry_fn spu_workload_find_img(uint64_t fp, int* id)
{
    if (fp == fp_of_image(IMG_A)) { *id = IMG_A; return (spu_lifted_entry_fn)(uintptr_t)1; }
    if (fp == fp_of_image(IMG_B)) { *id = IMG_B; return (spu_lifted_entry_fn)(uintptr_t)1; }
    return NULL;
}
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail) { (void)image; return max_avail >= IMG_SIZE ? IMG_SIZE : 0; }
uint32_t ps3_spu_image_source_ea(uint32_t img_ea) { return img_ea; }
/* the entry-point lookup: both images have code at 0xD0 and A registered first */
int spu_image_of_function(uint32_t addr) { return addr == 0xD0 ? IMG_A : -1; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    vm_base = calloc(1, 1u << 20); assert(vm_base);
    ppu_vm_size = 1u << 20;
    vm_base[0x1000] = 'A';                       /* image A's bytes */
    vm_base[0x2000] = 'B';                       /* image B's bytes */
    vm_base[0x3000] = 'X';                       /* bytes of an image nothing registered */

    spu_thread_t* t = spu_alloc_thread(); assert(t);
    static spu_context ctx;
    spu_lifted_thread_desc d;

    t->img_ea = 0x2000;
    CHECK(spu_thread_image_id(t) == IMG_B, "image B's bytes give id %d", spu_thread_image_id(t));
    memset(&d, 0, sizeof d); d.tid = 7; d.entry = 0xD0; d.image_id = spu_thread_image_id(t);
    spu_lifted_thread_setup(&ctx, &d);
    CHECK(ctx.image_id == IMG_B, "the thread starts in image %d, not image B", ctx.image_id);

    t->img_ea = 0x1000;
    CHECK(spu_thread_image_id(t) == IMG_A, "image A's bytes give id %d", spu_thread_image_id(t));

    memset(&d, 0, sizeof d); d.tid = 8; d.entry = 0xD0; d.image_id = 0;     /* what the lookup by entry gives */
    spu_lifted_thread_setup(&ctx, &d);
    CHECK(ctx.image_id == IMG_A, "without an image id the entry point still picks the first image (got %d)", ctx.image_id);

    t->img_ea = 0x3000;
    CHECK(spu_thread_image_id(t) == 0, "an unregistered image gives id %d", spu_thread_image_id(t));
    t->img_ea = 0;
    CHECK(spu_thread_image_id(t) == 0, "a thread with no image gives id %d", spu_thread_image_id(t));

    printf("[spu_thread_image_id] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
