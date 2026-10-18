/* CPU test: production timing wrapper with a deterministic clock and driver. */
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
typedef uint32_t u32;
typedef uint64_t Uint64;
static Uint64 ticks=1000000, cost=4000, frequency=1000000;
static int reads, calls, lines, failures;
static VkResult wanted=VK_SUCCESS;
static char last[512];
static Uint64 SDL_GetPerformanceCounter(void) { reads++; return ticks; }
static Uint64 SDL_GetPerformanceFrequency(void) { return frequency; }
static void record(const char* fmt,...) {
    va_list ap; va_start(ap,fmt); vsnprintf(last,sizeof last,fmt,ap); va_end(ap); lines++;
}
#define VK_LOG(...) record(__VA_ARGS__)
static VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice d,VkPipelineCache c,u32 n,
    const VkGraphicsPipelineCreateInfo* i,const VkAllocationCallbacks* a,VkPipeline* p) {
    (void)d; (void)c; (void)i; (void)a;
    if (n!=1 || !p) failures++;
    calls++; ticks+=cost; *p=(VkPipeline)(uintptr_t)99; return wanted;
}
static PFN_vkCreateGraphicsPipelines pvkCreateGraphicsPipelines=create;
#include "../rsx_vulkan_pipeline_timing.inc"
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr,"line %d: %s\n",__LINE__,#c); } } while (0)
static VkResult run(void) {
    VkPipeline p=VK_NULL_HANDLE;
    VkResult r=vk_create_graphics_timed("test",7,VK_NULL_HANDLE,(VkPipelineCache)(uintptr_t)1,1,NULL,NULL,&p);
    CHECK(p==(VkPipeline)(uintptr_t)99); return r;
}
int main(void) {
    s_pipeline_timing=0; CHECK(run()==VK_SUCCESS); CHECK(calls==1 && reads==0 && lines==0);
    s_pipeline_timing=1; cost=1999; run(); CHECK(lines==0);
    cost=2000; run(); CHECK(lines==1 && strstr(last,"id=7") && strstr(last,"elapsed_us=2000"));
    CHECK(strstr(last,"cache=1"));
    cost=1; wanted=VK_ERROR_DEVICE_LOST; CHECK(run()==wanted); CHECK(lines==2);
    frequency=0; run(); CHECK(lines==2); frequency=1000000;
    wanted=VK_SUCCESS; cost=6000;
    for (int i=0;i<300;i++) run();
    CHECK(lines==256 && s_pipeline_timing_lines==256);
    int old_reads=reads; run(); CHECK(reads==old_reads);
    printf("pipeline timing: %s\n",failures ? "FAIL" : "PASS"); return failures!=0;
}
