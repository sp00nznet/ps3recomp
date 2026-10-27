#pragma once
/* ps3recomp shim for PPSSPP's aligned allocator. */
#include <cstdlib>
#ifdef _WIN32
#include <malloc.h>
inline void* AllocateAlignedMemory(size_t size, size_t align) { return _aligned_malloc(size, align); }
inline void FreeAlignedMemory(void* p) { _aligned_free(p); }
#else
inline void* AllocateAlignedMemory(size_t size, size_t align)
{ void* p = nullptr; return posix_memalign(&p, align, size) ? nullptr : p; }
inline void FreeAlignedMemory(void* p) { free(p); }
#endif
