# Windows C++ bridge exception contract

Generated PPU nonlocal returns use a typed C++ exception. It may cross an
`extern "C"` HLE or dispatcher call before the verified owner catches it.
Those bridges must participate in synchronous C++ stack unwinding, including
their active-context, dispatcher-depth and virtual-call cleanup.

CMake's MSVC-style default `/EHsc` additionally assumes that C-linkage calls
cannot throw. A port that lifts with `--nonlocal-control-flow` and builds with
MSVC or clang-cl must therefore configure the SDK with
`-DPS3RECOMP_PPU_NONLOCAL_CONTROL_FLOW=ON`. The runtime target then publishes
`/EHsc-` for C++ only, removing that assumption while keeping synchronous C++
exceptions. It applies to both SDK C++ and the port's generated/loader C++
sources linked to `ps3recomp_runtime`. C compilation, other compiler families
and builds with the option OFF (the default) are unchanged. Consumers which do
not link the CMake target must maintain this contract themselves; flags added
later must not reinstate `/EHc`.

`tools/test_ppu_nonlocal_windows.py` runs with clang-cl, CMake and Ninja
(`CXX`, `CMAKE`, `NINJA` may specify paths). It emits the existing synthetic PPC
owner/restore functions into separate optimized translation units, extracts
the actual loader dispatcher cleanup, and compiles through an independent
CMake OBJECT consumer linked to the runtime's usage requirements. It verifies
register restoration, recursion, cross-module TLS, unrelated-exception cleanup,
4100 repeated transfers, five invalid-identity/lifetime rejections, and that
the override reaches SDK/consumer C++ but no C source. Windows CRT host-longjmp
currently rejects during destructor unwinding after the explicit registry-reset
hook; that separate fail-closed behavior is tested, not changed here.

The regression requires no game files and executes no guest-derived code.
