/*
 * ps3recomp - Fundamental PS3 types
 *
 * Fixed-width integers, 128-bit vector types, big-endian value wrapper (be_t<T>),
 * and PS3 virtual-memory pointer type (vm::ptr<T>).
 */

#ifndef PS3EMU_PS3TYPES_H
#define PS3EMU_PS3TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * C-visible fixed-width typedefs
 * -----------------------------------------------------------------------*/
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

typedef uint8_t   b8; /* PS3 boolean (1 byte) */

/* 128-bit types for VMX / AltiVec registers.
 * Kept as a union so the same object can be accessed by lane width. */
typedef union u128_t {
    uint64_t _u64[2];
    uint32_t _u32[4];
    uint16_t _u16[8];
    uint8_t  _u8[16];
    int64_t  _s64[2];
    int32_t  _s32[4];
    int16_t  _s16[8];
    int8_t   _s8[16];
    float    _f32[4];
    double   _f64[2];
#ifdef __cplusplus
    /* Convenience: construct zero-initialized */
    u128_t() : _u64{} {}
    u128_t(uint64_t hi, uint64_t lo) : _u64{hi, lo} {}
#endif
} u128;

typedef u128 s128; /* signed interpretation alias */

/* PS3 virtual address - always 32-bit even on 64-bit host */
typedef uint32_t ps3_addr_t;

/* Common PS3 SDK typedefs */
typedef s32  CellError;
typedef u32  sys_pid_t;
typedef u64  sys_ppu_thread_t;
typedef u32  sys_spu_thread_t;
typedef u32  sys_spu_thread_group_t;
typedef u32  sys_mutex_t;
typedef u32  sys_cond_t;
typedef u32  sys_rwlock_t;
typedef u32  sys_event_queue_t;
typedef u32  sys_semaphore_t;
typedef u32  sys_lwmutex_t;
typedef u32  sys_event_flag_t;
typedef u32  sys_memory_container_t;

/* ---------------------------------------------------------------------------
 * C++ types (be_t, vm::ptr)
 * -----------------------------------------------------------------------*/
#ifdef __cplusplus

#include "endian.h"
#include <type_traits>
#include <cstring>

/*
 * be_t<T> -- Big-endian value wrapper.
 *
 * Stores the value in PS3 (big-endian) byte order.  Implicit conversion
 * to/from host order at read/write boundaries.  Suitable for mapping over
 * guest memory structures.
 */
template <typename T>
class be_t
{
    static_assert(std::is_arithmetic<T>::value || std::is_enum<T>::value,
                  "be_t<T> requires an arithmetic or enum type");

    using storage_t = std::conditional_t<std::is_enum<T>::value,
                                         std::underlying_type_t<T>, T>;
    storage_t m_data; /* stored in big-endian order */

public:
    be_t() = default;

    be_t(T val)
        : m_data(ps3::endian::host_to_be(static_cast<storage_t>(val)))
    {}

    /* Read: convert from BE to host */
    operator T() const
    {
        return static_cast<T>(ps3::endian::be_to_host(m_data));
    }

    T value() const { return static_cast<T>(*this); }

    /* Raw access (no conversion) -- for serialization / DMA */
    storage_t raw() const { return m_data; }
    void set_raw(storage_t v) { m_data = v; }

    /* Assignment */
    be_t& operator=(T val)
    {
        m_data = ps3::endian::host_to_be(static_cast<storage_t>(val));
        return *this;
    }

    /* Arithmetic convenience */
    be_t& operator+=(T rhs) { *this = value() + rhs; return *this; }
    be_t& operator-=(T rhs) { *this = value() - rhs; return *this; }
    be_t& operator*=(T rhs) { *this = value() * rhs; return *this; }
    be_t& operator/=(T rhs) { *this = value() / rhs; return *this; }
    be_t& operator&=(T rhs) { *this = value() & rhs; return *this; }
    be_t& operator|=(T rhs) { *this = value() | rhs; return *this; }
    be_t& operator^=(T rhs) { *this = value() ^ rhs; return *this; }

    be_t& operator++()    { *this = value() + 1; return *this; }
    be_t  operator++(int) { be_t old = *this; ++(*this); return old; }
    be_t& operator--()    { *this = value() - 1; return *this; }
    be_t  operator--(int) { be_t old = *this; --(*this); return old; }
};

/* Common big-endian aliases */
using be_u16 = be_t<u16>;
using be_u32 = be_t<u32>;
using be_u64 = be_t<u64>;
using be_s16 = be_t<s16>;
using be_s32 = be_t<s32>;
using be_s64 = be_t<s64>;
using be_f32 = be_t<float>;
using be_f64 = be_t<double>;

/*
 * vm -- PS3 virtual memory namespace.
 *
 * All PS3 addresses are 32-bit.  The host maps them through a base pointer
 * (vm::g_base) so that  host_ptr = g_base + ps3_addr.
 */
namespace vm {

/* Set by the memory manager at init time (runtime/memory/vm.h) */
extern uint8_t* g_base;

inline void* host_addr(ps3_addr_t addr)
{
    return g_base + addr;
}

/*
 * vm::ptr<T> -- typed pointer into PS3 address space.
 *
 * Holds a 32-bit guest address.  Dereferencing translates through g_base.
 */
template <typename T>
class ptr
{
    be_t<u32> m_addr;

public:
    ptr() = default;
    ptr(ps3_addr_t addr) : m_addr(addr) {}

    /* Get raw PS3 address */
    ps3_addr_t addr() const { return m_addr.value(); }

    /* Dereference into host memory */
    T* get_ptr() const
    {
        return reinterpret_cast<T*>(host_addr(addr()));
    }

    T& operator*()  const { return *get_ptr(); }
    T* operator->() const { return  get_ptr(); }

    T& operator[](s32 index) const { return get_ptr()[index]; }

    /* Pointer arithmetic (advances by sizeof(T) on the guest side) */
    ptr operator+(s32 offset) const
    {
        return ptr(addr() + static_cast<u32>(offset * sizeof(T)));
    }
    ptr operator-(s32 offset) const
    {
        return ptr(addr() - static_cast<u32>(offset * sizeof(T)));
    }
    ptr& operator+=(s32 offset) { m_addr = addr() + (u32)(offset * sizeof(T)); return *this; }
    ptr& operator-=(s32 offset) { m_addr = addr() - (u32)(offset * sizeof(T)); return *this; }
    ptr& operator++()    { return *this += 1; }
    ptr  operator++(int) { ptr old = *this; ++(*this); return old; }

    explicit operator bool() const { return addr() != 0; }
    bool operator!() const { return addr() == 0; }

    bool operator==(ptr rhs) const { return addr() == rhs.addr(); }
    bool operator!=(ptr rhs) const { return addr() != rhs.addr(); }
    bool operator< (ptr rhs) const { return addr() <  rhs.addr(); }
};

/* void specialization -- untyped guest pointer */
template <>
class ptr<void>
{
    be_t<u32> m_addr;
public:
    ptr() = default;
    ptr(ps3_addr_t addr) : m_addr(addr) {}
    ps3_addr_t addr() const { return m_addr.value(); }
    void* get_ptr() const { return host_addr(addr()); }
    explicit operator bool() const { return addr() != 0; }
};

/* Convenience alias */
template <typename T>
using bptr = ptr<be_t<T>>;

} /* namespace vm */

#endif /* __cplusplus */
#endif /* PS3EMU_PS3TYPES_H */
