#!/usr/bin/env python3
"""fres and frsqrte return the console's table estimates (#823).

On a Cell PPU, fres and frsqrte are not exact: fres indexes a 128-entry table
with the top 7 fraction bits of FRB, and frsqrte one of two 8-entry tables
(by exponent parity) with the top 3, then both rescale by the exponent. The
lifter used to emit 1.0 / x and 1.0 / sqrt(x), which matches the console on
fewer than 1 input in 60,000.

CONSOLE below holds 408 results recorded on a real PS3 (FW 4.93) by a
homebrew probe that ran each instruction alone on synthetic inputs: one input
per table entry, samples at 15 exponents near both ends and among the single
denormals, the fres overflow boundary, negatives, and the special values.
Inputs are single-precision bit patterns, widened to double the way lfs does
(a signalling NaN stays signalling). The full captures (every mantissa of
[1,2) and [2,4)) were checked against the same helpers outside this test.

The test lifts both instructions, compiles the emitted statements against the
lifter's own preamble and compares every result bit for bit.

Usage:
    python3 tools/test_ppu_fp_estimates.py
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from ppu_disasm import decode  # noqa: E402
from ppu_lifter import FENCE_PREAMBLE, LiftedFunction, PPULifter  # noqa: E402

# A-form, frD = f1, frB = f4 (Book I): fres is opcode 59 XO 24, frsqrte 63 XO 26.
FRES = (59 << 26) | (1 << 21) | (4 << 11) | (24 << 1)
FRSQRTE = (63 << 26) | (1 << 21) | (4 << 11) | (26 << 1)

# (op, input single bits, console FPR bits), recorded on a PS3.
CONSOLE = [
    ("fres", 0x3F80AC37, 0x3FEFE00000000000),
    ("fres", 0x3F817B19, 0x3FEFA10000000000),
    ("fres", 0x3F8252FD, 0x3FEF630000000000),
    ("fres", 0x3F832EC6, 0x3FEF260000000000),
    ("fres", 0x3F84B613, 0x3FEEEA0000000000),
    ("fres", 0x3F8564BB, 0x3FEEAE0000000000),
    ("fres", 0x3F866A3E, 0x3FEE740000000000),
    ("fres", 0x3F87B9ED, 0x3FEE3C0000000000),
    ("fres", 0x3F885C14, 0x3FEE000000000000),
    ("fres", 0x3F899675, 0x3FEDC80000000000),
    ("fres", 0x3F8A1845, 0x3FED940000000000),
    ("fres", 0x3F8B84AA, 0x3FED5C0000000000),
    ("fres", 0x3F8CA491, 0x3FED280000000000),
    ("fres", 0x3F8DC43C, 0x3FECF00000000000),
    ("fres", 0x3F8E6CED, 0x3FECC00000000000),
    ("fres", 0x3F8F8C6B, 0x3FEC8C0000000000),
    ("fres", 0x3F90331F, 0x3FEC580000000000),
    ("fres", 0x3F9179A7, 0x3FEC280000000000),
    ("fres", 0x3F925718, 0x3FEBF80000000000),
    ("fres", 0x3F9335CF, 0x3FEBC80000000000),
    ("fres", 0x3F941B97, 0x3FEB980000000000),
    ("fres", 0x3F954133, 0x3FEB680000000000),
    ("fres", 0x3F96513B, 0x3FEB380000000000),
    ("fres", 0x3F97366C, 0x3FEB080000000000),
    ("fres", 0x3F9873B6, 0x3FEAE00000000000),
    ("fres", 0x3F9998DB, 0x3FEAB00000000000),
    ("fres", 0x3F9A90D8, 0x3FEA800000000000),
    ("fres", 0x3F9B399E, 0x3FEA580000000000),
    ("fres", 0x3F9C6C98, 0x3FEA300000000000),
    ("fres", 0x3F9DF1EF, 0x3FEA000000000000),
    ("fres", 0x3F9EB312, 0x3FE9D80000000000),
    ("fres", 0x3F9F3919, 0x3FE9B00000000000),
    ("fres", 0x3FA08AD8, 0x3FE9800000000000),
    ("fres", 0x3FA1376E, 0x3FE9600000000000),
    ("fres", 0x3FA2882E, 0x3FE9300000000000),
    ("fres", 0x3FA3FA57, 0x3FE9100000000000),
    ("fres", 0x3FA48063, 0x3FE8E80000000000),
    ("fres", 0x3FA56BC8, 0x3FE8C00000000000),
    ("fres", 0x3FA6F363, 0x3FE8980000000000),
    ("fres", 0x3FA73BD5, 0x3FE8700000000000),
    ("fres", 0x3FA85757, 0x3FE8500000000000),
    ("fres", 0x3FA95243, 0x3FE8300000000000),
    ("fres", 0x3FAAA605, 0x3FE8000000000000),
    ("fres", 0x3FAB79FB, 0x3FE7E00000000000),
    ("fres", 0x3FAC4BA3, 0x3FE7C00000000000),
    ("fres", 0x3FADBCD7, 0x3FE7A00000000000),
    ("fres", 0x3FAED737, 0x3FE7780000000000),
    ("fres", 0x3FAF5657, 0x3FE7580000000000),
    ("fres", 0x3FB06329, 0x3FE7300000000000),
    ("fres", 0x3FB1B75D, 0x3FE7100000000000),
    ("fres", 0x3FB2AC76, 0x3FE6F00000000000),
    ("fres", 0x3FB35721, 0x3FE6D00000000000),
    ("fres", 0x3FB4F630, 0x3FE6B00000000000),
    ("fres", 0x3FB57CEE, 0x3FE6900000000000),
    ("fres", 0x3FB648B9, 0x3FE6700000000000),
    ("fres", 0x3FB7AE5A, 0x3FE6500000000000),
    ("fres", 0x3FB8557B, 0x3FE6300000000000),
    ("fres", 0x3FB9C2BB, 0x3FE6100000000000),
    ("fres", 0x3FBAF040, 0x3FE5F00000000000),
    ("fres", 0x3FBB6754, 0x3FE5D80000000000),
    ("fres", 0x3FBCD83E, 0x3FE5C00000000000),
    ("fres", 0x3FBDF222, 0x3FE5A00000000000),
    ("fres", 0x3FBEC643, 0x3FE5800000000000),
    ("fres", 0x3FBF52C0, 0x3FE5600000000000),
    ("fres", 0x3FC0E9AC, 0x3FE5400000000000),
    ("fres", 0x3FC15525, 0x3FE5300000000000),
    ("fres", 0x3FC25C21, 0x3FE5100000000000),
    ("fres", 0x3FC3B060, 0x3FE4F00000000000),
    ("fres", 0x3FC4154D, 0x3FE4D80000000000),
    ("fres", 0x3FC54CB1, 0x3FE4C00000000000),
    ("fres", 0x3FC64937, 0x3FE4A00000000000),
    ("fres", 0x3FC72FC1, 0x3FE4880000000000),
    ("fres", 0x3FC86D23, 0x3FE4700000000000),
    ("fres", 0x3FC954BC, 0x3FE4500000000000),
    ("fres", 0x3FCA2BAE, 0x3FE4400000000000),
    ("fres", 0x3FCBAAF6, 0x3FE4200000000000),
    ("fres", 0x3FCC5E6F, 0x3FE4000000000000),
    ("fres", 0x3FCD19A3, 0x3FE3F00000000000),
    ("fres", 0x3FCE2AEC, 0x3FE3D00000000000),
    ("fres", 0x3FCF124D, 0x3FE3C00000000000),
    ("fres", 0x3FD04A84, 0x3FE3A00000000000),
    ("fres", 0x3FD1F22F, 0x3FE3900000000000),
    ("fres", 0x3FD234F0, 0x3FE3700000000000),
    ("fres", 0x3FD30A3A, 0x3FE3600000000000),
    ("fres", 0x3FD459E2, 0x3FE3400000000000),
    ("fres", 0x3FD50B3C, 0x3FE3300000000000),
    ("fres", 0x3FD64467, 0x3FE3200000000000),
    ("fres", 0x3FD7320D, 0x3FE3000000000000),
    ("fres", 0x3FD83887, 0x3FE2F00000000000),
    ("fres", 0x3FD94DE2, 0x3FE2D00000000000),
    ("fres", 0x3FDA5B16, 0x3FE2C00000000000),
    ("fres", 0x3FDB3D98, 0x3FE2B00000000000),
    ("fres", 0x3FDCF2AC, 0x3FE2900000000000),
    ("fres", 0x3FDDABB5, 0x3FE2800000000000),
    ("fres", 0x3FDE6D39, 0x3FE2700000000000),
    ("fres", 0x3FDFC989, 0x3FE2500000000000),
    ("fres", 0x3FE06936, 0x3FE2400000000000),
    ("fres", 0x3FE139A4, 0x3FE2300000000000),
    ("fres", 0x3FE230A9, 0x3FE2100000000000),
    ("fres", 0x3FE326AF, 0x3FE2000000000000),
    ("fres", 0x3FE49239, 0x3FE1F00000000000),
    ("fres", 0x3FE5FC9C, 0x3FE1E00000000000),
    ("fres", 0x3FE65125, 0x3FE1C00000000000),
    ("fres", 0x3FE70FA6, 0x3FE1B00000000000),
    ("fres", 0x3FE8B179, 0x3FE1A00000000000),
    ("fres", 0x3FE9CB5D, 0x3FE1900000000000),
    ("fres", 0x3FEA1662, 0x3FE1700000000000),
    ("fres", 0x3FEB268D, 0x3FE1600000000000),
    ("fres", 0x3FEC2070, 0x3FE1500000000000),
    ("fres", 0x3FED3E4D, 0x3FE1400000000000),
    ("fres", 0x3FEE27F3, 0x3FE1300000000000),
    ("fres", 0x3FEFCD7C, 0x3FE1200000000000),
    ("fres", 0x3FF05D8E, 0x3FE1000000000000),
    ("fres", 0x3FF1CBBA, 0x3FE0F00000000000),
    ("fres", 0x3FF2C900, 0x3FE0E00000000000),
    ("fres", 0x3FF37756, 0x3FE0D00000000000),
    ("fres", 0x3FF46C2A, 0x3FE0C00000000000),
    ("fres", 0x3FF5FC22, 0x3FE0B00000000000),
    ("fres", 0x3FF69C0D, 0x3FE0A00000000000),
    ("fres", 0x3FF7AEAB, 0x3FE0900000000000),
    ("fres", 0x3FF8B718, 0x3FE0800000000000),
    ("fres", 0x3FF9C459, 0x3FE0700000000000),
    ("fres", 0x3FFAA633, 0x3FE0600000000000),
    ("fres", 0x3FFB6FA4, 0x3FE0500000000000),
    ("fres", 0x3FFC554B, 0x3FE0400000000000),
    ("fres", 0x3FFD7201, 0x3FE0300000000000),
    ("fres", 0x3FFEF4B1, 0x3FE0200000000000),
    ("fres", 0x3FFFB12C, 0x3FE0000000000000),
    ("fres", 0x00051FDC, 0x7FF0000000000000),
    ("fres", 0x00791572, 0x47D0E00000000000),
    ("fres", 0x00698C0B, 0x47D3600000000000),
    ("fres", 0x002D498A, 0x47E6900000000000),
    ("fres", 0x004A9125, 0x47DB680000000000),
    ("fres", 0x0053ECC0, 0x47D8700000000000),
    ("fres", 0x00D65B00, 0x47C3200000000000),
    ("fres", 0x00F3E200, 0x47C0D00000000000),
    ("fres", 0x00F80500, 0x47C0800000000000),
    ("fres", 0x00D77200, 0x47C3000000000000),
    ("fres", 0x00B9AC00, 0x47C6100000000000),
    ("fres", 0x00D50F00, 0x47C3300000000000),
    ("fres", 0x01086B00, 0x47BE000000000000),
    ("fres", 0x010D4300, 0x47BCF00000000000),
    ("fres", 0x014B3300, 0x47B4200000000000),
    ("fres", 0x016D2C00, 0x47B1400000000000),
    ("fres", 0x0175F000, 0x47B0B00000000000),
    ("fres", 0x0172BD00, 0x47B0E00000000000),
    ("fres", 0x019DCC00, 0x47AA000000000000),
    ("fres", 0x01B90500, 0x47A6100000000000),
    ("fres", 0x01C62F00, 0x47A4A00000000000),
    ("fres", 0x01E42100, 0x47A1F00000000000),
    ("fres", 0x01ADA600, 0x47A7A00000000000),
    ("fres", 0x01C1D600, 0x47A5300000000000),
    ("fres", 0x02150600, 0x479B680000000000),
    ("fres", 0x02652100, 0x4791E00000000000),
    ("fres", 0x02224B00, 0x4799300000000000),
    ("fres", 0x02311700, 0x4797100000000000),
    ("fres", 0x020CED00, 0x479D280000000000),
    ("fres", 0x023A8900, 0x4795F00000000000),
    ("fres", 0x1F800000, 0x43EFE00000000000),
    ("fres", 0x3E7D1A00, 0x4010300000000000),
    ("fres", 0x3E4DCD00, 0x4013F00000000000),
    ("fres", 0x3E3B1C00, 0x4015D80000000000),
    ("fres", 0x3E5CB800, 0x4012900000000000),
    ("fres", 0x3E5F7D00, 0x4012500000000000),
    ("fres", 0x3E4ADF00, 0x4014400000000000),
    ("fres", 0x3E80A500, 0x400FE00000000000),
    ("fres", 0x3ECE5D00, 0x4003D00000000000),
    ("fres", 0x3EC93E00, 0x4004500000000000),
    ("fres", 0x3EBA6800, 0x4005F00000000000),
    ("fres", 0x3EA89200, 0x4008500000000000),
    ("fres", 0x3EE21800, 0x4002100000000000),
    ("fres", 0x3F37D100, 0x3FF6500000000000),
    ("fres", 0x3F3F8800, 0x3FF5600000000000),
    ("fres", 0x3F318600, 0x3FF7100000000000),
    ("fres", 0x3F095400, 0x3FFDC80000000000),
    ("fres", 0x3F24A500, 0x3FF8E80000000000),
    ("fres", 0x3F215000, 0x3FF9600000000000),
    ("fres", 0x40249F00, 0x3FD8E80000000000),
    ("fres", 0x40333700, 0x3FD6D00000000000),
    ("fres", 0x40254200, 0x3FD8C00000000000),
    ("fres", 0x406C4700, 0x3FD1500000000000),
    ("fres", 0x40331D00, 0x3FD6D00000000000),
    ("fres", 0x407F3300, 0x3FD0000000000000),
    ("fres", 0x40A94F00, 0x3FC8300000000000),
    ("fres", 0x40E10000, 0x3FC2300000000000),
    ("fres", 0x40F0C000, 0x3FC1000000000000),
    ("fres", 0x40FD1600, 0x3FC0300000000000),
    ("fres", 0x40C4A700, 0x3FC4D80000000000),
    ("fres", 0x40C2F800, 0x3FC5100000000000),
    ("fres", 0x411CAD00, 0x3FBA300000000000),
    ("fres", 0x410B6400, 0x3FBD5C0000000000),
    ("fres", 0x41619C00, 0x3FB2300000000000),
    ("fres", 0x410BF200, 0x3FBD5C0000000000),
    ("fres", 0x412AEB00, 0x3FB8000000000000),
    ("fres", 0x41692000, 0x3FB1900000000000),
    ("fres", 0x5F800000, 0x3BEFE00000000000),
    ("fres", 0x7D1BA500, 0x383A580000000000),
    ("fres", 0x7D175400, 0x383B080000000000),
    ("fres", 0x7D473A00, 0x3834880000000000),
    ("fres", 0x7D220B00, 0x3839300000000000),
    ("fres", 0x7D5EE500, 0x3832700000000000),
    ("fres", 0x7D4E3500, 0x3833D00000000000),
    ("fres", 0x7D8E4000, 0x382CC00000000000),
    ("fres", 0x7DB1BD00, 0x3827100000000000),
    ("fres", 0x7DC5CC00, 0x3824C00000000000),
    ("fres", 0x7D9FF500, 0x3829B00000000000),
    ("fres", 0x7D8A7700, 0x382D940000000000),
    ("fres", 0x7DCAE900, 0x3824400000000000),
    ("fres", 0x7E386D00, 0x3816300000000000),
    ("fres", 0x7E17D700, 0x381B080000000000),
    ("fres", 0x7E19BA00, 0x381AB00000000000),
    ("fres", 0x7E16DF00, 0x381B380000000000),
    ("fres", 0x7E227200, 0x3819300000000000),
    ("fres", 0x7E55A000, 0x3813300000000000),
    ("fres", 0x7EDCF900, 0x3802900000000000),
    ("fres", 0x7EA93400, 0x3808300000000000),
    ("fres", 0x7E9CD300, 0x380A300000000000),
    ("fres", 0x7EA1E400, 0x3809600000000000),
    ("fres", 0x7EE48900, 0x3801F00000000000),
    ("fres", 0x7EDF0B00, 0x3802500000000000),
    ("fres", 0x7F162400, 0x37FB380000000000),
    ("fres", 0x7F5CA300, 0x37F2900000000000),
    ("fres", 0x7F5B2B00, 0x37F2B00000000000),
    ("fres", 0x7F533400, 0x37F3600000000000),
    ("fres", 0x7F401600, 0x37F5400000000000),
    ("fres", 0x7F542F00, 0x37F3400000000000),
    ("fres", 0x7F800000, 0x0000000000000000),
    ("fres", 0xFFC00000, 0xFFF8000000000000),
    ("fres", 0xFF800000, 0x8000000000000000),
    ("fres", 0xFFA00000, 0xFFFC000000000000),
    ("fres", 0x7FA00000, 0x7FFC000000000000),
    ("fres", 0x7FC00000, 0x7FF8000000000000),
    ("fres", 0x001FFDE3, 0x7FF0000000000000),
    ("fres", 0x001FFEE2, 0x7FF0000000000000),
    ("fres", 0x001FFFE1, 0x7FF0000000000000),
    ("fres", 0x002000E0, 0x47EFE00000000000),
    ("fres", 0x002001DF, 0x47EFE00000000000),
    ("fres", 0x002002DE, 0x47EFE00000000000),
    ("fres", 0x00000000, 0x7FF0000000000000),
    ("fres", 0x80000000, 0xFFF0000000000000),
    ("fres", 0x3F800000, 0x3FEFE00000000000),
    ("fres", 0xBF800000, 0xBFEFE00000000000),
    ("fres", 0x40000000, 0x3FDFE00000000000),
    ("fres", 0x40800000, 0x3FCFE00000000000),
    ("fres", 0x00000001, 0x7FF0000000000000),
    ("fres", 0x80000001, 0xFFF0000000000000),
    ("fres", 0x007FFFFF, 0x47D0000000000000),
    ("fres", 0x807FFFFF, 0xC7D0000000000000),
    ("fres", 0x00800000, 0x47CFE00000000000),
    ("fres", 0x80800000, 0xC7CFE00000000000),
    ("fres", 0x7F7FFFFF, 0x37F0000000000000),
    ("fres", 0xFF7FFFFF, 0xB7F0000000000000),
    ("fres", 0x3F7FFFFF, 0x3FF0000000000000),
    ("fres", 0x3F800001, 0x3FEFE00000000000),
    ("fres", 0x3FFFFFFF, 0x3FE0000000000000),
    ("fres", 0x00400000, 0x47DFE00000000000),
    ("fres", 0xC0000000, 0xBFDFE00000000000),
    ("fres", 0xC0800000, 0xBFCFE00000000000),
    ("fres", 0xBFE77100, 0xBFE1B00000000000),
    ("fres", 0xBFB70500, 0xBFE6500000000000),
    ("fres", 0xBFF48500, 0xBFE0C00000000000),
    ("fres", 0xBFCDF300, 0xBFE3F00000000000),
    ("fres", 0xBF840D00, 0xBFEEEA0000000000),
    ("fres", 0xBFB44100, 0xBFE6B00000000000),
    ("fres", 0xBF976100, 0xBFEB080000000000),
    ("fres", 0xBFB06100, 0xBFE7300000000000),
    ("frsqrte", 0x3F897818, 0x3FEF100000000000),
    ("frsqrte", 0x3F98B473, 0x3FED800000000000),
    ("frsqrte", 0x3FA15BB6, 0x3FEC000000000000),
    ("frsqrte", 0x3FB5463D, 0x3FEA800000000000),
    ("frsqrte", 0x3FCB6497, 0x3FE9800000000000),
    ("frsqrte", 0x3FD0B616, 0x3FE8800000000000),
    ("frsqrte", 0x3FEE1875, 0x3FE8000000000000),
    ("frsqrte", 0x3FF576CF, 0x3FE7000000000000),
    ("frsqrte", 0x40085F0E, 0x3FE6000000000000),
    ("frsqrte", 0x401B90BB, 0x3FE4C00000000000),
    ("frsqrte", 0x402ABAFC, 0x3FE3C00000000000),
    ("frsqrte", 0x4037E6BF, 0x3FE3000000000000),
    ("frsqrte", 0x404B26E0, 0x3FE2000000000000),
    ("frsqrte", 0x405B37B4, 0x3FE1800000000000),
    ("frsqrte", 0x4066FCA7, 0x3FE1000000000000),
    ("frsqrte", 0x407EA279, 0x3FE0800000000000),
    ("frsqrte", 0x0055D3D7, 0x43E3C00000000000),
    ("frsqrte", 0x00189F49, 0x43F2000000000000),
    ("frsqrte", 0x00565555, 0x43E3C00000000000),
    ("frsqrte", 0x007D6D16, 0x43E0800000000000),
    ("frsqrte", 0x003ED2EF, 0x43E7000000000000),
    ("frsqrte", 0x0004BA42, 0x4404C00000000000),
    ("frsqrte", 0x00EDC600, 0x43D8000000000000),
    ("frsqrte", 0x00FAEA00, 0x43D7000000000000),
    ("frsqrte", 0x00D36000, 0x43D8800000000000),
    ("frsqrte", 0x0099C800, 0x43DD800000000000),
    ("frsqrte", 0x00937A00, 0x43DD800000000000),
    ("frsqrte", 0x00F14F00, 0x43D7000000000000),
    ("frsqrte", 0x01583800, 0x43D1800000000000),
    ("frsqrte", 0x01636F00, 0x43D1000000000000),
    ("frsqrte", 0x01748A00, 0x43D0800000000000),
    ("frsqrte", 0x0104CC00, 0x43D6000000000000),
    ("frsqrte", 0x014A8D00, 0x43D2000000000000),
    ("frsqrte", 0x0156EF00, 0x43D1800000000000),
    ("frsqrte", 0x019BDC00, 0x43CD800000000000),
    ("frsqrte", 0x01BB3A00, 0x43CA800000000000),
    ("frsqrte", 0x01D13F00, 0x43C8800000000000),
    ("frsqrte", 0x018EB200, 0x43CF100000000000),
    ("frsqrte", 0x01D4F400, 0x43C8800000000000),
    ("frsqrte", 0x01E52500, 0x43C8000000000000),
    ("frsqrte", 0x021D2000, 0x43C4C00000000000),
    ("frsqrte", 0x0254F300, 0x43C1800000000000),
    ("frsqrte", 0x023CF900, 0x43C3000000000000),
    ("frsqrte", 0x026CF300, 0x43C1000000000000),
    ("frsqrte", 0x02225B00, 0x43C3C00000000000),
    ("frsqrte", 0x021CBE00, 0x43C4C00000000000),
    ("frsqrte", 0x1F800000, 0x41EF100000000000),
    ("frsqrte", 0x3E0CFA00, 0x4006000000000000),
    ("frsqrte", 0x3E76FD00, 0x4000800000000000),
    ("frsqrte", 0x3E75B100, 0x4000800000000000),
    ("frsqrte", 0x3E7A2100, 0x4000800000000000),
    ("frsqrte", 0x3E2A4800, 0x4003C00000000000),
    ("frsqrte", 0x3E443B00, 0x4002000000000000),
    ("frsqrte", 0x3EA63600, 0x3FFC000000000000),
    ("frsqrte", 0x3E816D00, 0x3FFF100000000000),
    ("frsqrte", 0x3E863900, 0x3FFF100000000000),
    ("frsqrte", 0x3E8D1D00, 0x3FFF100000000000),
    ("frsqrte", 0x3EE72E00, 0x3FF8000000000000),
    ("frsqrte", 0x3E806F00, 0x3FFF100000000000),
    ("frsqrte", 0x3F3F8500, 0x3FF3000000000000),
    ("frsqrte", 0x3F460600, 0x3FF2000000000000),
    ("frsqrte", 0x3F589600, 0x3FF1800000000000),
    ("frsqrte", 0x3F3C9000, 0x3FF3000000000000),
    ("frsqrte", 0x3F157100, 0x3FF4C00000000000),
    ("frsqrte", 0x3F587D00, 0x3FF1800000000000),
    ("frsqrte", 0x40A8EB00, 0x3FDC000000000000),
    ("frsqrte", 0x40A7B700, 0x3FDC000000000000),
    ("frsqrte", 0x40F00800, 0x3FD7000000000000),
    ("frsqrte", 0x40F95F00, 0x3FD7000000000000),
    ("frsqrte", 0x40F96D00, 0x3FD7000000000000),
    ("frsqrte", 0x40897300, 0x3FDF100000000000),
    ("frsqrte", 0x41401700, 0x3FD2000000000000),
    ("frsqrte", 0x415F2D00, 0x3FD1800000000000),
    ("frsqrte", 0x410AD000, 0x3FD6000000000000),
    ("frsqrte", 0x4175F700, 0x3FD0800000000000),
    ("frsqrte", 0x41641F00, 0x3FD1000000000000),
    ("frsqrte", 0x412E0300, 0x3FD3C00000000000),
    ("frsqrte", 0x5F800000, 0x3DEF100000000000),
    ("frsqrte", 0x7D15AC00, 0x3C14C00000000000),
    ("frsqrte", 0x7D1FDB00, 0x3C14C00000000000),
    ("frsqrte", 0x7D016300, 0x3C16000000000000),
    ("frsqrte", 0x7D740D00, 0x3C10800000000000),
    ("frsqrte", 0x7D5C2100, 0x3C11800000000000),
    ("frsqrte", 0x7D3E4D00, 0x3C13000000000000),
    ("frsqrte", 0x7DEEDE00, 0x3C08000000000000),
    ("frsqrte", 0x7DAD2B00, 0x3C0C000000000000),
    ("frsqrte", 0x7DF21200, 0x3C07000000000000),
    ("frsqrte", 0x7DF7CD00, 0x3C07000000000000),
    ("frsqrte", 0x7DE06A00, 0x3C08000000000000),
    ("frsqrte", 0x7D840B00, 0x3C0F100000000000),
    ("frsqrte", 0x7E687100, 0x3C01000000000000),
    ("frsqrte", 0x7E2CB600, 0x3C03C00000000000),
    ("frsqrte", 0x7E6AF100, 0x3C01000000000000),
    ("frsqrte", 0x7E7A1500, 0x3C00800000000000),
    ("frsqrte", 0x7E716100, 0x3C00800000000000),
    ("frsqrte", 0x7E2D4900, 0x3C03C00000000000),
    ("frsqrte", 0x7ED72E00, 0x3BF8800000000000),
    ("frsqrte", 0x7EF4F300, 0x3BF7000000000000),
    ("frsqrte", 0x7E9AAA00, 0x3BFD800000000000),
    ("frsqrte", 0x7E8B0F00, 0x3BFF100000000000),
    ("frsqrte", 0x7EE2F400, 0x3BF8000000000000),
    ("frsqrte", 0x7EEF3200, 0x3BF8000000000000),
    ("frsqrte", 0x7F5FC800, 0x3BF1800000000000),
    ("frsqrte", 0x7F058800, 0x3BF6000000000000),
    ("frsqrte", 0x7F738A00, 0x3BF0800000000000),
    ("frsqrte", 0x7F1DF600, 0x3BF4C00000000000),
    ("frsqrte", 0x7F3ADD00, 0x3BF3000000000000),
    ("frsqrte", 0x7F16E900, 0x3BF4C00000000000),
    ("frsqrte", 0xFFA00000, 0xFFFC000000000000),
    ("frsqrte", 0x7FC00000, 0x7FF8000000000000),
    ("frsqrte", 0x7F800000, 0x0000000000000000),
    ("frsqrte", 0xFFC00000, 0xFFF8000000000000),
    ("frsqrte", 0xFF800000, 0x7FF8000000000000),
    ("frsqrte", 0x7FA00000, 0x7FFC000000000000),
    ("frsqrte", 0x00000000, 0x7FF0000000000000),
    ("frsqrte", 0x80000000, 0xFFF0000000000000),
    ("frsqrte", 0x3F800000, 0x3FEF100000000000),
    ("frsqrte", 0xBF800000, 0x7FF8000000000000),
    ("frsqrte", 0x40000000, 0x3FE6000000000000),
    ("frsqrte", 0x40800000, 0x3FDF100000000000),
    ("frsqrte", 0x00000001, 0x4496000000000000),
    ("frsqrte", 0x80000001, 0x7FF8000000000000),
    ("frsqrte", 0x007FFFFF, 0x43E0800000000000),
    ("frsqrte", 0x807FFFFF, 0x7FF8000000000000),
    ("frsqrte", 0x00800000, 0x43DF100000000000),
    ("frsqrte", 0x80800000, 0x7FF8000000000000),
    ("frsqrte", 0x7F7FFFFF, 0x3BF0800000000000),
    ("frsqrte", 0xFF7FFFFF, 0x7FF8000000000000),
    ("frsqrte", 0x3F7FFFFF, 0x3FF0800000000000),
    ("frsqrte", 0x3F800001, 0x3FEF100000000000),
    ("frsqrte", 0x3FFFFFFF, 0x3FE7000000000000),
    ("frsqrte", 0x00400000, 0x43E6000000000000),
    ("frsqrte", 0xC0000000, 0x7FF8000000000000),
    ("frsqrte", 0xC0800000, 0x7FF8000000000000),
    ("frsqrte", 0xBFDFC500, 0x7FF8000000000000),
    ("frsqrte", 0xBFC7FB00, 0x7FF8000000000000),
    ("frsqrte", 0xBFF1E400, 0x7FF8000000000000),
    ("frsqrte", 0xBFA83000, 0x7FF8000000000000),
    ("frsqrte", 0xBFB70500, 0x7FF8000000000000),
    ("frsqrte", 0xBFDE0000, 0x7FF8000000000000),
    ("frsqrte", 0xBFD12500, 0x7FF8000000000000),
    ("frsqrte", 0xBFE7D500, 0x7FF8000000000000),
]


def widen(s: int) -> int:
    """lfs: single bits -> double bits, without quieting a signalling NaN."""
    if (s >> 23) & 0xFF == 0xFF and s & 0x7FFFFF:
        return ((s >> 31) << 63) | (0x7FF << 52) | ((s & 0x7FFFFF) << 29)
    return struct.unpack(">Q", struct.pack(">d", struct.unpack(">f", struct.pack(">I", s))[0]))[0]


def emit(word: int) -> str:
    lifter = PPULifter()
    func = LiftedFunction(name="est_test", start_addr=0x1000, end_addr=0x2000)
    return lifter._translate_op(decode(word, 0x1000), func)


def check_text():
    fres, frsqrte = emit(FRES), emit(FRSQRTE)
    assert decode(FRES, 0x1000).mnemonic.rstrip(".") == "fres", decode(FRES, 0x1000)
    assert decode(FRSQRTE, 0x1000).mnemonic.rstrip(".") == "frsqrte", decode(FRSQRTE, 0x1000)
    assert "ppu_fres(ctx->fpr[4])" in fres, fres
    assert "ppu_frsqrte(ctx->fpr[4])" in frsqrte, frsqrte
    assert "sqrt(" not in frsqrte and "1.0 /" not in fres, (fres, frsqrte)
    return fres, frsqrte


def driver(fres: str, frsqrte: str) -> str:
    pre = PPULifter()._preamble_lines()
    pre[0] = pre[0].replace('#include "ppu_recomp.h"', '#include "ppu_context.h"\n' + FENCE_PREAMBLE)
    rows = ",\n".join(f'{{op_{op}, "{op}", 0x{s:08X}u, 0x{widen(s):016X}ull, 0x{want:016X}ull}}'
                      for op, s, want in CONSOLE)
    return "\n".join(pre) + f"""
#include <stdio.h>
#include <inttypes.h>
static void op_fres(ppu_context* ctx) {{ {fres} }}
static void op_frsqrte(ppu_context* ctx) {{ {frsqrte} }}
struct Case {{ void (*op)(ppu_context*); const char* mn; uint32_t single; uint64_t in, want; }};
static const Case cases[] = {{
{rows}
}};
static ppu_context ctx;
int main()
{{
    int bad = 0;
    for (const Case& c : cases) {{
        memcpy(&ctx.fpr[4], &c.in, 8);
        c.op(&ctx);
        uint64_t got; memcpy(&got, &ctx.fpr[1], 8);
        if (got != c.want) {{
            if (bad++ < 20)
                printf("%s(%08X): got %016" PRIX64 ", console %016" PRIX64 "\\n", c.mn, c.single, got, c.want);
        }}
    }}
    printf("%d of %d console results differ\\n", bad, (int)(sizeof cases / sizeof cases[0]));
    return bad != 0;
}}
"""


def main():
    fres, frsqrte = check_text()
    with tempfile.TemporaryDirectory(prefix="ppu-fp-est-") as tmp:
        root = Path(tmp)
        source = root / "est.cpp"
        exe = root / ("est.exe" if os.name == "nt" else "est")
        source.write_text(driver(fres, frsqrte))
        if os.name == "nt":
            from msvc_env import find_vcvars
            vcvars = find_vcvars()
            if not vcvars:
                raise SystemExit("Visual Studio environment is required; no silent skip.")
            batch = root / "run.bat"
            batch.write_text("@echo off\n"
                             f'call "{vcvars}" >nul 2>nul\n'
                             f'cd /d "{root}"\n'
                             f'cl /nologo /std:c++17 /O1 /W3 /fp:precise /I "{ROOT / "runtime/ppu"}" /Fe:"{exe}" "{source}"\n'
                             "if errorlevel 1 exit /b 2\n"
                             f'"{exe}"\n')
            return subprocess.run(["cmd", "/c", str(batch)]).returncode
        cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not cxx:
            raise SystemExit("A C++ compiler is required; no silent skip.")
        subprocess.run([cxx, "-std=c++17", "-O2", "-ffp-contract=off", "-Wno-parentheses", "-I",
                        str(ROOT / "runtime/ppu"), str(source), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
