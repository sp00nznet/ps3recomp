# RSX Fragment Program (NV40 ISA) → HLSL

Reference + status for the RSX fragment-program decompiler
(`libs/video/rsx_fp_decompiler.{h,c}`). The RSX fragment shader ISA is the
NVIDIA NV40 ("nvfx") fragment program encoding. Authoritative source for the
bit layout: Mesa/nouveau `src/gallium/drivers/nouveau/nv30/nvfx_shader.h`.

## Instruction format

Each instruction is **4× 32-bit words (16 bytes)**. A source operand of type
CONST is followed by an **inline 16-byte constant** (4 IEEE floats) occupying
the next instruction slot; the decoder skips it.

### Memory layout / word order
PS3 memory is big-endian. Fragment-program words additionally have their two
16-bit halves swapped relative to host order. So the host-order word is:

```
host = swap16halves( be32_load(ptr) )   where swap16halves(v) = (v<<16)|(v>>16)
```

`rsx_fp_read_word()` performs exactly this. The decoder proper operates on
already-host-order `u32` words so it can be unit-tested independently.

### DWORD 0 — OPDEST
| Field | Bits | Notes |
|---|---|---|
| PROGRAM_END | 0 | last instruction |
| OUT_REG | 1..6 | destination register index (6 bits) |
| OUT_REG_HALF | 7 | destination is an H (fp16) register |
| COND_WRITE_ENABLE | 8 | conditional write |
| OUT mask X/Y/Z/W | 9..12 | write mask |
| INPUT_SRC | 13..16 | interpolated input index (POSITION=0, COL0=1, COL1=2, FOGC=3, TC0=4, TC(n)=4+n, FACING=0xE) |
| TEX_UNIT | 17..20 | texture unit for TEX/TXP/… |
| PRECISION | 22..23 | |
| OPCODE | 24..29 | 6-bit opcode |
| OUT_NONE | 30 | no destination write |
| OUT_SAT | 31 | saturate result to [0,1] |

### DWORD 1/2/3 — SRC0 / SRC1 / SRC2
| Field | Bits | Notes |
|---|---|---|
| REG_TYPE | 0..1 | 0=TEMP, 1=INPUT, 2=CONST |
| SRC reg index | 2..7 | 6 bits (NV40 mask 63<<2) |
| SRC_HALF | 8 | source from H register file |
| SWZ X/Y/Z/W | 9..16 | 4× 2-bit swizzle (0=x,1=y,2=z,3=w) |
| NEGATE | 17 | negate source |

Per-source ABS lives outside the source word:
- SRC0_ABS = DWORD1 bit 29
- SRC1_ABS = DWORD2 bit 18
- SRC2 ABS — not modeled in MVP

## Opcodes (6-bit, from nvfx_shader.h)
```
NOP 0x00  MOV 0x01  MUL 0x02  ADD 0x03  MAD 0x04  DP3 0x05  DP4 0x06  DST 0x07
MIN 0x08  MAX 0x09  SLT 0x0A  SGE 0x0B  SLE 0x0C  SGT 0x0D  SNE 0x0E  SEQ 0x0F
FRC 0x10  FLR 0x11  KIL 0x12  PK4B 0x13 UP4B 0x14 DDX 0x15  DDY 0x16  TEX 0x17
TXP 0x18  TXD 0x19  RCP 0x1A  RSQ 0x1B* EX2 0x1C  LG2 0x1D  LIT 0x1E* LRP 0x1F*
STR 0x20  SFL 0x21  COS 0x22  SIN 0x23  PK2H 0x24 UP2H 0x25 POW 0x26* PK4UB 0x27
UP4UB 0x28 PK2US 0x29 UP2US 0x2A DP2A 0x2E TXL 0x2F* TXB 0x31  DIV 0x3A
```
`*` = NV30/NV40 variant value (RSQ_NV30=0x1B, LIT_NV30=0x1E, LRP_NV30=0x1F,
POW_NV30=0x26, TXL_NV40=0x2F, RFL_NV30=0x36).

Branch ops (when DWORD2 bit31 IS_BRANCH set): BRK0 CAL1 IF2 LOOP3 REP4 RET5.

## Decompiler status (MVP)
Implemented in the HLSL emitter: NOP, MOV, MUL, ADD, MAD, DP3, DP4, MIN, MAX,
FRC, FLR, RCP, RSQ, EX2, LG2, SLT/SGE/SLE/SGT/SNE/SEQ, COS, SIN, POW, LRP,
TEX/TXP, KIL. Per-source swizzle, negate, abs, and the dest write-mask are
honored. OUT_SAT wraps the result in `saturate()`.

Not yet handled (emit a `// TODO` comment, result left as the source / zero):
pack/unpack (PK*/UP*), DDX/DDY beyond XY, DST/LIT/DP2A/TXD/TXB, branching
(IF/LOOP/REP/CAL/RET), conditional writes (COND_WRITE_ENABLE), fp16 precision
differences (H registers are treated as full-precision float4).

### Integration TODO (not done this increment)
1. Wire `rsx_fp_decompile()` into the D3D12 backend: hash the bytecode at
   `fragment_program_addr`, decompile once, compile with `D3DCompile`, cache
   the PS blob, and fold the shader identity into the `PsoKey` so the PSO
   cache builds a pipeline per (shader × blend × depth).
2. **DONE.** Interpolated inputs COL0/COL1/FOG/TEXCOORD0..7 (+ WPOS via
   SV_POSITION) are plumbed through the passthrough VS + 12-element input
   layout; `input_expr` maps INPUT_SRC accordingly. TC8/TC9/FACING still → 0.
   Caveat: passthrough-VP model (raw attributes forwarded; the vertex program
   is not translated).
3. Upload fragment-program constants to a constant buffer (the inline CONST
   slots) and bind textures (depends on `d3d12_bind_texture`, still a stub).
