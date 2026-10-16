#include "../runtime/ppu/ppu_tls_pool.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <set>
#include <thread>
#include <vector>

int main() {
    ppu_tls_pool pool;
    constexpr uint32_t start = 0x10f00000, stride = 0x8000, limit = 0x11000000;
    std::set<uint32_t> live;
    for (uintptr_t owner = 1; owner <= 31; ++owner) {
        uint32_t address = pool.acquire(owner, start, stride, limit);
        assert(address > start && address + stride <= limit && live.insert(address).second);
    }
    assert(!pool.acquire(32, start, stride, limit));
    assert(!pool.acquire(1, start, stride, limit));
    assert(!pool.acquire(32, start, stride * 2, limit));
    assert(!pool.release(100));
    uint32_t reusable = pool.release(17);
    assert(live.erase(reusable) == 1 && !pool.release(17));
    for (uintptr_t owner = 100; owner < 10100; ++owner) {
        uint32_t address = pool.acquire(owner, start, stride, limit);
        assert(address == reusable && !live.count(address));
        assert(pool.release(owner) == address);
    }
    for (uintptr_t owner = 1; owner <= 31; ++owner) {
        if (owner != 17) assert(live.erase(pool.release(owner)) == 1);
    }
    assert(live.empty());
    std::mutex seen_lock;
    std::atomic<unsigned> passes{0};
    std::vector<std::thread> threads;
    for (uintptr_t owner = 1; owner <= 8; ++owner) threads.emplace_back([&, owner] {
        for (unsigned iteration = 0; iteration < 2000; ++iteration) {
            uint32_t address = pool.acquire(owner, start, stride, limit);
            assert(address > start && address + stride <= limit);
            {
                std::lock_guard<std::mutex> guard(seen_lock);
                assert(live.insert(address).second);
            }
            std::this_thread::yield();
            {
                std::lock_guard<std::mutex> guard(seen_lock);
                assert(live.erase(address) == 1);
                assert(pool.release(owner) == address);
            }
            ++passes;
        }
    });
    for (auto& thread : threads) thread.join();
    assert(passes == 16000 && live.empty());
    assert(!pool.acquire(1, start, 0, limit));
    assert(!pool.acquire(1, start, stride, start + stride));
    assert(!pool.acquire(0, start, stride, limit));
    assert(pool.acquire(1, start, stride * 2, limit) == start + stride * 2);
    assert(pool.release(1) == start + stride * 2);
    std::puts("PPU TLS pool: exhaustion, 10000 churn cycles, 16000 concurrent leases and bounds passed");
}
