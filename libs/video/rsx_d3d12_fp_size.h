#ifndef PS3RECOMP_RSX_D3D12_FP_SIZE_H
#define PS3RECOMP_RSX_D3D12_FP_SIZE_H

#include <windows.h>
#include "rsx_fp_decompiler.h"

#define RSX_D3D12_FP_MAX_BYTES 0x10000u

/* The caller supplies the same readable 4 KiB prefix used by the old path.
 * Ordinary programs never query virtual memory. Extend only when END lies
 * beyond that prefix, bounded by the committed region and a 64 KiB ceiling. */
static u32 d3d12_fp_program_size(const u8* ptr)
{
    const u32 short_size = rsx_fp_program_size(ptr, 4096);
    if (short_size) return short_size;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    SIZE_T available = mbi.RegionSize - (SIZE_T)(ptr - (const u8*)mbi.BaseAddress);
    if (available > RSX_D3D12_FP_MAX_BYTES) available = RSX_D3D12_FP_MAX_BYTES;
    return rsx_fp_program_size(ptr, (u32)available);
}

#endif
