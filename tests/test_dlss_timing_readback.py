"""Compile the actual timing readback block against deterministic query failures.

No Vulkan device is created. The block is extracted to exercise production
control flow, including whether the dependent query is incorrectly called.
"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    source = (Path(__file__).resolve().parents[1] /
              "libs/video/rsx_vulkan_ray_ao.inc").read_text()
    begin = source.index("    int gpu_timing_valid=0,dlss_prep_valid=0;")
    end = source.index("    s_ray_ao.cpu_ms=", begin)
    block = source[begin:end]
    fixture = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
#define VK_SUCCESS 0
#define VK_QUERY_RESULT_64_BIT 1
static int stage_result, split_result, query_calls;
static u64 stamp[8];
static int query(int device,int pool,unsigned first,unsigned count,size_t size,
                 void *out,size_t stride,unsigned flags) {
    (void)device;(void)pool;(void)size;(void)stride;(void)flags;
    ++query_calls;
    int result=first==0?stage_result:split_result;
    if(result==VK_SUCCESS)memcpy(out,stamp+first,count*sizeof(u64));
    return result;
}
static struct {int device;} s_vk;
static struct {
    int queries;
    unsigned timestamp_bits;
    int (*query_results)(int,int,unsigned,unsigned,size_t,void*,size_t,unsigned);
    struct {struct {double timestampPeriod;} limits;} properties;
    double gpu_ms[7];
} s_ray_ao;
static int stage_valid,split_valid,checks;
static void read_timings(int dlss_view) {
PRODUCTION_BLOCK
    stage_valid=gpu_timing_valid;split_valid=dlss_prep_valid;
}
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static void setup(void) {
    memset(&s_ray_ao,0,sizeof s_ray_ao);
    s_ray_ao.queries=1;s_ray_ao.timestamp_bits=64;s_ray_ao.query_results=query;
    s_ray_ao.properties.limits.timestampPeriod=1000000.0;
    for(unsigned i=0;i<7;i++){s_ray_ao.gpu_ms[i]=999;stamp[i]=10+i*10;}
    stamp[7]=65;query_calls=0;stage_result=split_result=0;
}
int main(void) {
    setup();s_ray_ao.queries=0;read_timings(1);
    CHECK(query_calls==0 && !stage_valid && !split_valid);
    for(unsigned i=0;i<7;i++)CHECK(s_ray_ao.gpu_ms[i]==0);
    setup();stage_result=1;read_timings(1);
    CHECK(query_calls==1 && !stage_valid && !split_valid);
    for(unsigned i=0;i<7;i++)CHECK(s_ray_ao.gpu_ms[i]==0);
    setup();split_result=1;read_timings(1);
    CHECK(query_calls==2 && stage_valid && !split_valid);
    for(unsigned i=0;i<6;i++)CHECK(s_ray_ao.gpu_ms[i]==10);
    CHECK(s_ray_ao.gpu_ms[6]==0);
    setup();read_timings(1);
    CHECK(query_calls==2 && stage_valid && split_valid);
    CHECK(s_ray_ao.gpu_ms[6]==5);
    setup();read_timings(0);
    CHECK(query_calls==1 && stage_valid && !split_valid);
    CHECK(s_ray_ao.gpu_ms[6]==0);
    setup();s_ray_ao.timestamp_bits=32;stamp[5]=UINT32_MAX-2u;stamp[6]=4;stamp[7]=1;
    read_timings(1);
    CHECK(stage_valid && split_valid);
    CHECK(s_ray_ao.gpu_ms[5]==7 && s_ray_ao.gpu_ms[6]==4);
    setup();stamp[5]=UINT64_MAX-2u;stamp[6]=4;stamp[7]=1;read_timings(1);
    CHECK(s_ray_ao.gpu_ms[5]==7 && s_ray_ao.gpu_ms[6]==4);
    printf("DLSS timing readback: %d checks passed\n",checks);
    return 0;
}
""".replace("PRODUCTION_BLOCK", block)
    path = args.out / "dlss_timing_readback.c"
    path.write_text(fixture, encoding="utf-8")
    executable = args.out / "dlss_timing_readback.exe"
    subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path), "-o", str(executable)], check=True)
    return subprocess.run([str(executable)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
