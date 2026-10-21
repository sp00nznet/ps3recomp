/* Force-included when building the pinned FidelityFX SDK 1.1.4 sources with
 * clang for x86_64-w64-mingw32. Supplies MSVC-provided names the sources use
 * without including; the official files stay byte-identical to the tag. */
#pragma once
#if defined(__cplusplus)
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <new>
#ifndef _countof
template <typename T, std::size_t N> constexpr std::size_t ffx_compat_countof(T (&)[N]) { return N; }
#define _countof(a) ffx_compat_countof(a)
#endif
#endif
