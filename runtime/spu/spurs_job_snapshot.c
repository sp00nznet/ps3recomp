#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "spurs_job_snapshot.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#if defined(__linux__)
#include <sys/uio.h>
#endif
#endif

static int checked_read(const void* base, uint64_t limit, uint64_t ea,
                        uint32_t length, void* destination)
{
    if (!limit || limit > 0x100000000ull || ea < 0x10000ull ||
        ea > limit || length > limit - ea || !length) return 0;
    uintptr_t address = (uintptr_t)base;
    if (ea > UINTPTR_MAX - address) return 0;
    address += (uintptr_t)ea;
    if (length > UINTPTR_MAX - address) return 0;
    uintptr_t current = address, end = address + length;
#ifdef _WIN32
    while (current < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void*)current, &mbi, sizeof mbi) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        DWORD protection = mbi.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
            protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return 0;
        uintptr_t region = (uintptr_t)mbi.BaseAddress;
        if (mbi.RegionSize > UINTPTR_MAX - region) return 0;
        uintptr_t next = region + mbi.RegionSize;
        if (region > current || next <= current) return 0;
        current = next;
    }
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void*)address,
                             destination, length, &copied) && copied == length;
#elif defined(__linux__)
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[1024];
    while (current < end && fgets(line, sizeof line, maps)) {
        unsigned long long first = 0, last = 0;
        char permissions[5] = {0};
        if (sscanf(line, "%llx-%llx %4s", &first, &last, permissions) != 3 || last <= current) continue;
        if (first > current || permissions[0] != 'r' || last > UINTPTR_MAX) break;
        current = (uintptr_t)(last < end ? last : end);
    }
    int readable = current == end;
    if (fclose(maps) != 0 || !readable) return 0;
    struct iovec local = { destination, length };
    struct iovec remote = { (void*)address, length };
    ssize_t copied;
    do { copied = process_vm_readv(getpid(), &local, 1, &remote, 1, 0); }
    while (copied < 0 && errno == EINTR);
    return copied == (ssize_t)length;
#else
    (void)destination; (void)current; (void)end;
    return 0; /* No unsafe memcpy fallback on an unsupported host. */
#endif
}

void spurs_job_snapshot_release(spurs_job_snapshot* snapshot)
{
    if (!snapshot) return;
    free(snapshot->descriptor);
    free(snapshot->image);
    memset(snapshot, 0, sizeof *snapshot);
}

spurs_job_snapshot_status spurs_job_snapshot_acquire(
    const void* base, uint64_t guest_limit, uint64_t job_ea,
    uint32_t descriptor_size, spurs_job_snapshot* out)
{
    if (!out) return SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN;
    memset(out, 0, sizeof *out);
    if (descriptor_size < SPURS_JOB_SNAPSHOT_DESC_MIN ||
        descriptor_size > SPURS_JOB_SNAPSHOT_DESC_MAX || (descriptor_size & 15u))
        return SPURS_JOB_SNAPSHOT_DESCRIPTOR_SIZE;
    if (job_ea & 15u) return SPURS_JOB_SNAPSHOT_ALIGNMENT;
    if (job_ea > UINT32_MAX) return SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN;
    spurs_job_snapshot_status status = SPURS_JOB_SNAPSHOT_NO_MEMORY;
    out->descriptor = (uint8_t*)malloc(descriptor_size);
    if (!out->descriptor) goto failed;
    status = SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN;
    if (!checked_read(base, guest_limit, job_ea, descriptor_size, out->descriptor)) goto failed;
    out->job_ea = (uint32_t)job_ea;
    out->descriptor_size = descriptor_size;
    for (unsigned i = 0; i < 8; ++i)
        out->raw_binary_ea = (out->raw_binary_ea << 8) | out->descriptor[i];
    out->binary_flags = (uint32_t)(out->raw_binary_ea & 7ull);
    status = SPURS_JOB_SNAPSHOT_UNSUPPORTED_BINARY_EA;
    if (out->raw_binary_ea > UINT32_MAX || out->binary_flags) goto failed;
    out->binary_ea = (uint32_t)out->raw_binary_ea;
    status = SPURS_JOB_SNAPSHOT_ALIGNMENT;
    if (out->binary_ea & 15u) goto failed;
    out->image_size = ((uint32_t)out->descriptor[8] << 8 | out->descriptor[9]) << 4;
    status = SPURS_JOB_SNAPSHOT_IMAGE_SIZE;
    if (!out->image_size || out->image_size > SPURS_JOB_SNAPSHOT_IMAGE_MAX) goto failed;
    status = SPURS_JOB_SNAPSHOT_NO_MEMORY;
    out->image = (uint8_t*)malloc(out->image_size);
    if (!out->image) goto failed;
    status = SPURS_JOB_SNAPSHOT_IMAGE_SPAN;
    if (!checked_read(base, guest_limit, out->binary_ea, out->image_size, out->image)) goto failed;
    out->fingerprint = 1469598103934665603ull; /* Exact existing runtime seed. */
    for (uint32_t i = 0; i < out->image_size; ++i) {
        out->fingerprint ^= out->image[i];
        out->fingerprint *= 1099511628211ull;
    }
    return SPURS_JOB_SNAPSHOT_OK;
failed:
    spurs_job_snapshot_release(out);
    return status;
}

const char* spurs_job_snapshot_status_name(spurs_job_snapshot_status status)
{
    switch (status) {
    case SPURS_JOB_SNAPSHOT_OK: return "ok";
    case SPURS_JOB_SNAPSHOT_DESCRIPTOR_SIZE: return "descriptor-size";
    case SPURS_JOB_SNAPSHOT_ALIGNMENT: return "alignment";
    case SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN: return "descriptor-span";
    case SPURS_JOB_SNAPSHOT_UNSUPPORTED_BINARY_EA: return "unsupported-binary-ea-or-flags";
    case SPURS_JOB_SNAPSHOT_IMAGE_SIZE: return "image-size";
    case SPURS_JOB_SNAPSHOT_IMAGE_SPAN: return "image-span";
    case SPURS_JOB_SNAPSHOT_NO_MEMORY: return "no-memory";
    default: return "unknown";
    }
}

static atomic_flag capture_lock = ATOMIC_FLAG_INIT;
static unsigned capture_attempts;
static uint64_t capture_bytes;

static int exclusive_write(const char* path, const void* data, uint32_t size, int* created)
{
    *created = 0;
#ifdef _WIN32
    int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif
    if (fd < 0) return 0;
    *created = 1;
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t done = 0;
    int okay = 1;
    while (done < size) {
#ifdef _WIN32
        int count = _write(fd, bytes + done, size - done);
#else
        ssize_t count = write(fd, bytes + done, size - done);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { okay = 0; break; }
        done += (uint32_t)count;
    }
#ifdef _WIN32
    if (_close(fd) != 0) okay = 0;
#else
    if (close(fd) != 0) okay = 0;
#endif
    return okay;
}

spurs_job_capture_status spurs_job_snapshot_capture(
    const spurs_job_snapshot* s, char* out_prefix, uint32_t capacity)
{
    if (out_prefix && capacity) out_prefix[0] = 0;
    const char* directory = getenv("FOLKLORE_SPU_CAPTURE_DIR");
    if (!directory || !*directory) return SPURS_JOB_CAPTURE_DISABLED;
    if (!s || !s->descriptor || !s->image || !out_prefix || !capacity ||
        s->descriptor_size < SPURS_JOB_SNAPSHOT_DESC_MIN || s->descriptor_size > SPURS_JOB_SNAPSHOT_DESC_MAX ||
        !s->image_size || s->image_size > SPURS_JOB_SNAPSHOT_IMAGE_MAX) return SPURS_JOB_CAPTURE_ERROR;
    size_t dir_size = strlen(directory);
    if (dir_size > 3000) return SPURS_JOB_CAPTURE_ERROR;
#ifdef _WIN32
    if (dir_size < 3 || directory[1] != ':' ||
        (directory[2] != '/' && directory[2] != '\\')) return SPURS_JOB_CAPTURE_ERROR;
    DWORD attrs = GetFileAttributesA(directory);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return SPURS_JOB_CAPTURE_ERROR;
#else
    struct stat info;
    if (directory[0] != '/' || lstat(directory, &info) || !S_ISDIR(info.st_mode))
        return SPURS_JOB_CAPTURE_ERROR;
#endif
    while (atomic_flag_test_and_set_explicit(&capture_lock, memory_order_acquire)) { }
    uint64_t bytes = (uint64_t)s->descriptor_size + s->image_size;
    if (capture_attempts >= SPURS_JOB_CAPTURE_MAX_COUNT ||
        bytes > SPURS_JOB_CAPTURE_MAX_BYTES - capture_bytes) {
        atomic_flag_clear_explicit(&capture_lock, memory_order_release);
        return SPURS_JOB_CAPTURE_LIMIT;
    }
    unsigned index = capture_attempts++;
    capture_bytes += bytes; /* Failed attempts consume their reservation too. */
    atomic_flag_clear_explicit(&capture_lock, memory_order_release);
    char leaf[128], prefix[4096], descriptor_path[4096], image_path[4096], metadata_path[4096];
    int n = snprintf(leaf, sizeof leaf, "spurs-job-%08X-%016llX-%02u", s->job_ea,
                     (unsigned long long)s->fingerprint, index);
    if (n < 0 || (size_t)n >= sizeof leaf) return SPURS_JOB_CAPTURE_ERROR;
    n = snprintf(prefix, sizeof prefix, "%s/%s", directory, leaf);
    if (n < 0 || (size_t)n >= sizeof prefix || (uint32_t)n >= capacity) return SPURS_JOB_CAPTURE_ERROR;
    if (snprintf(descriptor_path, sizeof descriptor_path, "%s.desc.bin", prefix) >= (int)sizeof descriptor_path ||
        snprintf(image_path, sizeof image_path, "%s.image.bin", prefix) >= (int)sizeof image_path ||
        snprintf(metadata_path, sizeof metadata_path, "%s.json", prefix) >= (int)sizeof metadata_path)
        return SPURS_JOB_CAPTURE_ERROR;
    char metadata[2048];
    n = snprintf(metadata, sizeof metadata,
        "{\n  \"schema_version\": 1,\n  \"job_ea\": \"0x%08X\",\n  \"descriptor_size\": %u,\n"
        "  \"raw_binary_ea\": \"0x%016llX\",\n  \"binary_flags\": %u,\n  \"binary_ea\": \"0x%08X\",\n"
        "  \"image_size\": %u,\n  \"runtime_fingerprint\": \"0x%016llX\",\n"
        "  \"fingerprint_seed\": \"1469598103934665603\",\n"
        "  \"descriptor_file\": \"%s.desc.bin\",\n  \"image_file\": \"%s.image.bin\",\n"
        "  \"snapshot_consistency\": \"separate checked copies; guest not quiesced\"\n}\n",
        s->job_ea, s->descriptor_size, (unsigned long long)s->raw_binary_ea, s->binary_flags,
        s->binary_ea, s->image_size, (unsigned long long)s->fingerprint, leaf, leaf);
    if (n < 0 || (size_t)n >= sizeof metadata) return SPURS_JOB_CAPTURE_ERROR;
    int desc_created = 0, image_created = 0, meta_created = 0;
    if (!exclusive_write(descriptor_path, s->descriptor, s->descriptor_size, &desc_created) ||
        !exclusive_write(image_path, s->image, s->image_size, &image_created) ||
        !exclusive_write(metadata_path, metadata, (uint32_t)n, &meta_created)) {
        /* Only remove files this attempt exclusively created. Never overwrite. */
        if (meta_created) (void)remove(metadata_path);
        if (image_created) (void)remove(image_path);
        if (desc_created) (void)remove(descriptor_path);
        return SPURS_JOB_CAPTURE_ERROR;
    }
    memcpy(out_prefix, prefix, strlen(prefix) + 1);
    return SPURS_JOB_CAPTURE_WRITTEN;
}
