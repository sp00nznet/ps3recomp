/*
 * spu_fallback.c — registry implementation for PPU-side SPU job fallbacks.
 *
 * See ps3emu/spu_fallback.h for usage. The lookup is consulted by
 * sys_spu_thread_group_start_handler in lv2_register.c when starting a
 * group; matching threads spawn a host thread running the registered
 * handler instead of the default no-op completion.
 */

#include "ps3emu/spu_fallback.h"

#include <stddef.h>
#include <string.h>

#define SPU_FALLBACK_MAX 64

typedef struct {
    int                 in_use;
    uint32_t            entry_point;
    spu_ppu_fallback_fn handler;
    void*               user;
} spu_fallback_entry_t;

static spu_fallback_entry_t s_table[SPU_FALLBACK_MAX];

int spu_register_ppu_fallback(uint32_t entry_point,
                              spu_ppu_fallback_fn handler, void* user)
{
    if (handler == NULL) return spu_unregister_ppu_fallback(entry_point);

    /* Replace any existing entry for the same entry point. */
    for (int i = 0; i < SPU_FALLBACK_MAX; i++) {
        if (s_table[i].in_use && s_table[i].entry_point == entry_point) {
            s_table[i].handler = handler;
            s_table[i].user    = user;
            return 0;
        }
    }
    /* Otherwise find a free slot. */
    for (int i = 0; i < SPU_FALLBACK_MAX; i++) {
        if (!s_table[i].in_use) {
            s_table[i].in_use      = 1;
            s_table[i].entry_point = entry_point;
            s_table[i].handler     = handler;
            s_table[i].user        = user;
            return 0;
        }
    }
    return -1; /* table full */
}

int spu_unregister_ppu_fallback(uint32_t entry_point)
{
    for (int i = 0; i < SPU_FALLBACK_MAX; i++) {
        if (s_table[i].in_use && s_table[i].entry_point == entry_point) {
            s_table[i].in_use = 0;
            s_table[i].handler = NULL;
            s_table[i].user = NULL;
            return 1;
        }
    }
    return 0;
}

spu_ppu_fallback_fn spu_lookup_ppu_fallback(uint32_t entry_point,
                                            void** out_user)
{
    for (int i = 0; i < SPU_FALLBACK_MAX; i++) {
        if (s_table[i].in_use && s_table[i].entry_point == entry_point) {
            if (out_user) *out_user = s_table[i].user;
            return s_table[i].handler;
        }
    }
    if (out_user) *out_user = NULL;
    return NULL;
}
