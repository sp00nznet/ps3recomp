"""triage_elf.py - quick recomp-tractability read on a decrypted PS3 EBOOT.elf.

Reports arch, exec-segment size, firmware module imports (raw .rodata string scan),
and SPU / online-NP flags -- the signals that actually predict porting effort
(genre does not; Pac-Man CE DX looked trivial but pulls SPU audio + NP store).

Usage: python triage_elf.py <EBOOT.elf> [<EBOOT.elf> ...]
"""
import re, struct, sys
from collections import Counter

SPU_MARKERS = ("cellSpurs", "cellSync", "sys_spu", "cellMSDSP", "cellMS",
               "_spu", "spu_thread", "cellFiber", "cellDmux", "cellVdec")
NP_MARKERS  = ("sceNp", "cellNetCtl", "sys_net", "cellHttp", "cellSsl",
               "cellRudp", "cellSubscriber")

def triage(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF":
        return f"{path}: not an ELF"
    e_machine = struct.unpack(">H", d[18:20])[0]
    e_phoff = struct.unpack(">Q", d[0x20:0x28])[0]
    e_phnum = struct.unpack(">H", d[0x38:0x3A])[0]
    e_phes  = struct.unpack(">H", d[0x36:0x38])[0]
    exec_sz = 0
    text = b""
    for i in range(e_phnum):
        o = e_phoff + i * e_phes
        p_type, p_flags = struct.unpack(">II", d[o:o+8])
        p_off = struct.unpack(">Q", d[o+8:o+16])[0]
        p_filesz = struct.unpack(">Q", d[o+32:o+40])[0]
        if p_type == 1 and (p_flags & 1):
            exec_sz = p_filesz; text = d[p_off:p_off+p_filesz]
    # module / lib name strings
    names = set(s.decode() for s in re.findall(rb'[a-zA-Z][a-zA-Z0-9_]{4,31}', d))
    mods = sorted(n for n in names if n.startswith(
        ("cell", "sys_", "sysPrx", "sceNp", "libsn", "libgcm")) and not n.endswith("_t"))
    spu = sorted(set(n for n in mods if any(m in n for m in SPU_MARKERS)))
    npm = sorted(set(n for n in mods if any(m in n for m in NP_MARKERS)))
    # function-frame proxy: 'stdu r1,-N(r1)' (0xF821xxxx) in exec segment
    frames = len(re.findall(b'\xf8\x21', text))
    # distinct bl targets (0x48xxxxx1 / 0x4Bxxxxx1) as a call-graph size proxy
    bl = 0
    for i in range(0, len(text) - 3, 4):
        w = text[i]
        if (w & 0xFC) == 0x48 and (text[i+3] & 1) == 1:
            bl += 1
    name = path.split("\\")[-1].split("/")[-2] if "/" in path or "\\" in path else path
    out = []
    out.append(f"=== {path}")
    out.append(f"  machine={e_machine}(21=PPC64)  exec={exec_sz//1024}KB  "
               f"~frames={frames}  bl-sites={bl}")
    out.append(f"  SPU markers ({len(spu)}): {', '.join(spu) if spu else 'NONE (good)'}")
    out.append(f"  NP/online ({len(npm)}): {', '.join(npm) if npm else 'NONE (good)'}")
    # the rest of the gfx/audio/util libs
    other = [m for m in mods if m not in spu and m not in npm]
    key = [m for m in other if m in (
        "cellGcmSys","cellResc","cellSysutil","cellSysmodule","cellAudio",
        "cellPngDec","cellJpgDec","cellGifDec","cellFont","cellFontFT","cellL10n",
        "cellGame","cellSaveData","cellPad","cellKb","cellMouse","cellFs",
        "sysPrxForUser","cellVideoOut","cellAtrac","cellSheap")]
    out.append(f"  key libs: {', '.join(sorted(key))}")
    return "\n".join(out)

if __name__ == "__main__":
    for p in sys.argv[1:]:
        print(triage(p)); print()
