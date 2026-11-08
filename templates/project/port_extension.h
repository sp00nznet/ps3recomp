#pragma once
/*
 * Optional lifecycle callbacks for build extensions (PORT_EXTENSIONS).
 *
 * An extension is code a port keeps outside its main tree -- a renderer
 * experiment, a diagnostic, a capture tool -- and adds to a build with
 * -DPORT_EXTENSIONS=path/to/ext.cmake. Its sources fill a PortExtensionCallbacks
 * from a static constructor and register it. With nothing registered every
 * slot is null, each call site in main.cpp costs one branch, and the build
 * behaves exactly as without this file.
 */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortExtensionCallbacks {
    /* Host-only setup before settings are read and the guest VM is allocated. */
    void (*prepare)(const char* elf);
    /* The guest image is loaded and registered; the entry point runs next. */
    void (*start)(const char* elf);
    /* Once per pass of the host frame clock (its thread, not the guest's). */
    void (*frame_poll)(void);
    /* Process exit, whichever way it happens (run_guest returning,
     * sys_process_exit, a host quit): called once, from the exiting thread. */
    void (*stop)(void);
} PortExtensionCallbacks;

/* Adds every non-null slot. Each slot accepts up to eight registrants, called
 * in registration order (stop in reverse). Returns 0, or -1 without changing
 * anything if callbacks is null or a slot it fills is full. */
int port_extension_register(const PortExtensionCallbacks* callbacks);

/* Never null. A slot is null while nothing is registered for it. */
const PortExtensionCallbacks* port_extension(void);

#ifdef __cplusplus
}
#endif
