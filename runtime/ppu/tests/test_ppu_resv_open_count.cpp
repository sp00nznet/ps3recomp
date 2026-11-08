// PPU reservation breaks with PS3RECOMP_PPU_RESV_OPEN_COUNT off and on.
//
// Built by tools/test_ppu_resv_open_count.py against the real ppu_loader.cpp
// and lifted lwarx/stwcx. functions from ppu_lifter.py --resv-open-count:
//   func_00001000: lwarx r3,0,r4    func_00002000: stwcx. r5,0,r4
//
// The same checks must pass with the counts off and on:
//   - a successful store-conditional breaks another context's reservation on
//     the word, a line break (ppu_resv_break_line) breaks one on the line, and
//     with PPU_RESV_STORE=1 a plain store breaks one on the word;
//   - a reservation left open (no store-conditional) keeps being broken;
//   - two contexts that interleave load-reserves on one host thread (fibers)
//     keep their reservations breakable;
//   - stress: an updater thread commits same-value store-conditionals to one
//     word while readers load-reserve it (moving between two lines on the way)
//     and store-conditional the same value back. Value-CAS cannot tell those
//     apart, so a reader commit that spans a completed updater commit is a
//     missed break. The driver counts those and must see none.
// With the counts on, a white-box check also shows the skip is taken: a
// context whose reservation was planted without a load-reserve is spared.
// With PPU_RESV_OFF=1 (plain value CAS) the stress must report violations,
// which shows it can see the failure it guards against.
#include "ppu_recomp.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include "win32_compat.h"

extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x20000000;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern uint32_t ppu_vm_size;
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
void cellGcm_ref_on_poll(void) { std::abort(); }
DWORD GetFileAttributesA(const char*) { return INVALID_FILE_ATTRIBUTES; }
void ppu_resv_register(ppu_context*);
void ppu_resv_break_line(uint32_t ea);
}

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

static unsigned char arena[0x40000] __attribute__((aligned(128)));
static bool eq(const ppu_context& c) { return (c.cr & 0x20000000u) != 0; }
static uint32_t ll(ppu_context& c, uint32_t ea) { g_active_ctx = &c; c.gpr[4] = ea; func_00001000(&c); return (uint32_t)c.gpr[3]; }
static bool sc(ppu_context& c, uint32_t ea, uint32_t v) { g_active_ctx = &c; c.gpr[4] = ea; c.gpr[5] = v; func_00002000(&c); return eq(c); }

/* Registered contexts must outlive the process: the registry keeps them. */
static ppu_context g_main, g_other, g_fiber_a, g_fiber_b, g_planted, g_upd;
static ppu_context g_readers[3];

/* Run fn on its own host thread and wait for it. */
template <class F> static void on_thread(F fn) { std::thread t(fn); t.join(); }

int main(int argc, char** argv)
{
    const char* e = std::getenv("PS3RECOMP_PPU_RESV_OPEN_COUNT");
    const bool counting = e && !std::strcmp(e, "1");
    const bool resv_off = std::getenv("PPU_RESV_OFF") != nullptr;
    const bool store_breaks = std::getenv("PPU_RESV_STORE") != nullptr;
    const long rounds = argc > 1 ? std::atol(argv[1]) : 200000;
    vm_base = arena; ppu_vm_size = sizeof arena;
    for (ppu_context* c : {&g_main, &g_other, &g_fiber_a, &g_fiber_b, &g_planted, &g_upd,
                           &g_readers[0], &g_readers[1], &g_readers[2]})
        ppu_resv_register(c);

    const uint32_t A = 0x10008, A_LINE_OTHER = 0x10040, B = 0x10108;
    if (!resv_off) {
        /* A successful store-conditional breaks another context's reservation. */
        on_thread([&] { ll(g_other, A); });
        ll(g_main, A); CHECK(sc(g_main, A, 5));
        CHECK(!sc(g_other, A, 6) && "store-conditional must break another reservation on the word");
        CHECK(vm_read32(A) == 5);

        /* A line break reaches a reservation on another word of the line. */
        on_thread([&] { ll(g_other, A); });
        g_active_ctx = &g_main;
        ppu_resv_break_line(A_LINE_OTHER);
        CHECK(!sc(g_other, A, 7) && "a line break must reach a reservation on the line");

        /* An abandoned load-reserve stays breakable, and keeps being broken. */
        on_thread([&] { ll(g_other, A); });
        for (int i = 0; i < 3; i++) {
            ll(g_main, A); CHECK(sc(g_main, A, 10 + i));
            CHECK(g_other.reserve_addr > 0xFFFFFFFFull);
            g_other.reserve_addr = A;   /* re-open it as the abandoned one was */
        }
        g_other.reserve_addr = 0x100000000ull;

        /* Fibers: two contexts load-reserve on one host thread; context B's
         * store-conditional must not release A's reservation from the count. */
        on_thread([&] {
            ll(g_fiber_a, A);
            ll(g_fiber_b, B);
            CHECK(sc(g_fiber_b, B, 1));
        });
        ll(g_main, A); CHECK(sc(g_main, A, 20));
        CHECK(!sc(g_fiber_a, A, 21) && "an interleaved fiber's reservation must stay breakable");

        /* PPU_RESV_STORE=1: a plain store breaks a reservation on the word. */
        if (store_breaks) {
            on_thread([&] { ll(g_other, A); });
            g_active_ctx = &g_main;
            vm_write32(A, 30);
            CHECK(!sc(g_other, A, 31) && "a plain store must break a reservation on the word");
        }

        /* White box: with the counts on, a break whose line no counted
         * reservation is on skips the scan, so a reservation planted without
         * a load-reserve survives it. Off, the scan clears it. */
        g_planted.reserve_addr = B;
        ll(g_main, B); CHECK(sc(g_main, B, 40));
        CHECK(counting ? g_planted.reserve_addr == B : g_planted.reserve_addr > 0xFFFFFFFFull);
        g_planted.reserve_addr = 0x100000000ull;
        std::printf("deterministic checks pass (counts %s%s)\n", counting ? "on" : "off",
                    store_breaks ? ", plain-store breaks on" : "");
    }

    /* Stress. W never changes value, so every store-conditional is a value-CAS
     * that succeeds unless its reservation was broken. */
    const uint32_t W = 0x20000, W2 = 0x20200;
    vm_write32(W, 0x1234); vm_write32(W2, 0x1234);
    std::atomic<long> started{0}, done{0};
    std::atomic<bool> stop{false};
    std::atomic<unsigned long> violations{0}, reader_commits{0};
    std::thread upd([&] {
        for (long k = 1; k <= rounds; k++) {
            started.store(k);
            while (!(ll(g_upd, W), sc(g_upd, W, 0x1234))) {}
            done.store(k);
        }
        stop = true;
    });
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; r++)
        readers.emplace_back([&, r] {
            ppu_context& c = g_readers[r];
            unsigned n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                if (n++ & 1) ll(c, W2);              /* move the count between lines */
                ll(c, W);
                /* The lifted lwarx publishes reserve_addr with a plain store:
                 * order it before reading `done` (the runtime's breaks are
                 * best effort against that store, see ppu_resv_break_store). */
                std::atomic_thread_fence(std::memory_order_seq_cst);
                const long d0 = done.load();
                for (volatile unsigned i = 0; i < (n & 255); i = i + 1) {}
                const long d1 = done.load();
                if (sc(c, W, 0x1234)) {
                    reader_commits.fetch_add(1, std::memory_order_relaxed);
                    /* Updater commit d0+1 may predate our load-reserve; commit
                     * d0+2 started after done==d0+1, which we had not seen
                     * after our load-reserve, and finished before d1. */
                    if (d1 >= d0 + 2) violations.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    upd.join();
    for (auto& t : readers) t.join();
    std::printf("stress counts=%s resv_off=%d: %ld updater commits, %lu reader commits, %lu violations\n",
                counting ? "on" : "off", resv_off ? 1 : 0, rounds, reader_commits.load(), violations.load());
    (void)started;
    if (resv_off) return violations.load() ? 0 : 3;
    CHECK(reader_commits.load() > 0);
    CHECK(violations.load() == 0);
    std::puts("PASS");
    return 0;
}
