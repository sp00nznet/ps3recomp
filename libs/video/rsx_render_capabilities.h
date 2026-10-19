/* Read-only advanced-rendering observations. No optional feature is enabled
 * by querying these fields. Hardware observations are not renderer support. */
#ifndef RSX_RENDER_CAPABILITIES_H
#define RSX_RENDER_CAPABILITIES_H
#include <stdint.h>
#include <stddef.h>

typedef struct rsx_render_capabilities {
    uint32_t initialized, instance_api, device_api, vendor_id, device_id;
    /* -1 unknown, 0 unavailable, 1 supported by the physical device.
     * Features are NOT enabled on the logical device by this module. */
    int acceleration_structure, ray_tracing_pipeline, ray_query, buffer_device_address;
    int deferred_host_operations;
} rsx_render_capabilities;

#endif
