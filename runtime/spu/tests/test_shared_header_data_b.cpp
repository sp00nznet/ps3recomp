/* Second object of test_shared_header_data.c: C++ readers of the header-defined data. */
#include "spu_coh_bitmap.h"
#include "ps3emu/ps3_ftrace.h"

extern "C" int shared_b_line_reserved(uint32_t addr) { return spu_coh_line_reserved(addr); }
extern "C" const void* shared_b_coh_bits(void) { return (const void*)&g_spu_coh_bits; }
extern "C" const void* shared_b_ft(void) { return (const void*)&g_ps3_ft; }
