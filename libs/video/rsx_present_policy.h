#ifndef PS3RECOMP_RSX_PRESENT_POLICY_H
#define PS3RECOMP_RSX_PRESENT_POLICY_H

static inline int rsx_present_flip_already_rendered(
    unsigned clear_present_count,
    unsigned last_clear_presented_flip,
    unsigned current_flip)
{
    return clear_present_count != 0 &&
           last_clear_presented_flip == current_flip;
}

#endif
