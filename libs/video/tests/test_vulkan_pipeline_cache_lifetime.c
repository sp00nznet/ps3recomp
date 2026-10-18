/* CPU fault injection against the production include. No Vulkan device,
 * SDL thread, existing cache, or filesystem write is used by this test. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

typedef uint32_t u32;
typedef uint32_t Uint32;
typedef struct { int value; } SDL_atomic_t;
typedef struct SDL_Thread { int (*fn)(void*); void* arg; int finished; } SDL_Thread;
#define SDLCALL
#define VK_LOG(...) ((void)0)
static Uint32 now_ms;
static int joined, destroyed, data_calls, create_fail, failures;
static SDL_Thread thread_slot;
static int SDL_AtomicGet(SDL_atomic_t* a) { return a->value; }
static int SDL_AtomicSet(SDL_atomic_t* a,int v) { int old=a->value; a->value=v; return old; }
static int SDL_AtomicAdd(SDL_atomic_t* a,int v) { int old=a->value; a->value+=v; return old; }
static int SDL_AtomicCAS(SDL_atomic_t* a,int old,int v) {
    if (a->value!=old) return 0;
    a->value=v; return 1;
}
static Uint32 SDL_GetTicks(void) { return now_ms; }
static SDL_Thread* SDL_CreateThread(int (*fn)(void*),const char* name,void* arg) {
    (void)name;
    if (create_fail) return NULL;
    thread_slot=(SDL_Thread){fn,arg,0}; return &thread_slot;
}
static void run_worker(void) {
    if (!thread_slot.finished) {
        thread_slot.fn(thread_slot.arg); thread_slot.finished=1;
    }
}
static void SDL_WaitThread(SDL_Thread* t,int* result) {
    if (t!=&thread_slot) failures++;
    now_ms+=6000; /* longer than the old unsafe five-second cutoff */
    run_worker(); joined++; if (result) *result=0;
}
static PFN_vkGetDeviceProcAddr pvkGetDeviceProcAddr;
static PFN_vkGetPhysicalDeviceProperties pvkGetPhysicalDeviceProperties;
#include "../rsx_vulkan_pipeline_cache.inc"

#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr,"line %d: %s\n",__LINE__,#c); } } while (0)
static VKAPI_ATTR VkResult VKAPI_CALL fail_data(VkDevice d,VkPipelineCache c,size_t* n,void* p) {
    (void)d; (void)c; (void)n; (void)p; data_calls++;
    CHECK(destroyed==0); return VK_ERROR_OUT_OF_HOST_MEMORY;
}
static VKAPI_ATTR void VKAPI_CALL destroy_cache(VkDevice d,VkPipelineCache c,const VkAllocationCallbacks* a) {
    (void)d; (void)c; (void)a;
    CHECK(!s_pcache.save_thread);
    CHECK(!SDL_AtomicGet(&s_pcache.saving));
    CHECK(thread_slot.finished); destroyed++;
}
static void reset(void) {
    memset(&s_pcache,0,sizeof s_pcache);
    memset(&thread_slot,0,sizeof thread_slot);
    joined=destroyed=data_calls=create_fail=0; now_ms=1;
    s_pcache.cache=(VkPipelineCache)(uintptr_t)1;
    pvkGetPipelineCacheData=fail_data; pvkDestroyPipelineCache=destroy_cache;
}
int main(void) {
    reset(); vk_pcache_created(); vk_pcache_tick(VK_NULL_HANDLE);
    CHECK(s_pcache.save_thread && SDL_AtomicGet(&s_pcache.saving));
    CHECK(SDL_AtomicGet(&s_pcache.dirty)==0);
    vk_pcache_shutdown(VK_NULL_HANDLE);
    CHECK(joined==1 && destroyed==1 && data_calls==2);
    CHECK(s_pcache.cache==VK_NULL_HANDLE && !s_pcache.save_thread);
    CHECK(SDL_AtomicGet(&s_pcache.dirty)>0); /* failed final retry not claimed saved */

    reset(); vk_pcache_created(); vk_pcache_tick(VK_NULL_HANDLE); run_worker();
    CHECK(SDL_AtomicGet(&s_pcache.dirty)>0 && !SDL_AtomicGet(&s_pcache.saving));
    now_ms=5000; vk_pcache_tick(VK_NULL_HANDLE); CHECK(joined==0);
    now_ms=10001; vk_pcache_tick(VK_NULL_HANDLE);
    CHECK(joined==1 && s_pcache.save_thread && SDL_AtomicGet(&s_pcache.saving));
    vk_pcache_shutdown(VK_NULL_HANDLE); CHECK(joined==2 && destroyed==1);

    reset(); create_fail=1; vk_pcache_created(); vk_pcache_tick(VK_NULL_HANDLE);
    CHECK(!s_pcache.save_thread && !SDL_AtomicGet(&s_pcache.saving));
    CHECK(SDL_AtomicGet(&s_pcache.dirty)>0);
    create_fail=0; now_ms=10001; vk_pcache_tick(VK_NULL_HANDLE);
    CHECK(s_pcache.save_thread); vk_pcache_shutdown(VK_NULL_HANDLE);
    CHECK(joined==1 && destroyed==1);

    memset(&s_pcache,0,sizeof s_pcache); joined=destroyed=0;
    vk_pcache_tick(VK_NULL_HANDLE); vk_pcache_shutdown(VK_NULL_HANDLE);
    CHECK(!joined && !destroyed);
    printf("pipeline cache lifetime: %s\n", failures ? "FAIL" : "PASS");
    return failures!=0;
}
