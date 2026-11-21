/*
 * ps3recomp - short/failed write detection shared by the cellFs write paths.
 *
 * A cache or install worker loops while its nwrite cursor advances. Returning
 * CELL_OK with no progress on a read-only or full volume leaves it no signal,
 * so it loops forever.
 */
#ifndef PS3RECOMP_FS_WRITE_CHECK_H
#define PS3RECOMP_FS_WRITE_CHECK_H

#include <stddef.h>
#include <stdint.h>

static inline int ps3_fs_write_failed(size_t written, uint64_t requested, int stream_error)
{
    return stream_error != 0 || written != (size_t)requested;
}

#endif /* PS3RECOMP_FS_WRITE_CHECK_H */
