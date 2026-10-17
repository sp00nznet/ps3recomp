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

int mfc_strict_guest_range_verified(uint32_t ea, uint32_t size); /* spu_vm_pagemap.c */
int mfc_strict_host_copy(void* local, uintptr_t remote, uint32_t size, int writing);
extern uint8_t* vm_base;

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
    /* Guest pages proven committed read-write under the host's guarantee
     * skip the region walk; the copy below still reports any fault. */
    if (base == (const void*)vm_base && mfc_strict_guest_range_verified((uint32_t)ea, length))
        return mfc_strict_host_copy(destination, address, length, 0);
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
    out->guest_base = base;
    out->guest_limit = guest_limit;
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

static int auxiliary_ea_parse(const char* text, uint32_t* ea)
{
    if (!text || text[0] != '0' || text[1] != 'x') return 0;
    uint32_t value = 0;
    unsigned digits = 0;
    for (const char* p = text + 2; *p; ++p) {
        unsigned nibble;
        if (*p >= '0' && *p <= '9') nibble = (unsigned)(*p - '0');
        else if (*p >= 'a' && *p <= 'f') nibble = (unsigned)(*p - 'a') + 10;
        else if (*p >= 'A' && *p <= 'F') nibble = (unsigned)(*p - 'A') + 10;
        else return 0;
        if (++digits > 8) return 0;
        value = (value << 4) | nibble;
    }
    if (!digits) return 0;
    *ea = value;
    return 1;
}

typedef struct capture_span { uint32_t ea, size; uint8_t* data; } capture_span;
static int bounded_text(const char* text, size_t maximum, size_t* length)
{
    if (!text) return 0;
    for (size_t i = 0; i <= maximum; ++i) {
        if (!text[i]) { *length = i; return 1; }
    }
    return 0;
}
static int capture_index_parse(const char* text, unsigned* index)
{
    size_t n;
    if (!bounded_text(text, 2, &n) || !n || (n > 1 && text[0] == '0')) return 0;
    unsigned value = 0;
    for (size_t i = 0; i < n; ++i) {
        if (text[i] < '0' || text[i] > '9') return 0;
        value = value * 10 + (unsigned)(text[i] - '0');
    }
    if (value >= SPURS_JOB_CAPTURE_MAX_COUNT) return 0;
    *index = value; return 1;
}
static int capture_spans_parse(const char* text, capture_span spans[2],
                               unsigned* count, uint32_t* total)
{
    size_t n;
    if (!bounded_text(text, 64, &n) || !n) return 0;
    size_t at = 0; *count = 0; *total = 0;
    while (at < n) {
        if (*count == SPURS_JOB_CAPTURE_MAX_SPANS || n - at < 4 ||
            text[at++] != '0' || text[at++] != 'x') return 0;
        uint32_t ea = 0, size = 0; unsigned digits = 0;
        while (at < n && text[at] != ':') {
            unsigned nibble; char c = text[at++];
            if (c >= '0' && c <= '9') nibble = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = (unsigned)(c - 'a') + 10;
            else if (c >= 'A' && c <= 'F') nibble = (unsigned)(c - 'A') + 10;
            else return 0;
            if (++digits > 8) return 0;
            ea = (ea << 4) | nibble;
        }
        if (!digits || at == n || text[at++] != ':' || at == n || text[at] == '0') return 0;
        digits = 0;
        while (at < n && text[at] != ',') {
            char c = text[at++];
            if (c < '0' || c > '9' || ++digits > 6) return 0;
            size = size * 10 + (unsigned)(c - '0');
            if (size > SPURS_JOB_CAPTURE_MAX_SPAN_BYTES) return 0;
        }
        if (!digits || !size || ea < 0x10000u ||
            (uint64_t)ea + size > UINT64_C(0x100000000) ||
            size > SPURS_JOB_CAPTURE_MAX_SPAN_BYTES - *total) return 0;
        for (unsigned i = 0; i < *count; ++i)
            if ((uint64_t)ea < (uint64_t)spans[i].ea + spans[i].size &&
                (uint64_t)spans[i].ea < (uint64_t)ea + size) return 0;
        spans[*count].ea = ea; spans[*count].size = size; ++*count; *total += size;
        if (at < n && (++at == n)) return 0;
    }
    return *count != 0;
}

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
    size_t dir_size;
    if (!bounded_text(directory, 3000, &dir_size)) return SPURS_JOB_CAPTURE_ERROR;
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
    const char* auxiliary_env = getenv("FOLKLORE_SPU_CAPTURE_AUX_EA");
    int auxiliary_enabled = auxiliary_env && *auxiliary_env;
    const char* spans_env = getenv("FOLKLORE_SPU_CAPTURE_SPANS");
    const char* index_env = getenv("FOLKLORE_SPU_CAPTURE_SPANS_INDEX");
    capture_span spans[SPURS_JOB_CAPTURE_MAX_SPANS] = {{0}};
    unsigned span_count = 0, selected_index = 0;
    uint32_t span_bytes = 0;
    uint32_t auxiliary_ea = 0;
    while (atomic_flag_test_and_set_explicit(&capture_lock, memory_order_acquire)) { }
    uint64_t bytes = (uint64_t)s->descriptor_size + s->image_size + (auxiliary_enabled ? 4u : 0u);
    if (capture_attempts >= SPURS_JOB_CAPTURE_MAX_COUNT ||
        bytes > SPURS_JOB_CAPTURE_MAX_BYTES - capture_bytes) {
        atomic_flag_clear_explicit(&capture_lock, memory_order_release);
        return SPURS_JOB_CAPTURE_LIMIT;
    }
    unsigned index = capture_attempts++;
    capture_bytes += bytes; /* Failed attempts consume their reservation too. */
    atomic_flag_clear_explicit(&capture_lock, memory_order_release);
    if ((index_env && !spans_env) || (spans_env && auxiliary_enabled) ||
        (index_env && !capture_index_parse(index_env, &selected_index)) ||
        (spans_env && !capture_spans_parse(spans_env, spans, &span_count, &span_bytes)))
        return SPURS_JOB_CAPTURE_ERROR;
    int spans_selected = spans_env && (!index_env || index == selected_index);
    if (spans_selected) {
        while (atomic_flag_test_and_set_explicit(&capture_lock, memory_order_acquire)) { }
        if (span_bytes > SPURS_JOB_CAPTURE_MAX_BYTES - capture_bytes) {
            atomic_flag_clear_explicit(&capture_lock, memory_order_release);
            return SPURS_JOB_CAPTURE_LIMIT;
        }
        capture_bytes += span_bytes;
        atomic_flag_clear_explicit(&capture_lock, memory_order_release);
    }
    if (auxiliary_enabled && !auxiliary_ea_parse(auxiliary_env, &auxiliary_ea))
        return SPURS_JOB_CAPTURE_ERROR;
    uint8_t auxiliary[4];
    if (auxiliary_enabled && !checked_read(s->guest_base, s->guest_limit,
                                           auxiliary_ea, sizeof auxiliary, auxiliary))
        return SPURS_JOB_CAPTURE_ERROR;
    char leaf[128], prefix[4096], descriptor_path[4096], image_path[4096], metadata_path[4096], auxiliary_path[4096];
    int n = snprintf(leaf, sizeof leaf, "spurs-job-%08X-%016llX-%02u", s->job_ea,
                     (unsigned long long)s->fingerprint, index);
    if (n < 0 || (size_t)n >= sizeof leaf) return SPURS_JOB_CAPTURE_ERROR;
    n = snprintf(prefix, sizeof prefix, "%s/%s", directory, leaf);
    if (n < 0 || (size_t)n >= sizeof prefix || (uint32_t)n >= capacity) return SPURS_JOB_CAPTURE_ERROR;
    if (snprintf(descriptor_path, sizeof descriptor_path, "%s.desc.bin", prefix) >= (int)sizeof descriptor_path ||
        snprintf(image_path, sizeof image_path, "%s.image.bin", prefix) >= (int)sizeof image_path ||
        snprintf(auxiliary_path, sizeof auxiliary_path, "%s.aux.bin", prefix) >= (int)sizeof auxiliary_path ||
        snprintf(metadata_path, sizeof metadata_path, "%s.json", prefix) >= (int)sizeof metadata_path)
        return SPURS_JOB_CAPTURE_ERROR;
    char auxiliary_metadata[1024] = {0};
    if (auxiliary_enabled) {
        n = snprintf(auxiliary_metadata, sizeof auxiliary_metadata,
            ",\n  \"auxiliary_ea\": \"0x%08X\",\n  \"auxiliary_size\": 4,\n"
            "  \"auxiliary_file\": \"%s.aux.bin\",\n"
            "  \"auxiliary_consistency\": \"separate checked copy; guest not quiesced\"", auxiliary_ea, leaf);
        if (n < 0 || (size_t)n >= sizeof auxiliary_metadata) return SPURS_JOB_CAPTURE_ERROR;
    }
    char span_paths[SPURS_JOB_CAPTURE_MAX_SPANS][4096];
    if (spans_selected) {
        size_t used = 0;
        n = snprintf(auxiliary_metadata, sizeof auxiliary_metadata, ",\n  \"capture_spans\": [");
        if (n < 0 || (size_t)n >= sizeof auxiliary_metadata) return SPURS_JOB_CAPTURE_ERROR;
        used = (size_t)n;
        for (unsigned i = 0; i < span_count; ++i) {
            n = snprintf(span_paths[i], sizeof span_paths[i], "%s.span-%02u.bin", prefix, i);
            if (n < 0 || (size_t)n >= sizeof span_paths[i]) return SPURS_JOB_CAPTURE_ERROR;
            n = snprintf(auxiliary_metadata + used, sizeof auxiliary_metadata - used,
                "%s\n    {\"ea\":\"0x%08X\",\"size\":%u,\"file\":\"%s.span-%02u.bin\","
                "\"consistency\":\"separate checked copy; guest not quiesced\"}",
                i ? "," : "", spans[i].ea, spans[i].size, leaf, i);
            if (n < 0 || (size_t)n >= sizeof auxiliary_metadata - used) return SPURS_JOB_CAPTURE_ERROR;
            used += (size_t)n;
        }
        n = snprintf(auxiliary_metadata + used, sizeof auxiliary_metadata - used, "\n  ]");
        if (n < 0 || (size_t)n >= sizeof auxiliary_metadata - used) return SPURS_JOB_CAPTURE_ERROR;
    }
    char metadata[2048];
    n = snprintf(metadata, sizeof metadata,
        "{\n  \"schema_version\": %u,\n  \"job_ea\": \"0x%08X\",\n  \"descriptor_size\": %u,\n"
        "  \"raw_binary_ea\": \"0x%016llX\",\n  \"binary_flags\": %u,\n  \"binary_ea\": \"0x%08X\",\n"
        "  \"image_size\": %u,\n  \"runtime_fingerprint\": \"0x%016llX\",\n"
        "  \"fingerprint_seed\": \"1469598103934665603\",\n"
        "  \"descriptor_file\": \"%s.desc.bin\",\n  \"image_file\": \"%s.image.bin\",\n"
        "  \"snapshot_consistency\": \"separate checked copies; guest not quiesced\"%s\n}\n",
        spans_selected ? 3u : auxiliary_enabled ? 2u : 1u, s->job_ea, s->descriptor_size, (unsigned long long)s->raw_binary_ea, s->binary_flags,
        s->binary_ea, s->image_size, (unsigned long long)s->fingerprint, leaf, leaf, auxiliary_metadata);
    if (n < 0 || (size_t)n >= sizeof metadata) return SPURS_JOB_CAPTURE_ERROR;
    /* Own all explicit copies before creating even the descriptor file. */
    if (spans_selected) for (unsigned i = 0; i < span_count; ++i) {
        spans[i].data = (uint8_t*)malloc(spans[i].size);
        if (!spans[i].data || !checked_read(s->guest_base, s->guest_limit,
                                          spans[i].ea, spans[i].size, spans[i].data)) {
            for (unsigned j = 0; j <= i; ++j) free(spans[j].data);
            return SPURS_JOB_CAPTURE_ERROR;
        }
    }
    int desc_created = 0, image_created = 0, meta_created = 0, aux_created = 0;
    int span_created[SPURS_JOB_CAPTURE_MAX_SPANS] = {0};
    int okay = exclusive_write(descriptor_path, s->descriptor, s->descriptor_size, &desc_created) &&
        exclusive_write(image_path, s->image, s->image_size, &image_created) &&
        (!auxiliary_enabled || exclusive_write(auxiliary_path, auxiliary, sizeof auxiliary, &aux_created));
    if (okay && spans_selected) for (unsigned i = 0; i < span_count; ++i)
        if (!exclusive_write(span_paths[i], spans[i].data, spans[i].size, &span_created[i])) { okay = 0; break; }
    if (okay) okay = exclusive_write(metadata_path, metadata, (uint32_t)n, &meta_created);
    for (unsigned i = 0; i < span_count; ++i) free(spans[i].data);
    if (!okay) {
        /* Only remove files this attempt exclusively created. Never overwrite. */
        if (meta_created) (void)remove(metadata_path);
        if (aux_created) (void)remove(auxiliary_path);
        for (unsigned i = 0; i < span_count; ++i) if (span_created[i]) (void)remove(span_paths[i]);
        if (image_created) (void)remove(image_path);
        if (desc_created) (void)remove(descriptor_path);
        return SPURS_JOB_CAPTURE_ERROR;
    }
    memcpy(out_prefix, prefix, strlen(prefix) + 1);
    return SPURS_JOB_CAPTURE_WRITTEN;
}
