/* Default definition of ppu_resv_break_line.
 *
 * spu_coherency.c calls it on every SPU write to a reserved line, to break the
 * PPU threads' lwarx/stwcx. reservations. The real definition lives in
 * runtime/ppu/ppu_loader.cpp, which is compiled per game rather than into the
 * library, so an executable without the PPU loader (ps3recomp_host, the tests)
 * could not link without this one.
 *
 * Without a PPU loader there are no PPU reservations, so doing nothing is the
 * exact behaviour. This file defines only this symbol, so the linker pulls the
 * archive member only when nothing else provides it; as soon as a game links
 * ppu_loader.cpp, that definition is the one used.
 * (A plain definition, not __attribute__((weak)): the library also builds
 * under MSVC, which has no weak attribute; see spu_tsp_weak.c.) */
#include <stdint.h>

void ppu_resv_break_line(uint32_t ea)
{
    (void)ea;
}
