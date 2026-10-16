/* Native cooperative contexts. Android supplies the ABI structs but not the
 * switching functions. The pinned libucontext POSIX wrapper preserves signal
 * masks; its architecture implementation performs the actual stack switch. */
#ifndef PS3RECOMP_UCONTEXT_H
#define PS3RECOMP_UCONTEXT_H

#if defined(__ANDROID__)
#include <libucontext/libucontext.h>
#ifdef __cplusplus
extern "C" {
#endif
int getcontext(libucontext_ucontext_t*);
int setcontext(const libucontext_ucontext_t*);
int swapcontext(libucontext_ucontext_t*, const libucontext_ucontext_t*);
#ifdef __cplusplus
}
#endif
/* makecontext does not alter the signal mask and lives in the base library. */
#define makecontext libucontext_makecontext
#else
#include <ucontext.h>
#endif

#endif
