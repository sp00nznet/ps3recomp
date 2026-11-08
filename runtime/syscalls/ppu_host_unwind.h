/* Optional generated-code cleanup before a host C nonlocal exit skips frames.
 * This does not make C longjmp run C++ destructors or unwind arbitrary resources. */
#ifndef PS3RECOMP_PPU_HOST_UNWIND_H
#define PS3RECOMP_PPU_HOST_UNWIND_H
#ifdef __cplusplus
extern "C" {
#endif
void ps3_ppu_set_host_unwind_handler(void (*handler)(void));
void ps3_ppu_notify_host_unwind(void);
#ifdef __cplusplus
}
#endif
#endif
