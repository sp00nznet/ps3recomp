#ifndef PS3RECOMP_SPURS_EVENT_ROUTE_H
#define PS3RECOMP_SPURS_EVENT_ROUTE_H
#include <stdint.h>
struct spu_context;
#ifdef __cplusplus
extern "C" {
#endif
/* Actual SPURS mailbox event routing. The owner EA is binding provenance,
 * not an initialized owner/worker lease. Owner0 resolves unique legacy ports.
 * Serializes lookup, mailbox consumption and real queue push against teardown. */
int spurs_route_user_event(struct spu_context*,uint32_t value);
uint32_t spurs_port_queue(uint32_t port);
uint32_t spurs_port_queue_for_owner(uint32_t owner,uint32_t port);
/* Called only after event queue destruction has released its table/queue
 * locks. Invalidates bindings before their old slot can be reused. */
void spurs_event_queue_destroyed(uint32_t queue);
#ifdef __cplusplus
}
#endif
#endif
