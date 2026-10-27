#include <time.h>
static time_t fixture_now;
static time_t fixture_time(time_t* out) { if(out) *out=fixture_now; return fixture_now; }
#define time fixture_time
#include "../runtime/syscalls/sys_timer.c"
#undef time
#include <assert.h>

uint8_t* vm_base;
static int32_t read_output(unsigned a) {
    uint8_t* p=vm_base+a;
    return (int32_t)(((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]);
}
static void check(const char* zone,time_t now,int32_t standard,int32_t dst) {
    setenv("TZ",zone,1); tzset(); fixture_now=now;
    memset(vm_base,0xa5,64);
    ppu_context c={0}; c.gpr[3]=16; c.gpr[4]=20;
    assert(sys_time_get_timezone(&c)==CELL_OK);
    assert(read_output(16)==standard); assert(read_output(20)==dst);
    assert(vm_base[15]==0xa5 && vm_base[24]==0xa5);
}
int main(void) {
    vm_base=calloc(1,64);
    check("UTC0",1705320000,0,0); // 2024-01-15 12:00 UTC
    check("PST8PDT,M3.2.0,M11.1.0",1705320000,-480,0);
    check("PST8PDT,M3.2.0,M11.1.0",1721044800,-480,60); // July
    check("NPT-5:45",1705320000,345,0);
    check("LHST-10:30LHDT-11,M10.1.0,M4.1.0",1705320000,630,30);
    check("LHST-10:30LHDT-11,M10.1.0,M4.1.0",1721044800,630,0);
    ppu_context c={0}; c.gpr[3]=0; c.gpr[4]=20;
    assert((int32_t)sys_time_get_timezone(&c)==(int32_t)CELL_EFAULT);
    c.gpr[3]=16; c.gpr[4]=0;
    assert((int32_t)sys_time_get_timezone(&c)==(int32_t)CELL_EFAULT);
    assert(SYS_TIME_GET_TIMEZONE==144);
    puts("PASS: timezone syscall writes signed BE32 minutes, separates DST, and rejects NULL outputs");
    free(vm_base);
}
