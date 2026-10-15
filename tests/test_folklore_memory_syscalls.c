/* Standalone synthetic host test. Compile this TU only; no game input.
 * The commit dependency can return a deterministic failure, while success
 * uses the real VM commit and guest memory rather than a CELL_OK fake. */
#include "../runtime/syscalls/sys_memory.h"
#include <assert.h>
#include <stdio.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base;
static int32_t commit_error;
static int commit_calls;
static int32_t test_commit(uint32_t address, uint32_t size)
{
    commit_calls++;
    return commit_error ? commit_error : vm_commit(address, size);
}
#define vm_commit test_commit
#include "../runtime/syscalls/sys_memory.c"
#undef vm_commit

static ppu_context context;
static lv2_syscall_table table;
static uint32_t output = 0x10000;
static uint32_t read_be_output(uint32_t address)
{
    unsigned char* p = (unsigned char*)vm_to_host(address);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t get_output(void) { return read_be_output(output); }
static void check_size(uint32_t cid, uint32_t total, uint32_t available)
{
    context.gpr[3] = output; context.gpr[4] = cid;
    assert(table.handlers[343](&context) == CELL_OK);
    assert(read_be_output(output) == total);
    assert(read_be_output(output + 4) == available);
}
static uint32_t create_container(uint32_t size)
{
    context.gpr[3] = output; context.gpr[4] = size;
    assert(table.handlers[341](&context) == CELL_OK);
    return get_output();
}
static int64_t allocate(uint64_t size, uint32_t cid, uint64_t flags)
{
    memset(vm_to_host(output), 0xA5, 4);
    context.gpr[3] = size; context.gpr[4] = cid;
    context.gpr[5] = flags; context.gpr[6] = output;
    return table.handlers[350](&context);
}
static void expect_failure(uint64_t size, uint32_t cid, uint64_t flags, uint32_t error)
{
    uint32_t old_bump = g_sys_mem_bump_ptr;
    uint32_t old_used = cid && cid <= SYS_MEMORY_CONTAINER_MAX ? g_sys_mem_containers[cid - 1].used_size : 0;
    assert(allocate(size, cid, flags) == (int64_t)(int32_t)error);
    assert(get_output() == 0xA5A5A5A5);
    assert(g_sys_mem_bump_ptr == old_bump);
    if (cid && cid <= SYS_MEMORY_CONTAINER_MAX)
        assert(g_sys_mem_containers[cid - 1].used_size == old_used);
}
int main(void)
{
#ifdef _WIN32
    vm_base = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)VM_TOTAL_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    assert(vm_base);
    assert(VirtualAlloc(vm_base + output, 0x1000, MEM_COMMIT, PAGE_READWRITE));
#else
    vm_base = mmap(NULL, VM_TOTAL_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(vm_base != MAP_FAILED);
    assert(mprotect(vm_base + output, 0x1000, PROT_READ | PROT_WRITE) == 0);
#endif
    sys_memory_init(&table);
    assert(table.handlers[341] == sys_memory_container_create);
    assert(table.handlers[342] == sys_memory_container_destroy);
    assert(table.handlers[350] == sys_memory_allocate_from_container);
    uint32_t cid = create_container(0x4800000);
    assert(cid == 1 && g_sys_mem_bump_ptr == 0);
    check_size(cid, 0x4800000, 0x4800000);
    context.gpr[3] = 0; context.gpr[4] = cid;
    assert(table.handlers[343](&context) == (int64_t)(int32_t)CELL_EFAULT);
    context.gpr[3] = UINT32_MAX - 3;
    assert(table.handlers[343](&context) == (int64_t)(int32_t)CELL_EFAULT);
    context.gpr[3] = 0x60000000;
    assert(table.handlers[343](&context) == (int64_t)(int32_t)CELL_EFAULT);
#ifdef _WIN32
    /* First output word committed, second lies in uncommitted host page. */
    context.gpr[3] = output + 0xffc;
    assert(table.handlers[343](&context) == (int64_t)(int32_t)CELL_EFAULT);
#endif
    context.gpr[3] = output; context.gpr[4] = 17;
    assert(table.handlers[343](&context) == (int64_t)(int32_t)CELL_ESRCH);
    context.gpr[3] = 0x100000; context.gpr[4] = cid;
    context.gpr[5] = 0x400; context.gpr[6] = 0;
    assert(table.handlers[350](&context) == (int64_t)(int32_t)CELL_EFAULT);
    assert(g_sys_mem_bump_ptr == 0 && g_sys_mem_containers[0].used_size == 0);
    expect_failure(0, cid, 0x400, CELL_EALIGN);
    expect_failure(0x100001, cid, 0x400, CELL_EALIGN);
    expect_failure(0x100000, cid, 0x600, CELL_EINVAL);
    expect_failure(0x100000, 0, 0x400, CELL_ESRCH);
    expect_failure(0x100000, 17, 0x400, CELL_ESRCH);
    expect_failure(0x100000000ULL, cid, 0x400, CELL_ENOMEM);
    expect_failure(0x4900000, cid, 0x400, CELL_ENOMEM);
    commit_error = CELL_ENOMEM;
    expect_failure(0x4800000, cid, 0x400, CELL_ENOMEM);
    commit_error = CELL_EFAULT;
    expect_failure(0x4800000, cid, 0x400, CELL_EFAULT);
    commit_error = 0;
    assert(allocate(0x4800000, cid, 0x400) == CELL_OK);
    uint32_t address = get_output();
    assert(address == 0x40000000 && !(address % 0x100000));
    assert(g_sys_mem_containers[cid - 1].used_size == 0x4800000);
    check_size(cid, 0x4800000, 0);
    *(volatile unsigned char*)vm_to_host(address) = 0x7B;
    *(volatile unsigned char*)vm_to_host(address + 0x4800000 - 1) = 0xC1;
    assert(*(volatile unsigned char*)vm_to_host(address + 0x4800000 - 1) == 0xC1);
    assert(*(volatile unsigned char*)vm_to_host(address) == 0x7B);
    expect_failure(0x10000, cid, 0x200, CELL_ENOMEM);
    context.gpr[3] = cid;
    assert(table.handlers[342](&context) == (int64_t)(int32_t)CELL_EBUSY);
    context.gpr[3] = address;
    assert(sys_memory_free(&context) == CELL_OK);
    assert(g_sys_mem_containers[cid - 1].used_size == 0);
    check_size(cid, 0x4800000, 0x4800000);
    context.gpr[3] = cid;
    assert(table.handlers[342](&context) == CELL_OK);
    context.gpr[3] = output; context.gpr[4] = cid;
    assert(table.handlers[343](&context) == (int64_t)(int32_t)CELL_ESRCH);
    expect_failure(0x100000, cid, 0x400, CELL_ESRCH);
    cid = create_container(0x200000);
    assert(allocate(0x10000, cid, 0x200) == CELL_OK);
    assert(!(get_output() % 0x10000));
    assert(allocate(0x100000, cid, 0) == CELL_OK);
    assert(!(get_output() % 0x100000));
    printf("PASS: syscall350 ABI,341/342 aliases,72MiB real commit/write,budget/alignment/handle/errors,rollback,free lifecycle (%d commit calls)\n", commit_calls);
#ifdef _WIN32
    VirtualFree(vm_base, 0, MEM_RELEASE);
#else
    munmap(vm_base, VM_TOTAL_SIZE);
#endif
    return 0;
}
