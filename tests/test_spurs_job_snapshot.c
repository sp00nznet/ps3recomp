/* Synthetic data only. Link runtime/spu/spurs_job_snapshot.c directly for the
 * helper modes. Define SPURS_SNAPSHOT_PRODUCTION and link the actual runtime
 * for production-miss / production-invalid children (expected exit 86).
 * Every capture mode requires a fresh, existing, empty absolute directory.
 * No production functions or strict guards are replaced by test stubs. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(SPURS_SNAPSHOT_NATIVE) && !defined(SPURS_SNAPSHOT_PRODUCTION)
#define SPURS_SNAPSHOT_PRODUCTION
#endif
#include "spurs_job_snapshot.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <dirent.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define VM_SIZE UINT64_C(0x100000000)
#define JOB_EA 0x20000u
#define IMAGE_EA 0x20000000u
#define IMAGE_SIZE 4096u
#define DESC_SIZE 128u
static uint8_t* memory;
static size_t page_size;
static uint8_t descriptor[DESC_SIZE], image_bytes[IMAGE_SIZE];

static void env_set(const char* name, const char* value)
{
#ifdef _WIN32
    CHECK(_putenv_s(name, value ? value : "") == 0);
#else
    CHECK(value ? setenv(name, value, 1) == 0 : unsetenv(name) == 0);
#endif
}
static void be64(uint8_t* p, uint64_t value)
{
    for (int i = 7; i >= 0; --i) { p[i] = (uint8_t)value; value >>= 8; }
}
static void image_header(uint64_t ea, uint32_t size)
{
    be64(memory + JOB_EA, ea);
    memory[JOB_EA + 8] = (uint8_t)((size >> 4) >> 8);
    memory[JOB_EA + 9] = (uint8_t)(size >> 4);
}
static uint64_t reference_fingerprint(const uint8_t* bytes, size_t size)
{
    uint64_t value = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < size; ++i) value = (value ^ bytes[i]) * UINT64_C(1099511628211);
    return value;
}
static void commit(uint32_t ea, size_t size)
{
    CHECK((ea % page_size) == 0 && (size % page_size) == 0);
#ifdef _WIN32
    CHECK(VirtualAlloc(memory + ea, size, MEM_COMMIT, PAGE_READWRITE) == memory + ea);
#else
    CHECK(mprotect(memory + ea, size, PROT_READ | PROT_WRITE) == 0);
#endif
}
static void protection(uint32_t ea, size_t size, int readable, int writable)
{
#ifdef _WIN32
    DWORD old;
    CHECK(VirtualProtect(memory + ea, size,
        writable ? PAGE_READWRITE : readable ? PAGE_READONLY : PAGE_NOACCESS, &old));
#else
    CHECK(mprotect(memory + ea, size, (readable ? PROT_READ : 0) | (writable ? PROT_WRITE : 0)) == 0);
#endif
}
static void setup(void)
{
    CHECK(sizeof(void*) >= 8);
#ifdef _WIN32
    SYSTEM_INFO info; GetSystemInfo(&info); page_size = info.dwPageSize;
    memory = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)VM_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(memory != NULL);
#else
    long size = sysconf(_SC_PAGESIZE); CHECK(size > 0); page_size = (size_t)size;
    memory = (uint8_t*)mmap(NULL, (size_t)VM_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED);
#endif
    CHECK(page_size <= 65536 && (IMAGE_SIZE % 16) == 0);
    commit(JOB_EA, page_size * 2);
    commit(IMAGE_EA, page_size * 3);
    for (size_t i = 0; i < sizeof descriptor; ++i) descriptor[i] = (uint8_t)(i * 13 + 5);
    for (size_t i = 0; i < sizeof image_bytes; ++i) image_bytes[i] = (uint8_t)(i * 37 + 11);
    be64(descriptor, IMAGE_EA);
    descriptor[8] = (uint8_t)((IMAGE_SIZE >> 4) >> 8);
    descriptor[9] = (uint8_t)(IMAGE_SIZE >> 4);
    memcpy(memory + JOB_EA, descriptor, sizeof descriptor);
    memcpy(memory + IMAGE_EA, image_bytes, sizeof image_bytes);
    memory[JOB_EA + DESC_SIZE] = 0xA7;
    memory[IMAGE_EA + IMAGE_SIZE] = 0xB9;
}
static void teardown(void)
{
#ifdef _WIN32
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
#else
    CHECK(munmap(memory, (size_t)VM_SIZE) == 0);
#endif
}
static spurs_job_snapshot acquire_good(void)
{
    spurs_job_snapshot s = {0};
    CHECK(spurs_job_snapshot_acquire(memory, VM_SIZE, JOB_EA, DESC_SIZE, &s) == SPURS_JOB_SNAPSHOT_OK);
    CHECK(s.job_ea == JOB_EA && s.descriptor_size == DESC_SIZE);
    CHECK(s.raw_binary_ea == IMAGE_EA && s.binary_ea == IMAGE_EA && s.binary_flags == 0);
    CHECK(s.image_size == IMAGE_SIZE);
    CHECK(s.fingerprint == reference_fingerprint(image_bytes, sizeof image_bytes));
    CHECK(memcmp(s.descriptor, descriptor, DESC_SIZE) == 0);
    CHECK(memcmp(s.image, image_bytes, IMAGE_SIZE) == 0);
    return s;
}
static void reject(const void* base, uint64_t limit, uint64_t ea, uint32_t size,
                   spurs_job_snapshot_status expected)
{
    struct { uint64_t before; spurs_job_snapshot s; uint64_t after; } guarded = {0};
    guarded.before = UINT64_C(0x13579BDF2468ACE0); guarded.after = ~guarded.before;
    CHECK(spurs_job_snapshot_acquire(base, limit, ea, size, &guarded.s) == expected);
    CHECK(guarded.before == UINT64_C(0x13579BDF2468ACE0) && guarded.after == ~guarded.before);
    CHECK(guarded.s.descriptor == NULL && guarded.s.image == NULL);
    CHECK(memory[JOB_EA + DESC_SIZE] == 0xA7);
    spurs_job_snapshot_release(&guarded.s);
}
static void unit(void)
{
    spurs_job_snapshot s = acquire_good();
    /* Copies remain owned and exact after both source regions change. */
    memory[JOB_EA + 100] ^= 0xFF; memory[IMAGE_EA + 100] ^= 0xFF;
    CHECK(memcmp(s.descriptor, descriptor, DESC_SIZE) == 0 && memcmp(s.image, image_bytes, IMAGE_SIZE) == 0);
    memcpy(memory + JOB_EA, descriptor, DESC_SIZE); memcpy(memory + IMAGE_EA, image_bytes, IMAGE_SIZE);
    spurs_job_snapshot_release(&s); CHECK(!s.descriptor && !s.image && !s.image_size);
    reject(memory, VM_SIZE, JOB_EA, 32, SPURS_JOB_SNAPSHOT_DESCRIPTOR_SIZE);
    reject(memory, VM_SIZE, JOB_EA, 129, SPURS_JOB_SNAPSHOT_DESCRIPTOR_SIZE);
    reject(memory, VM_SIZE, JOB_EA, 1040, SPURS_JOB_SNAPSHOT_DESCRIPTOR_SIZE);
    reject(memory, VM_SIZE, JOB_EA + 1, DESC_SIZE, SPURS_JOB_SNAPSHOT_ALIGNMENT);
    reject(memory, VM_SIZE, 0, DESC_SIZE, SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN);
    reject(memory, VM_SIZE, 0xFFF0, DESC_SIZE, SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN);
    reject(memory, VM_SIZE, 0x40000, DESC_SIZE, SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN);
    reject(memory, VM_SIZE, UINT64_C(0x100000000), DESC_SIZE, SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN);
    reject(memory, VM_SIZE, UINT64_C(0xFFFFFFF0), DESC_SIZE, SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN);
    reject(memory, 0, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN);
    reject(memory, VM_SIZE + 1, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN);
    reject((void*)(UINTPTR_MAX - 0x1000), VM_SIZE, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN);
    for (uint32_t flag = 1; flag <= 7; ++flag) {
        image_header(IMAGE_EA | flag, IMAGE_SIZE);
        reject(memory, VM_SIZE, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_UNSUPPORTED_BINARY_EA);
    }
    image_header((UINT64_C(1) << 32) | IMAGE_EA, IMAGE_SIZE);
    reject(memory, VM_SIZE, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_UNSUPPORTED_BINARY_EA);
    image_header(IMAGE_EA + 8, IMAGE_SIZE);
    reject(memory, VM_SIZE, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_ALIGNMENT);
    image_header(IMAGE_EA, 0);
    reject(memory, VM_SIZE, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_IMAGE_SIZE);
    image_header(IMAGE_EA, SPURS_JOB_SNAPSHOT_IMAGE_MAX + 16);
    reject(memory, VM_SIZE, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_IMAGE_SIZE);
    image_header(0, IMAGE_SIZE);
    reject(memory, VM_SIZE, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_IMAGE_SPAN);
    image_header(0xFFF0, IMAGE_SIZE);
    reject(memory, VM_SIZE, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_IMAGE_SPAN);
    image_header(0x40000, IMAGE_SIZE);
    reject(memory, VM_SIZE, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_IMAGE_SPAN);
    image_header(0xFFFFFFF0, IMAGE_SIZE);
    reject(memory, VM_SIZE, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_IMAGE_SPAN);
    image_header(IMAGE_EA, IMAGE_SIZE);
    reject(memory, IMAGE_EA + IMAGE_SIZE - 1, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_IMAGE_SPAN);
    /* Read-only pages are valid sources; a readable first/last page cannot
     * hide an inaccessible middle page. */
    protection(IMAGE_EA, page_size, 1, 0); s = acquire_good(); spurs_job_snapshot_release(&s);
    protection(IMAGE_EA, page_size, 1, 1);
    image_header(IMAGE_EA, (uint32_t)(page_size * 3));
    protection(IMAGE_EA + (uint32_t)page_size, page_size, 0, 0);
    reject(memory, VM_SIZE, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_IMAGE_SPAN);
    protection(IMAGE_EA + (uint32_t)page_size, page_size, 1, 1);
    CHECK(memory[IMAGE_EA + IMAGE_SIZE] == 0xB9);
    image_header(IMAGE_EA, IMAGE_SIZE);
    /* Header readable, descriptor tail inaccessible. */
    uint32_t crossing = JOB_EA + (uint32_t)page_size - 64;
    memcpy(memory + crossing, descriptor, DESC_SIZE);
    protection(JOB_EA + (uint32_t)page_size, page_size, 0, 0);
    reject(memory, VM_SIZE, crossing, DESC_SIZE, SPURS_JOB_SNAPSHOT_DESCRIPTOR_SPAN);
    protection(JOB_EA + (uint32_t)page_size, page_size, 1, 1);
    protection(JOB_EA, page_size, 1, 0); s = acquire_good(); spurs_job_snapshot_release(&s);
    protection(JOB_EA, page_size, 1, 1);
#ifdef _WIN32
    DWORD old; MEMORY_BASIC_INFORMATION info;
    CHECK(VirtualProtect(memory + IMAGE_EA, page_size, PAGE_READWRITE | PAGE_GUARD, &old));
    reject(memory, VM_SIZE, JOB_EA, DESC_SIZE, SPURS_JOB_SNAPSHOT_IMAGE_SPAN);
    CHECK(VirtualQuery(memory + IMAGE_EA, &info, sizeof info) != 0 && (info.Protect & PAGE_GUARD));
    protection(IMAGE_EA, page_size, 1, 1);
#endif
    /* Both inclusive descriptor boundaries and the maximum qualified image
     * must work, rather than rejecting everything outside the common case. */
    commit(IMAGE_EA, SPURS_JOB_SNAPSHOT_IMAGE_MAX);
    for (uint32_t i = 0; i < SPURS_JOB_SNAPSHOT_IMAGE_MAX; ++i)
        memory[IMAGE_EA + i] = (uint8_t)(i * 19 + 3);
    image_header(IMAGE_EA, SPURS_JOB_SNAPSHOT_IMAGE_MAX);
    CHECK(spurs_job_snapshot_acquire(memory, VM_SIZE, JOB_EA, SPURS_JOB_SNAPSHOT_DESC_MAX, &s) == SPURS_JOB_SNAPSHOT_OK);
    CHECK(s.descriptor_size == SPURS_JOB_SNAPSHOT_DESC_MAX && s.image_size == SPURS_JOB_SNAPSHOT_IMAGE_MAX);
    CHECK(memcmp(s.descriptor, memory + JOB_EA, SPURS_JOB_SNAPSHOT_DESC_MAX) == 0);
    CHECK(memcmp(s.image, memory + IMAGE_EA, SPURS_JOB_SNAPSHOT_IMAGE_MAX) == 0);
    CHECK(s.fingerprint == reference_fingerprint(memory + IMAGE_EA, SPURS_JOB_SNAPSHOT_IMAGE_MAX));
    spurs_job_snapshot_release(&s);
    CHECK(spurs_job_snapshot_acquire(memory, VM_SIZE, JOB_EA, SPURS_JOB_SNAPSHOT_DESC_MIN, &s) == SPURS_JOB_SNAPSHOT_OK);
    CHECK(s.descriptor_size == SPURS_JOB_SNAPSHOT_DESC_MIN);
    CHECK(memcmp(s.descriptor, memory + JOB_EA, SPURS_JOB_SNAPSHOT_DESC_MIN) == 0);
    spurs_job_snapshot_release(&s);
    puts("PASS snapshot: full descriptor, high readable image, ownership, decode, bounds, protections, canaries");
}

static unsigned file_count(const char* directory)
{
    unsigned count = 0;
#ifdef _WIN32
    char path[4096]; CHECK(snprintf(path, sizeof path, "%s/*", directory) < (int)sizeof path);
    WIN32_FIND_DATAA data; HANDLE find = FindFirstFileA(path, &data);
    if (find == INVALID_HANDLE_VALUE) { CHECK(GetLastError() == ERROR_FILE_NOT_FOUND); return 0; }
    do { if (strcmp(data.cFileName, ".") && strcmp(data.cFileName, "..")) ++count; } while (FindNextFileA(find, &data));
    CHECK(GetLastError() == ERROR_NO_MORE_FILES); CHECK(FindClose(find));
#else
    DIR* dir = opendir(directory); CHECK(dir != NULL); struct dirent* item;
    while ((item = readdir(dir))) if (strcmp(item->d_name, ".") && strcmp(item->d_name, "..")) ++count;
    CHECK(closedir(dir) == 0);
#endif
    return count;
}
static void capture_prefix(char* out, size_t capacity, const char* directory, unsigned index)
{
    int count = snprintf(out, capacity, "%s/spurs-job-%08X-%016llX-%02u", directory, JOB_EA,
        (unsigned long long)reference_fingerprint(image_bytes, IMAGE_SIZE), index);
    CHECK(count > 0 && (size_t)count < capacity);
}
static void read_exact(const char* prefix, const char* suffix, const void* expected, size_t size)
{
    char path[4096]; CHECK(snprintf(path, sizeof path, "%s%s", prefix, suffix) < (int)sizeof path);
    FILE* file = fopen(path, "rb"); CHECK(file != NULL);
    uint8_t* bytes = (uint8_t*)malloc(size); CHECK(bytes != NULL);
    CHECK(fread(bytes, 1, size, file) == size && fgetc(file) == EOF && !ferror(file));
    CHECK(memcmp(bytes, expected, size) == 0); free(bytes); CHECK(fclose(file) == 0);
}
static void verify_capture(const char* prefix)
{
    read_exact(prefix, ".desc.bin", descriptor, DESC_SIZE);
    read_exact(prefix, ".image.bin", image_bytes, IMAGE_SIZE);
    char path[4096], json[4096], expected[128];
    CHECK(snprintf(path, sizeof path, "%s.json", prefix) < (int)sizeof path);
    FILE* file = fopen(path, "rb"); CHECK(file != NULL);
    size_t size = fread(json, 1, sizeof json - 1, file); CHECK(!ferror(file) && feof(file));
    json[size] = 0; CHECK(fclose(file) == 0);
    CHECK(strstr(json, "\"schema_version\": 1") && strstr(json, "\"descriptor_size\": 128"));
    CHECK(strstr(json, "\"image_size\": 4096") && strstr(json, "\"binary_ea\": \"0x20000000\""));
    CHECK(strstr(json, "\"fingerprint_seed\": \"1469598103934665603\""));
    CHECK(snprintf(expected, sizeof expected, "\"runtime_fingerprint\": \"0x%016llX\"",
        (unsigned long long)reference_fingerprint(image_bytes, IMAGE_SIZE)) < (int)sizeof expected);
    CHECK(strstr(json, expected));
}
static void captures(const char* mode, const char* directory)
{
    CHECK(file_count(directory) == 0);
    spurs_job_snapshot s = acquire_good(); char prefix[4096], expected[4096];
    env_set("FOLKLORE_SPU_CAPTURE_DIR", NULL);
    CHECK(spurs_job_snapshot_capture(&s, prefix, sizeof prefix) == SPURS_JOB_CAPTURE_DISABLED);
    CHECK(!prefix[0] && file_count(directory) == 0);
    if (!strcmp(mode, "disabled")) goto done;
    env_set("FOLKLORE_SPU_CAPTURE_DIR", directory);
    if (!strcmp(mode, "capture-errors")) {
        struct { char output[2]; char canary[8]; } tiny;
        memset(&tiny, 0x5A, sizeof tiny);
        CHECK(spurs_job_snapshot_capture(&s, tiny.output, sizeof tiny.output) == SPURS_JOB_CAPTURE_ERROR);
        CHECK(tiny.output[0] == 0 && tiny.output[1] == 0x5A);
        for (size_t i = 0; i < sizeof tiny.canary; ++i) CHECK(tiny.canary[i] == 0x5A);
        CHECK(file_count(directory) == 0);
        env_set("FOLKLORE_SPU_CAPTURE_DIR", "relative-path");
        CHECK(spurs_job_snapshot_capture(&s, prefix, sizeof prefix) == SPURS_JOB_CAPTURE_ERROR && !prefix[0]);
        CHECK(snprintf(expected, sizeof expected, "%s/missing-subdirectory", directory) < (int)sizeof expected);
        env_set("FOLKLORE_SPU_CAPTURE_DIR", expected);
        CHECK(spurs_job_snapshot_capture(&s, prefix, sizeof prefix) == SPURS_JOB_CAPTURE_ERROR && !prefix[0]);
        CHECK(file_count(directory) == 0);
    } else if (!strcmp(mode, "capture-collision") || !strcmp(mode, "capture-metadata-collision")) {
        /* Collide on the SECOND artifact: failed capture must remove only its
         * own descriptor and preserve the pre-existing image byte-for-byte. */
        capture_prefix(expected, sizeof expected, directory, 0);
        const char* suffix = !strcmp(mode, "capture-metadata-collision") ? ".json" : ".image.bin";
        char path[4096]; CHECK(snprintf(path, sizeof path, "%s%s", expected, suffix) < (int)sizeof path);
        FILE* file = fopen(path, "wbx"); CHECK(file != NULL);
        const uint8_t blocker[] = {0x91, 0xA2, 0xB3};
        CHECK(fwrite(blocker, 1, sizeof blocker, file) == sizeof blocker && fclose(file) == 0);
        CHECK(spurs_job_snapshot_capture(&s, prefix, sizeof prefix) == SPURS_JOB_CAPTURE_ERROR && !prefix[0]);
        read_exact(expected, suffix, blocker, sizeof blocker); CHECK(file_count(directory) == 1);
        CHECK(spurs_job_snapshot_capture(&s, prefix, sizeof prefix) == SPURS_JOB_CAPTURE_WRITTEN);
        capture_prefix(expected, sizeof expected, directory, 1); CHECK(!strcmp(prefix, expected));
        verify_capture(prefix); CHECK(file_count(directory) == 4);
    } else {
        unsigned count = !strcmp(mode, "capture-limit") ? SPURS_JOB_CAPTURE_MAX_COUNT : 2;
        for (unsigned i = 0; i < count; ++i) {
            CHECK(spurs_job_snapshot_capture(&s, prefix, sizeof prefix) == SPURS_JOB_CAPTURE_WRITTEN);
            capture_prefix(expected, sizeof expected, directory, i); CHECK(!strcmp(prefix, expected));
            verify_capture(prefix);
        }
        CHECK(file_count(directory) == count * 3);
        if (count == SPURS_JOB_CAPTURE_MAX_COUNT) {
            CHECK(spurs_job_snapshot_capture(&s, prefix, sizeof prefix) == SPURS_JOB_CAPTURE_LIMIT && !prefix[0]);
            CHECK(file_count(directory) == count * 3);
        }
        capture_prefix(expected, sizeof expected, directory, 0); verify_capture(expected);
    }
done:
    spurs_job_snapshot_release(&s); printf("PASS %s\n", mode);
}

#ifdef SPURS_SNAPSHOT_PRODUCTION
/* Actual public production entry points; no fake workload or strict stop. */
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
extern int32_t cellSpursCreateJobChain(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t);
extern int32_t cellSpursRunJobChain(uint64_t);
extern int32_t cellSpursJoinJobChain(uint64_t);
extern unsigned spu_workload_count(void);
struct spu_context;
extern void spu_workload_register(uint64_t, void (*)(struct spu_context*), const char*);
extern uint64_t spu_workload_fingerprint(const void*, size_t);
static void forbidden_entry(struct spu_context* context)
{
    (void)context; fprintf(stderr, "FAIL unqualified registered job entry was executed\n"); _Exit(91);
}
static void production(const char* mode, const char* directory)
{
    CHECK(file_count(directory) == 0 && spu_workload_count() == 0);
    uint64_t fingerprint = reference_fingerprint(image_bytes, IMAGE_SIZE);
    CHECK(spu_workload_fingerprint(image_bytes, IMAGE_SIZE) == fingerprint);
    if (!strcmp(mode, "production-hit")) {
        spu_workload_register(fingerprint, forbidden_entry, "synthetic forbidden snapshot entry");
        CHECK(spu_workload_count() == 1);
    }
    vm_base = memory; ppu_vm_size = 0; /* Production full-VM sentinel. */
    commit(0x40000, page_size);
    uint64_t job = JOB_EA;
    if (!strcmp(mode, "production-invalid")) job = 0x60000;
    if (!strcmp(mode, "production-high-job")) job |= UINT64_C(1) << 32;
    be64(memory + 0x40000, job);
    be64(memory + 0x40008, 0x7F); /* Existing decoder's END. */
    env_set("PS3_STRICT_BOOT", "1"); env_set("PS3_NO_JOBCHAIN", NULL); env_set("SPURS_JC_DELAY", NULL);
    char missing[4096];
    if (!strcmp(mode, "production-miss-disabled")) env_set("FOLKLORE_SPU_CAPTURE_DIR", NULL);
    else if (!strcmp(mode, "production-capture-failure")) {
        CHECK(snprintf(missing, sizeof missing, "%s/missing-subdirectory", directory) < (int)sizeof missing);
        env_set("FOLKLORE_SPU_CAPTURE_DIR", missing);
    } else env_set("FOLKLORE_SPU_CAPTURE_DIR", directory);
    CHECK(cellSpursCreateJobChain(0, 0x50000, 0x40000, DESC_SIZE, 1, 0, 1, 1) == 0);
    CHECK(cellSpursRunJobChain(0x50000) == 0);
    (void)cellSpursJoinJobChain(0x50000);
    fprintf(stderr, "FAIL production strict stop returned instead of exit 86\n"); exit(1);
}
#endif
static int run_mode(int argc, const char* const* argv)
{
    CHECK(argc >= 2); setup();
    if (!strcmp(argv[1], "unit")) unit();
    else if (!strcmp(argv[1], "verify-capture")) {
        CHECK(argc == 3 && file_count(argv[2]) == 3);
        char prefix[4096]; capture_prefix(prefix, sizeof prefix, argv[2], 0); verify_capture(prefix);
        puts("PASS production capture bytes and metadata");
    }
#ifdef SPURS_SNAPSHOT_PRODUCTION
    else if (!strcmp(argv[1], "production-miss") || !strcmp(argv[1], "production-invalid") ||
             !strcmp(argv[1], "production-hit") || !strcmp(argv[1], "production-high-job") ||
             !strcmp(argv[1], "production-miss-disabled") || !strcmp(argv[1], "production-capture-failure")) {
        CHECK(argc == 3); production(argv[1], argv[2]);
    }
#endif
    else {
        CHECK(argc == 3);
        CHECK(!strcmp(argv[1], "disabled") || !strcmp(argv[1], "capture") ||
            !strcmp(argv[1], "capture-errors") || !strcmp(argv[1], "capture-collision") ||
            !strcmp(argv[1], "capture-metadata-collision") || !strcmp(argv[1], "capture-limit"));
        captures(argv[1], argv[2]);
    }
    teardown(); return 0;
}
#ifdef SPURS_SNAPSHOT_NATIVE
/* Call once in a fresh diagnostic process, before game or registry startup. */
int folklore_check_spurs_snapshot(const char* mode, const char* directory)
{
    const char* args[] = {"spurs-snapshot", mode, directory};
    CHECK(mode != NULL);
    return run_mode(directory ? 3 : 2, args);
}
#else
int main(int argc, char** argv)
{
    return run_mode(argc, (const char* const*)argv);
}
#endif
