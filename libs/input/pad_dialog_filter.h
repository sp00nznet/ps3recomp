/* Pure per-port gate for pad input while a host message dialog owns input.
 * The caller serializes access to each state and supplies completed raw polls.
 */
#ifndef PS3RECOMP_PAD_DIALOG_FILTER_H
#define PS3RECOMP_PAD_DIALOG_FILTER_H

#include <stdint.h>

typedef struct PadDialogFilter {
    uint64_t epoch;
    uint64_t release_after_poll;
    int holding;
} PadDialogFilter;

typedef struct PadDialogRaw {
    uint16_t buttons;
    uint8_t stick[4];
    uint8_t trigger[2];
    int pressure;
} PadDialogRaw;

static inline int pad_dialog_raw_neutral(const PadDialogRaw* raw)
{
    if (raw->buttons || raw->pressure || raw->trigger[0] > 8 ||
        raw->trigger[1] > 8) return 0;
    for (unsigned i=0;i<4;++i)
        if (raw->stick[i] < 120 || raw->stick[i] > 136) return 0;
    return 1;
}

/* An unseen dialog epoch still starts quarantine when first observed after
 * closure. A cached neutral sample never clears it: one later completed host
 * poll with every control released is required. */
static inline int pad_dialog_filter_step(PadDialogFilter* state, int active,
                                          uint64_t epoch, uint64_t poll_serial,
                                          const PadDialogRaw* raw)
{
    if (epoch && state->epoch != epoch) {
        state->epoch=epoch;
        state->holding=1;
        state->release_after_poll=poll_serial;
    }
    if (active) {
        state->holding=1;
        if (poll_serial > state->release_after_poll)
            state->release_after_poll=poll_serial;
        return 1;
    }
    if (!state->holding) return 0;
    if (poll_serial > state->release_after_poll && pad_dialog_raw_neutral(raw)) {
        state->holding=0;
        return 0;
    }
    return 1;
}

#endif
