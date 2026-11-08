# Explicit PPU nonlocal control flow

A recompiled guest `longjmp` may restore guest SP/LR and all saved registers correctly, yet an ordinary generated C++ `return` still returns to its immediate host caller. This opt-in feature changes only the final transfer: the original guest restore executes, then a typed C++ exception unwinds to the configured live owner and resumes at its exact guest continuation. The runtime does not emulate `jmp_buf`, normalize return values, or suppress guest error handling.

## Configuration

Pass `--nonlocal-control-flow path/to/config.json` to `tools/ppu_lifter.py`. For example, for an independently verified synthetic image:

```json
{
  "nonlocal_return_functions": ["0x3000"],
  "continuations": [{"function": "0x1000", "address": "0x101c"}]
}
```

Addresses may be integers or integer strings. The Python API is `PPULifter(nonlocal_return_functions={0x3000}, nonlocal_continuations={0x1000: {0x101c}})`. The default empty configuration does not change generated output. Configuration reaches both serial and multiprocessing lifters.

Each continuation must be a decoded instruction inside the emitted owner, immediately after an emitted direct linked call (`bl`). Indirect linked calls are rejected because the existing lifter does not establish their guest LR. Duplicate or ambiguous continuation addresses, missing functions, HLE replacement of configured functions, and malformed addresses are rejected. Owners and restores must be complete emitted functions: tail/fallthrough fragments and indirect tail exits are unsupported. Restore functions must contain `blr` and have no ordinary-return or conditional-return path. Configuration is an explicit allowlist, not automatic recognition or proof of a library's ABI. Callers should pin the executable hash and audited function ranges.

## Live frame and register contract

Each owner invocation registers a frame and records SP immediately before each configured call. Its checkpoint is identified by context pointer, guest thread ID, saved SP, and continuation LR. Restore code runs unchanged until its `blr`; the replacement transfer searches the current host thread's live frames for an exact match. A missing match aborts with a diagnostic. A match discards a pending tail trampoline and raises a signal tagged to that specific owner invocation. Intervening owners rethrow; only the target owner catches and resumes. Recursive instances of the same owner remain distinct. The resume jumps directly to the instruction after the linked call without rerunning the prologue or replacing restored registers.

Owner state, including existing generated callee-save snapshots, survives the resume loop. Original guest epilogues still run on normal protected-call return. Original guest error/status values remain visible. Unrelated C++ exceptions propagate, and their pending trampoline is not cleared by this support.

Generated headers use a common helper guard and ODR-shared inline TLS registry, so separately generated prefixes/modules share the same live-frame lookup when linked in one native executable. Independently loaded dynamic shared objects are not validated by this feature.

## Runtime integration and limitations

Reconfigure the SDK build after adding `runtime/syscalls/ppu_host_unwind.c`, because the existing runtime source glob is evaluated during CMake configuration. Opt-in generated code requires that runtime object, C++17-or-later inline variables, and C++ exceptions (the project currently uses C++20).

The existing thread-exit host `longjmp` skips generated C++ frames. Immediately before that nonreturning exit, the runtime calls a default-null TLS cleanup hook; opt-in generated code installs a hook that drops the abandoned frame registry. This permits the existing interrupt-thread reentry path to create fresh owners. It does not make C `longjmp` execute arbitrary C++ destructors or clean up other host resources. External host nonlocal-exit paths must invoke the same notification before abandoning guest frames; calling it during an ordinary returning path is invalid.

Indirect dispatch's depth and virtual-call-stack counters, and scratch guest-callback active-context pointers, now use C++ scope guards. These guards restore host bookkeeping on exceptions and never overwrite guest registers. Existing host C `longjmp` still bypasses those guards, as it bypassed their prior manual cleanup; fixing that inherited runtime behavior is separate from typed guest transfers.

The supported scope is synchronous control flow within one live guest context on one host thread. Cross-context and cross-thread restores, dead stack targets, split owner/restore functions, unconfigured restores, and guessed destinations are unsupported. An unconfigured restore keeps its existing generated behavior; only explicitly configured functions acquire this behavior.

## Verification

Run `python3 tools/test_ppu_nonlocal.py`. The baseline generated ordinary-return code fails with `guest longjmp returned to the throwing host call`. The regression compiles and executes generated synthetic guest save/restore instructions, including GPR/CR/FPR/vector restoration and guest zero-to-one return normalization. It tests nested and recursive owners, transfer to an outer owner, tail trampoline callees, distinct generated module prefixes/translation units, wrong identity/SP/LR, stale targets, host-exit cleanup and same-context reentry, unrelated exceptions, and host-thread isolation. It executes the actual runtime dispatch block and active-context guard extracted from source, including 4,100 repeated transfers to detect leaked recursion depth, rather than substituting a model dispatcher. It also rejects unsupported configurations and compares actual serial/parallel CLI output from decoded PPC bytes.

The test uses a Clang/GCC-compatible C/C++ compiler. On Windows, `tools/test_ppu_nonlocal_windows.py` runs the same synthetic functions through clang-cl and an SDK configured with `PS3RECOMP_PPU_NONLOCAL_CONTROL_FLOW=ON`; see [WINDOWS_CXX_UNWIND.md](WINDOWS_CXX_UNWIND.md). Synthetic coverage does not by itself validate a particular title's address allowlist or prove that subsequent guest errors are benign.
