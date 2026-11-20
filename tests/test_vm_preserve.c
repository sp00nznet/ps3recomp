/* Preservation sessions of the guest write watch (runtime/memory/vm_watch.h)
 * on synthetic guest memory: a write to a page armed with vm_watch_preserve
 * after the session is sealed is copied first, so reads return the bytes of
 * the sealed moment (from the copy, or live while still protected); pages
 * never armed, armed after the seal, armed without preservation, or lost to a
 * full pool are reported; kernel writes and runtime protection changes
 * preserve too; two sessions each see their own moment; concurrent writers
 * never leak into a session's reads. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../runtime/memory/vm_watch.h"
#include "../runtime/memory/vm_watch_hooks.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <pthread.h>
  #include <sys/mman.h>
  #include <unistd.h>
#endif

uint8_t* vm_base;
uint32_t ppu_vm_size;

static uint64_t page;
#define REGION (UINT64_C(64) << 20) /* guest [0, 64 MB) */
#define BASE UINT32_C(0x100000)

static void set_env(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
static vm_preserve_stats_t pstats(void)
{
    vm_preserve_stats_t s;
    vm_preserve_stats(&s);
    return s;
}
static uint32_t at(unsigned n) { return BASE + (uint32_t)(n * page); }
static void fill(uint32_t ea, size_t len, uint8_t v) { memset(vm_base + ea, v, len); }
static int all(const uint8_t* p, size_t n, uint8_t v)
{
    for (size_t i = 0; i < n; ++i)
        if (p[i] != v) return 0;
    return 1;
}

#ifndef _WIN32
struct stamper {
    volatile uint8_t* at;
    size_t bytes;
    uint8_t value;
    volatile int* stop;
};
static void* stamp(void* arg)
{
    struct stamper* s = (struct stamper*)arg;
    while (!*s->stop)
        for (size_t i = 0; i < s->bytes; i += 64) s->at[i] = s->value;
    return NULL;
}
#endif

int main(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    page = si.dwPageSize;
    vm_base = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)REGION, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(vm_base);
#else
    page = (uint64_t)sysconf(_SC_PAGESIZE);
    vm_base = (uint8_t*)mmap(NULL, (size_t)REGION, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(vm_base != MAP_FAILED);
#endif
    ppu_vm_size = (uint32_t)REGION;
    set_env("PS3RECOMP_VM_WRITE_WATCH", "1");
    set_env("PS3RECOMP_VM_PRESERVE_POOL_MB", "1"); /* 1 MB: the pool can fill */
    const unsigned slots = (unsigned)((UINT64_C(1) << 20) / page);
    assert(vm_watch_enable());
    uint8_t* buf = (uint8_t*)malloc((size_t)(4 * page));
    uint32_t pages = 0;

    /* 0. Unsealed or unknown sessions read nothing. */
    assert(vm_preserve_read(0, at(0), buf, 16, &pages) == -1);

    /* 1. A two-page span written after the seal reads as of the seal, from
     * the copies. */
    fill(at(0), (size_t)(2 * page), 1);
    int s = vm_preserve_begin();
    assert(s);
    assert(vm_preserve_read(s, at(0), buf, 16, &pages) == -1); /* not sealed */
    assert(vm_watch_preserve(at(0), (uint32_t)(2 * page)));
    const uint64_t e = vm_preserve_seal(s);
    assert(e);
    vm_base[at(0) + 5] = 2;
    vm_base[at(1) + 7] = 3;
    assert(vm_base[at(0) + 5] == 2 && vm_base[at(1) + 7] == 3); /* writes proceed */
    assert(pstats().preserved_pages == 2);
    /* Straddling the page boundary. */
    assert(vm_preserve_read(s, at(1) - 8, buf, 16, &pages) == 0 && pages == 2);
    assert(all(buf, 16, 1));
    assert(vm_preserve_read(s, at(0), buf, (uint32_t)(2 * page), &pages) == 0 && pages == 2);
    assert(all(buf, (size_t)(2 * page), 1));

    /* 2. A span over a written (copied) page and a still-protected one:
     * the second is read live. */
    fill(at(2), (size_t)(2 * page), 4);
    assert(vm_watch_preserve(at(2), (uint32_t)(2 * page)));
    vm_preserve_end(s);
    s = vm_preserve_begin();
    const uint64_t e2 = vm_preserve_seal(s);
    assert(e2 > e);
    vm_base[at(2)] = 5;
    const vm_preserve_stats_t before_live = pstats();
    assert(vm_preserve_read(s, at(2), buf, (uint32_t)(2 * page), &pages) == 0 && pages == 1);
    assert(all(buf, (size_t)(2 * page), 4));
    assert(pstats().read_live == before_live.read_live + 1);

    /* 3. Never armed, or armed after the seal: reported, live bytes. */
    fill(at(4), 32, 6);
    assert(vm_preserve_read(s, at(4), buf, 32, &pages) == -1 && all(buf, 32, 6));
    assert(vm_watch_preserve(at(4), 32));
    assert(vm_preserve_read(s, at(4), buf, 32, &pages) == -1);

    /* 4. Armed without preservation and written: no copy. */
    fill(at(5), 32, 7);
    vm_preserve_end(s);
    assert(vm_watch(at(5), 32));
    s = vm_preserve_begin();
    vm_preserve_seal(s);
    const uint64_t copies = pstats().preserved_pages;
    vm_base[at(5)] = 8;
    assert(pstats().preserved_pages == copies);
    assert(vm_preserve_read(s, at(5), buf, 32, &pages) == -1 && buf[0] == 8);
    /* Armed with preservation but no session sealed: no copy either. */
    vm_preserve_end(s);
    fill(at(6), 32, 9);
    assert(vm_watch_preserve(at(6), 32));
    vm_base[at(6)] = 10;
    assert(pstats().preserved_pages == copies);

    /* 5. An announced kernel write and a runtime protection change preserve
     * first. */
    fill(at(7), 32, 11);
    fill(at(8), 32, 12);
    assert(vm_watch_preserve(at(7), 32) && vm_watch_preserve(at(8), 32));
    s = vm_preserve_begin();
    vm_preserve_seal(s);
    vm_note_write(at(7), 16);
    memset(vm_base + at(7), 13, 16); /* the kernel's write, now allowed */
    vm_watch_forget(at(8), 16, 1);
    vm_base[at(8)] = 14;
    assert(vm_preserve_read(s, at(7), buf, 32, &pages) == 0 && all(buf, 32, 11));
    assert(vm_preserve_read(s, at(8), buf, 32, &pages) == 0 && all(buf, 32, 12));
    vm_preserve_end(s);

    /* 6. Two sessions, each with its own moment: the page re-armed between
     * them and written after both. */
    fill(at(9), 32, 20);
    assert(vm_watch_preserve(at(9), 32));
    int a = vm_preserve_begin();
    vm_preserve_seal(a);
    vm_base[at(9)] = 21; /* copy for a: 20 */
    assert(vm_watch_preserve(at(9), 32));
    int b = vm_preserve_begin();
    assert(b && b != a);
    vm_preserve_seal(b);
    vm_base[at(9) + 1] = 22; /* copy for b: 21 then 20 */
    assert(vm_preserve_read(a, at(9), buf, 2, &pages) == 0 && buf[0] == 20 && buf[1] == 20);
    assert(vm_preserve_read(b, at(9), buf, 2, &pages) == 0 && buf[0] == 21 && buf[1] == 20);
    /* At most four open sessions. */
    int c = vm_preserve_begin(), d = vm_preserve_begin();
    assert(c && d && !vm_preserve_begin());
    vm_preserve_end(a);
    vm_preserve_end(b);
    vm_preserve_end(c);
    vm_preserve_end(d);

    /* 7. A full pool loses pages, which then read as unpreserved; slots of
     * ended sessions are reused. */
    const unsigned many = slots + 8;
    fill(at(16), (size_t)(many * page), 30);
    assert(vm_watch_preserve(at(16), (uint32_t)(many * page)));
    s = vm_preserve_begin();
    vm_preserve_seal(s);
    const uint64_t lost = pstats().lost_pages;
    for (unsigned i = 0; i < many; ++i) vm_base[at(16 + i)] = 31;
    assert(pstats().lost_pages >= lost + 8);
    unsigned unpreserved = 0;
    for (unsigned i = 0; i < many; ++i) {
        const int r = vm_preserve_read(s, at(16 + i), buf, 4, &pages);
        if (r == 0) assert(all(buf, 4, 30));
        else ++unpreserved;
    }
    assert(unpreserved >= 8);
    vm_preserve_end(s);
    /* All slots free again: a new session copies. */
    fill(at(10), 32, 40);
    assert(vm_watch_preserve(at(10), 32));
    s = vm_preserve_begin();
    vm_preserve_seal(s);
    const uint64_t lost_after = pstats().lost_pages;
    vm_base[at(10)] = 41;
    assert(pstats().lost_pages == lost_after);
    assert(vm_preserve_read(s, at(10), buf, 32, &pages) == 0 && all(buf, 32, 40));
    vm_preserve_end(s);

#ifndef _WIN32
    /* 8. Writers stamping the pages while a reader reads them: every read is
     * the sealed moment's bytes. */
    const unsigned span_pages = 4;
    for (unsigned round = 0; round < 20; ++round) {
        const uint8_t then = (uint8_t)(50 + round);
        fill(at(12), (size_t)(span_pages * page), then);
        assert(vm_watch_preserve(at(12), (uint32_t)(span_pages * page)));
        s = vm_preserve_begin();
        vm_preserve_seal(s);
        volatile int stop = 0;
        pthread_t t[4];
        struct stamper w[4];
        for (int i = 0; i < 4; ++i) {
            w[i].at = vm_base + at(12 + (unsigned)i % span_pages);
            w[i].bytes = (size_t)page;
            w[i].value = (uint8_t)(200 + i);
            w[i].stop = &stop;
            assert(pthread_create(&t[i], NULL, stamp, &w[i]) == 0);
        }
        for (int reads = 0; reads < 200; ++reads) {
            assert(vm_preserve_read(s, at(12), buf, (uint32_t)(span_pages * page), &pages) == 0);
            assert(all(buf, (size_t)(span_pages * page), then));
        }
        stop = 1;
        for (int i = 0; i < 4; ++i) pthread_join(t[i], NULL);
        vm_preserve_end(s);
    }
#endif

    const vm_preserve_stats_t p = pstats();
    printf("vm_preserve: sessions=%llu preserved=%llu lost=%llu read_preserved=%llu read_live=%llu "
           "read_unpreserved=%llu\n",
           (unsigned long long)p.sessions, (unsigned long long)p.preserved_pages,
           (unsigned long long)p.lost_pages, (unsigned long long)p.read_preserved,
           (unsigned long long)p.read_live, (unsigned long long)p.read_unpreserved);
    free(buf);
    return 0;
}
