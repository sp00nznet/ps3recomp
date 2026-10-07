# PPU conformance suite

Differential test of every PPU instruction: RPCS3's PPU interpreter (the oracle)
against ps3recomp (ppu_loader → ppu_lifter → runtime), on the same bytes.

```
python3 tests/conformance/ppu/run_ppu_conform.py --work /tmp/ppuconf            # everything
python3 tests/conformance/ppu/run_ppu_conform.py --work /tmp/ppuconf --only ADD,ADDO,BC
python3 tests/conformance/ppu/run_ppu_conform.py --work /tmp/ppuconf --skip-oracle   # reuse oracle.txt
```

Needs a local RPCS3 build (`--rpcs3 PATH`; default `../rpcs3/build/bin/...`).
The oracle runs headless with its own config (`oracle_config.yml`, written into
the work dir): PPU interpreter, saturation bit, accurate non-Java mode, accurate
vector NaNs, FPCC bits, accurate DFMA. Your normal RPCS3 config is not touched.

## How it works

`gen_ppu_conform.py` writes a bare lv2 ELF (no imports, raw syscalls). For each
case it loads a random full state — GPRs, FPRs, VRs, CR, XER, LR, CTR, FPSCR,
VSCR, VRSAVE — runs the instruction(s) under test, saves the state over the same
slot and prints it as one hex line via `sys_tty_write` (plus the 1 KB scratch
buffer for memory ops). `ppu_ops.py` encodes every op RPCS3's decoder knows
(`rpcs3/Emu/Cell/PPUOpcodes.h`) from its fields, so every operand field is
randomized; `ppu_isa.py` draws the values (specials: 0, ±1, INT_MIN/MAX, NaNs,
SNaNs, denormals, ±inf, rounding boundaries, ...). `compare.py` diffs the two
transcripts field by field and prints the failing fields per op.

r1 (stack) and r31 (the slot pointer) are never operands. Branch tests use the
shapes real code uses (conditional returns, tail calls through CTR, calls
through LR/CTR, the `bl $+4` get-PC idiom): a static recompiler models `blr` as
a host return, so branching through LR/CTR to an arbitrary mid-function address
is not expressible and is not tested.

## Known oracle limitations (masked or normalized in compare.py)

Every allowance is counted and printed at the end of a run.

- **FPSCR**: RPCS3 does not model it (mtfsf drops RN and the sticky bits; it
  keeps FPCC). The FPSCR instructions (mffs, mtfsf, mtfsfi, mtfsb0/1, mcrfs and
  their record forms) are therefore checked against an independent PowerISA
  reference in `compare.py` (`fpscr_reference`: FEX/VX summaries, FX rules,
  mcrfs clearing, CR1), not against the oracle. That reference and the
  lifter's implementation share an author; a misreading of the ISA would match
  on both sides. FP arithmetic does not yet update FPSCR status (FPRF, FR/FI,
  exception bits) in ps3recomp, so the `fpscr` field is ignored for every other
  op (`--ignore r1,fpscr`).
- **CR1 of FP record forms**: the ISA sets CR1 = FPSCR[FX FEX VX OX] (as
  ps3recomp does); RPCS3 writes the result's FPCC. Masked for arithmetic ops.
- **NaN operand priority**: PowerISA propagates the first NaN of FRA, FRB, FRC
  (quieted), signalling or not; RPCS3 inherits the host rule (an SNaN wins).
  Accepted only when ps3recomp produced exactly the ISA's NaN.
- **FMA-family NaN sign**: RPCS3 negates operands with host arithmetic, which
  flips a propagated NaN's sign; the ISA says QNaNs propagate unchanged.
- **fres / frsqrte**: estimates. RPCS3 reproduces the PPE's tables, ps3recomp
  computes exactly; accepted within the ISA bounds (1/256, 1/32). At the
  denormal edges RPCS3's table model flushes (fres results below single range
  become 0; frsqrte treats a denormal input as +-0). The ISA does neither;
  ps3recomp's ISA value is accepted. Real PPE behaviour here is unverified.
- **fnmsub / fnmadd zero sign**: the ISA negates after rounding, so an exact
  +0 from A*C -/+ B becomes -0. RPCS3's compiled interpreter returns +0 (its
  source is right; the build folds the negation). ps3recomp's -0 is accepted.
- **VSCR**: RPCS3 reads/writes it in big-endian word 0 of the vector; the ISA
  says word 3 (VRB bits 96:127). The harness feeds both words and compares
  RPCS3's word 0 with ps3recomp's word 3.
- **stwcx./stdcx.**: a store-conditional may fail spuriously; RPCS3's
  reservation model sometimes does. Oracle-failed / ours-succeeded is allowed.
- Undefined results (lve*x non-addressed lanes, mftb) are masked per case.
