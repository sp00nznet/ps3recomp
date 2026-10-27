/*
 * ps3recomp - the thread-local storage qualifier, on its own.
 *
 * PPU_THREAD_LOCAL is defined in three places that must agree: here,
 * ppu_context.h, and the lifter's HEADER_PREAMBLE (tools/ppu_lifter.py) which
 * bakes it into every generated ppu_recomp.h. The #ifndef makes including any
 * combination harmless.
 *
 * It lives in its own header because the boot scaffold and the loader need the
 * QUALIFIER but must not pull in a second definition of `ppu_context`: a port
 * built from a generated ppu_recomp.h already declares that struct, and
 * including ppu_context.h there is a redefinition error. Ports generated before
 * the qualifier existed do not define it at all, so the scaffold has to carry
 * its own reachable definition or it fails to build against exactly those
 * (older, not yet re-lifted) titles.
 */
#ifndef PS3RECOMP_PPU_TLS_H
#define PS3RECOMP_PPU_TLS_H

#ifndef PPU_THREAD_LOCAL
#  ifdef _MSC_VER
#    define PPU_THREAD_LOCAL __declspec(thread)
#  else
#    define PPU_THREAD_LOCAL __thread
#  endif
#endif

#endif /* PS3RECOMP_PPU_TLS_H */
