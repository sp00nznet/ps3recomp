/*
 * ps3recomp - Big-endian conversion utilities
 *
 * Platform-aware byte swap intrinsics and templated conversion functions
 * for translating between PS3 (big-endian) and host (little-endian) byte order.
 */

#ifndef PS3EMU_ENDIAN_H
#define PS3EMU_ENDIAN_H

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Platform intrinsics
 * -----------------------------------------------------------------------*/

#if defined(_MSC_VER)
  #include <stdlib.h>   /* _byteswap_* */

  static inline uint16_t ps3_bswap16(uint16_t v) { return _byteswap_ushort(v); }
  static inline uint32_t ps3_bswap32(uint32_t v) { return _byteswap_ulong(v);  }
  static inline uint64_t ps3_bswap64(uint64_t v) { return _byteswap_uint64(v); }

#elif defined(__GNUC__) || defined(__clang__)

  static inline uint16_t ps3_bswap16(uint16_t v) { return __builtin_bswap16(v); }
  static inline uint32_t ps3_bswap32(uint32_t v) { return __builtin_bswap32(v); }
  static inline uint64_t ps3_bswap64(uint64_t v) { return __builtin_bswap64(v); }

#else
  /* Portable fallbacks */
  static inline uint16_t ps3_bswap16(uint16_t v)
  {
      return (uint16_t)((v >> 8) | (v << 8));
  }

  static inline uint32_t ps3_bswap32(uint32_t v)
  {
      v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
      return (v << 16) | (v >> 16);
  }

  static inline uint64_t ps3_bswap64(uint64_t v)
  {
      v = ((v & 0x00FF00FF00FF00FFull) <<  8) | ((v & 0xFF00FF00FF00FF00ull) >>  8);
      v = ((v & 0x0000FFFF0000FFFFull) << 16) | ((v & 0xFFFF0000FFFF0000ull) >> 16);
      return (v << 32) | (v >> 32);
  }
#endif

/* 128-bit byte swap (two-word swap + exchange) */
typedef struct ps3_u128 {
    uint64_t hi;
    uint64_t lo;
} ps3_u128;

static inline ps3_u128 ps3_bswap128(ps3_u128 v)
{
    ps3_u128 r;
    r.hi = ps3_bswap64(v.lo);
    r.lo = ps3_bswap64(v.hi);
    return r;
}

/* ---------------------------------------------------------------------------
 * C++ template helpers
 * -----------------------------------------------------------------------*/
#ifdef __cplusplus

namespace ps3 {
namespace endian {

template <typename T> T byte_swap(T v);

template <> inline uint8_t  byte_swap<uint8_t>(uint8_t v)   { return v; }
template <> inline int8_t   byte_swap<int8_t>(int8_t v)     { return v; }
template <> inline uint16_t byte_swap<uint16_t>(uint16_t v) { return ps3_bswap16(v); }
template <> inline int16_t  byte_swap<int16_t>(int16_t v)   { return (int16_t)ps3_bswap16((uint16_t)v); }
template <> inline uint32_t byte_swap<uint32_t>(uint32_t v) { return ps3_bswap32(v); }
template <> inline int32_t  byte_swap<int32_t>(int32_t v)   { return (int32_t)ps3_bswap32((uint32_t)v); }
template <> inline uint64_t byte_swap<uint64_t>(uint64_t v) { return ps3_bswap64(v); }
template <> inline int64_t  byte_swap<int64_t>(int64_t v)   { return (int64_t)ps3_bswap64((uint64_t)v); }
template <> inline float byte_swap<float>(float v)
{
    uint32_t tmp;
    memcpy(&tmp, &v, sizeof(tmp));
    tmp = ps3_bswap32(tmp);
    float result;
    memcpy(&result, &tmp, sizeof(result));
    return result;
}
template <> inline double byte_swap<double>(double v)
{
    uint64_t tmp;
    memcpy(&tmp, &v, sizeof(tmp));
    tmp = ps3_bswap64(tmp);
    double result;
    memcpy(&result, &tmp, sizeof(result));
    return result;
}

/*
 * be_to_host / host_to_be  --  identical operations (swap is its own inverse),
 * but the two names make intent clear at call sites.
 */
template <typename T>
inline T be_to_host(T v) { return byte_swap<T>(v); }

template <typename T>
inline T host_to_be(T v) { return byte_swap<T>(v); }

} /* namespace endian */
} /* namespace ps3 */

#endif /* __cplusplus */
#endif /* PS3EMU_ENDIAN_H */
