/* Default stub for the lifted SPURS taskset-policy SPU image.
 *
 * spurs_policy.c memcpy's this blob to LS 0xA00 before running the taskset policy
 * (see spu_run_taskset_policy). The real bytes are extracted per-game from that
 * title's firmware libsre image; every other port still needs the symbols to LINK.
 * A zero size makes spurs_policy.c bail out of the taskset-policy path instead of
 * executing an empty local store.
 *
 * Separate one-symbol TU on purpose: a port that ships the real blob resolves the
 * reference from its own object and this archive member is never pulled in, so no
 * duplicate-symbol error. Same pattern as spu_tsp_weak.c -- which must stay a
 * SEPARATE file, since a port can supply the lifted entry (tsp_spu_func_00000A00)
 * without supplying the raw blob, and vice versa. */

const unsigned char g_taskset_policy_bytes[1] = { 0 };
const unsigned      g_taskset_policy_size     = 0;
