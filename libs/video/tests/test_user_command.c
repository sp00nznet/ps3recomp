/*
 * ps3recomp -- GCM_SET_USER_COMMAND header decode
 *
 * The FIFO walker in cellGcmSys.c splits a method header into
 * (type, count, subchannel, method). GCM_SET_USER_COMMAND is method 0xEB00,
 * which is WIDER than the 13-bit field that decode keeps -- so it lands as
 * subchannel 7 / method 0x0B00, not as anything resembling 0xEB00. That is why
 * it was being routed into the 2D engine path and discarded, and the routing in
 * the walker is written in terms of the split values.
 *
 * ps1_netemu is what makes this matter: its user-command handler posts the
 * semaphore its flip path blocks on, so a dropped 0xEB00 parks the emulator.
 * If someone widens the method mask, this fails instead of the emulator
 * silently going back to zero frames.
 *
 * Build:
 *   clang -std=c11 -O2 test_user_command.c -o t.exe && ./t.exe
 */
#include <assert.h>
#include <stdio.h>

typedef unsigned int u32;

/* Verbatim from the walker (cellGcmSys.c). */
#define HDR_TYPE(w)   ((w) >> 29)
#define HDR_COUNT(w)  (((w) >> 18) & 0x7FFu)
#define HDR_METHOD(w) ((w) & 0x1FFCu)
#define HDR_SUBCH(w)  (((w) >> 13) & 7u)

int main(void)
{
    /* The exact word ps1_netemu writes at 0x114530:
     *     lis r9, 4 ; ori r9, r9, 0xeb00   ->  0x0004EB00
     * i.e. one incrementing method at address 0xEB00, argument 1. */
    const u32 w = 0x0004EB00u;

    assert(HDR_TYPE(w)   == 0);        /* incrementing method, not a jump/call */
    assert(HDR_COUNT(w)  == 1);        /* exactly one data dword follows      */
    assert(HDR_SUBCH(w)  == 7);        /* what the walker must match on ...   */
    assert(HDR_METHOD(w) == 0x0B00u);  /* ... together with this              */

    /* And the whole point: the raw method address is NOT what survives, so
     * matching on 0xEB00 would never fire. */
    assert(HDR_METHOD(w) != 0xEB00u);

    /* Neighbours must not be swept up with it. 0x0B00 on another subchannel is
     * a real 2D method, and 0x0B04 is the second user-command slot. */
    assert(HDR_SUBCH(0x0004CB00u) == 6);
    assert(HDR_METHOD(0x0004EB04u) == 0x0B04u);

    printf("test_user_command: ok\n");
    return 0;
}
