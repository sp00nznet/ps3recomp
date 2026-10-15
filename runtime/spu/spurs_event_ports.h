/* Host-only synchronized SPURS event-port bookkeeping. This registry does not
 * validate initialized SPURS owners/workers or live LV2 queues. */
#ifndef PS3RECOMP_SPURS_EVENT_PORTS_H
#define PS3RECOMP_SPURS_EVENT_PORTS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SPURS_EVENT_PORT_OWNER_LIMIT 4u
#define SPURS_EVENT_PORT_COUNT 64u
typedef enum spurs_event_port_status {
    SPURS_EVENT_PORT_OK=0,
    SPURS_EVENT_PORT_INVALID,
    SPURS_EVENT_PORT_BUSY,
    SPURS_EVENT_PORT_FULL,
    SPURS_EVENT_PORT_NOT_FOUND
} spurs_event_port_status;
/* All pointers name caller-owned host memory, never unchecked guest memory.
 * Failures leave output values/buffers and registry unchanged. Dynamic search
 * is per owner, ports16..63 only; requested_port must still be in range. */
spurs_event_port_status spurs_event_ports_attach(uint32_t owner, uint32_t queue,
    uint32_t requested_port, int is_dynamic, uint32_t *selected);
spurs_event_port_status spurs_event_ports_detach(uint32_t owner, uint32_t port);
uint32_t spurs_event_ports_lookup(uint32_t owner, uint32_t port);
/* Returns0 for no binding, invalid port, or more than one bound owner, even
 * when those owners name the same queue. No worker identity is inferred. */
uint32_t spurs_event_ports_lookup_unique(uint32_t port);
/* Remove all bindings of a destroyed queue and reclaim empty owner slots.
 * Returns number of removed per-port references. Caller serializes lifetime
 * release around this operation using the outer routing mutex. */
uint32_t spurs_event_ports_remove_queue(uint32_t queue);
/* Clear is all-or-nothing. Outputs contain unique queues that no remaining
 * owner uses. Unknown owner => NOT_FOUND; insufficient capacity => FULL.
 * Caller must serialize queue cancellation and reattachment around this result:
 * after unlock another attach could otherwise bind a returned queue again. */
spurs_event_port_status spurs_event_ports_clear_owner(uint32_t owner,
    uint32_t *orphaned_queues, uint32_t capacity, uint32_t *count);
/* Unique currently referenced queueIDs. No output changes on FULL/INVALID.
 * Both output pointers are required even when capacity is0. */
spurs_event_port_status spurs_event_ports_snapshot_all(uint32_t *queues,
    uint32_t capacity, uint32_t *count);
#ifdef __cplusplus
}
#endif
#endif
