#ifndef PPU_TLS_POOL_H
#define PPU_TLS_POOL_H

#include <cstdint>
#include <mutex>

/* Fixed guest-memory region. Slot zero belongs to main and is never leased.
 * Owners are host context identities, independent of guest changes to r13. */
class ppu_tls_pool {
    std::mutex lock_;
    uintptr_t owners_[64] = {};
    uint32_t start_ = 0, stride_ = 0, count_ = 0;
public:
    uint32_t acquire(uintptr_t owner, uint32_t start, uint32_t stride, uint32_t limit) {
        if (!owner || stride < 0x7000u || start >= limit) return 0;
        uint32_t blocks = (limit - start) / stride;
        if (blocks <= 1 || blocks - 1 > 64) return 0;
        std::lock_guard<std::mutex> guard(lock_);
        bool busy = false;
        for (auto value : owners_) {
            if (value == owner) return 0;
            busy |= value != 0;
        }
        if (busy && (start_ != start || stride_ != stride || count_ != blocks - 1)) return 0;
        start_ = start; stride_ = stride; count_ = blocks - 1;
        for (uint32_t i = 0; i < count_; ++i) {
            if (!owners_[i]) {
                owners_[i] = owner;
                return start_ + (i + 1) * stride_;
            }
        }
        return 0;
    }
    uint32_t release(uintptr_t owner) {
        if (!owner) return 0;
        std::lock_guard<std::mutex> guard(lock_);
        for (uint32_t i = 0; i < count_; ++i) {
            if (owners_[i] == owner) {
                owners_[i] = 0;
                return start_ + (i + 1) * stride_;
            }
        }
        return 0;
    }
};
#endif
