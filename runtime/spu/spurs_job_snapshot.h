#ifndef PS3_SPURS_JOB_SNAPSHOT_H
#define PS3_SPURS_JOB_SNAPSHOT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SPURS_JOB_SNAPSHOT_DESC_MIN 48u
#define SPURS_JOB_SNAPSHOT_DESC_MAX 1024u
#define SPURS_JOB_SNAPSHOT_IMAGE_MAX 262144u
#define SPURS_JOB_CAPTURE_MAX_COUNT 16u
#define SPURS_JOB_CAPTURE_MAX_BYTES (16ull * (1024ull + 262144ull + 4ull))

typedef enum spurs_job_snapshot_status {
    SPURS_JOB_SNAPSHOT_OK = 0,
    SPURS_JOB_SNAPSHOT_DESCRIPTOR_SIZE,
    SPURS_JOB_SNAPSHOT_ALIGNMENT,
    SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN,
    SPURS_JOB_SNAPSHOT_UNSUPPORTED_BINARY_EA,
    SPURS_JOB_SNAPSHOT_IMAGE_SIZE,
    SPURS_JOB_SNAPSHOT_IMAGE_SPAN,
    SPURS_JOB_SNAPSHOT_NO_MEMORY
} spurs_job_snapshot_status;

typedef struct spurs_job_snapshot {
    uint32_t job_ea;
    uint32_t descriptor_size;
    uint64_t raw_binary_ea;
    uint32_t binary_flags;
    uint32_t binary_ea;
    uint32_t image_size;
    uint64_t fingerprint;
    uint8_t* descriptor;
    uint8_t* image;
    /* Non-owning mapping provenance, valid through capture. Never serialized. */
    const void* guest_base;
    uint64_t guest_limit;
} spurs_job_snapshot;

/* Explicit guest limit in bytes, <= 4 GiB. base NULL permits identity mapping.
 * Descriptor EA must be 16-byte aligned; size is 48..1024, a multiple of 16.
 * Binary must be 16-byte aligned, low flags zero, high u32 zero. Full spans
 * must be >=64 KiB and OS-readable. Safe OS copies fail on remap/read faults.
 * Caller must supply an empty out and release any successful snapshot.
 * Captures can race guest writes: neither copy promises a coherent guest epoch. */
spurs_job_snapshot_status spurs_job_snapshot_acquire(
    const void* base, uint64_t guest_limit, uint64_t job_ea,
    uint32_t descriptor_size, spurs_job_snapshot* out);
void spurs_job_snapshot_release(spurs_job_snapshot* snapshot);
const char* spurs_job_snapshot_status_name(spurs_job_snapshot_status status);

typedef enum spurs_job_capture_status {
    SPURS_JOB_CAPTURE_DISABLED = 0,
    SPURS_JOB_CAPTURE_WRITTEN,
    SPURS_JOB_CAPTURE_LIMIT,
    SPURS_JOB_CAPTURE_ERROR
} spurs_job_capture_status;

/* Reads FOLKLORE_SPU_CAPTURE_DIR. Existing absolute private directory;
 * no mkdir, exclusive creation, no overwrite; maximum 16 attempts/process and
 * bounded descriptor+image+optional four-byte auxiliary total. Writes PREFIX.desc.bin, PREFIX.image.bin,
 * PREFIX.json. Returns complete prefix in out_prefix on success, else empty.
 * Metadata schema version 1 by default, version 2 with auxiliary fields;
 * runtime fingerprint only (launcher adds SHA256).
 * Optional FOLKLORE_SPU_CAPTURE_AUX_EA must be 0x followed by 1..8 hex digits.
 * With capture enabled, copies exactly four bytes using acquire's guest mapping
 * and writes PREFIX.aux.bin plus auxiliary_* metadata. Separate copy, no epoch
 * coherence. Invalid/unreadable auxiliary spans fail the whole capture. With
 * DIR unset/empty, no auxiliary parse/read occurs. Mapping must remain valid.
 * No guest pointer follows, native execution or success-result synthesis. */
spurs_job_capture_status spurs_job_snapshot_capture(
    const spurs_job_snapshot* snapshot, char* out_prefix, uint32_t prefix_capacity);

#ifdef __cplusplus
}
#endif
#endif
