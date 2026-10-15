/* Opt-in diagnostic stops for native bring-up. This does not certify HLE
 * semantics or successful boot. Set PS3_STRICT_BOOT=1 before process startup. */
#pragma once

#include "ppu_recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline bool ppu_strict_boot_enabled()
{
    static const bool enabled = [] {
        const char* value = getenv("PS3_STRICT_BOOT");
        return value && strcmp(value, "1") == 0;
    }();
    return enabled;
}

/* Only read the supplied context, never guest memory or a guessed OPD. A bad
 * dispatch target must not fault while trying to produce its failure receipt.
 * CIA/LR are the recorded context values, not guaranteed precise call sites. */
static inline void ppu_strict_boot_guard(const char* category,
                                         const ppu_context* ctx,
                                         const char* detail, uint64_t value)
{
    if (!ppu_strict_boot_enabled()) return;
    fprintf(stderr, "[strict-boot] FATAL category=%s %s=0x%llX (%llu)",
            category, detail, (unsigned long long)value,
            (unsigned long long)value);
    if (ctx) {
        fprintf(stderr, " cia=0x%08X lr=0x%08X ctr=0x%08X tid=%llu"
                        " r2=0x%08X r3=0x%08X r11=0x%08X r12=0x%08X",
                (uint32_t)ctx->cia, (uint32_t)ctx->lr, (uint32_t)ctx->ctr,
                (unsigned long long)ctx->thread_id, (uint32_t)ctx->gpr[2],
                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[11],
                (uint32_t)ctx->gpr[12]);
    } else {
        fprintf(stderr, " context=unavailable");
    }
    fprintf(stderr, " exit=86\n");
    fflush(NULL);
    _Exit(86);
}
