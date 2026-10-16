# Android native fibers

Android Bionic exposes the `ucontext_t` signal-frame layout but does not provide
the `getcontext`, `setcontext`, `swapcontext`, and `makecontext` functions used
by the cooperative fiber implementation. Android builds require upstream
[libucontext](https://github.com/kaniini/libucontext), pinned here to commit
`49e671dd52ff6791295d8161ad3b6da7dc5f6f9d`.

Build the ARM64 static libraries with `ARCH=aarch64`, `FREESTANDING=no`,
`EXPORT_UNPREFIXED=no`, and `BUILD_POSIX=yes`, using the Android NDK compiler
and archiver. Retain upstream LICENSE with the installed dependency. Use the
public header and `arch/common/include/libucontext/bits.h`, which selects the
actual Bionic context layout. Upstream layout assertions are compiled against
that layout.

Pass these CMake variables to the runtime build:

* `PS3RECOMP_UCONTEXT_INCLUDE_DIR`: installed include directory.
* `PS3RECOMP_UCONTEXT_POSIX_LIBRARY`: absolute path to `libucontext_posix.a`.
* `PS3RECOMP_UCONTEXT_LIBRARY`: absolute path to `libucontext.a`.

The POSIX wrapper preserves signal masks. The base library supplies real native
register/stack switching and `makecontext`. Android's variadic makecontext
arguments are passed as words, matching the upstream AArch64 implementation.
Desktop builds retain their existing system context implementation.

NDK r30/API26 builds of both libraries and the actual cellFiber switching
fixture link successfully. The unchanged Linux fixture passes. ARM64 execution,
floating-point environment behavior and physical Android acceptance are still
pending; compilation is not evidence that those runtime requirements pass.
