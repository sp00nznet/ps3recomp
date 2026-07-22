#!/usr/bin/env python3
"""
Extract WWS SPU job code modules (C0DEC0DE) from LBP's SDATA archives.

Pipeline:
  1. SDATA-decrypt patch.sdat / data.sdat (same algorithm as the runtime's
     sdata_decrypt.h -- verified against RPCS3 Crypto/unedat.cpp).
  2. Find every zlib stream in the plaintext and inflate it FULLY (no size cap
     -- the earlier extraction capped at 32 KiB and truncated 5 modules, one of
     which had its entry point past the 32 KiB boundary).
  3. A module is `4 ila | entryOffset | imageSize | 0 | 0 | 0xC0DEC0DE`. Each
     inflated stream is exactly one module, so the inflated length IS the
     module's initialised (code+rodata) image; bss extends past it in LS.

Output: lbp_spu/jobmods/jobmod_<sha1[:12]>_e<entryOffset>.bin
"""
import glob, hashlib, os, struct, sys, zlib
from Crypto.Cipher import AES

SDAT_KEY   = bytes([0x0D,0x65,0x5E,0xF8,0xE6,0x74,0xA9,0x8A,0xB8,0x50,0x5C,0xFA,0x7D,0x01,0x29,0x33])
EDAT_KEY_0 = bytes([0xBE,0x95,0x9C,0xA8,0x30,0x8D,0xEF,0xA2,0xE5,0xE1,0x80,0xC6,0x37,0x12,0xA9,0xAE])
ZERO_IV    = b"\x00" * 16

def sdata_decrypt(d: bytes):
    if len(d) < 0x100 or d[:3] != b"NPD":
        return None
    digest   = d[0x40:0x50]
    dev_hash = d[0x60:0x70]
    flags      = struct.unpack_from(">I", d, 0x80)[0]
    block_size = struct.unpack_from(">I", d, 0x84)[0]
    file_size  = struct.unpack_from(">Q", d, 0x88)[0]
    if not (flags & 0x01000000):        # not SDATA
        return None
    if block_size == 0 or file_size == 0 or file_size > len(d):
        return None
    if (flags & 0x01) or (flags & 0x20):  # compressed / 0x20-metadata variants unused by LBP
        return None
    enc_key = (flags & 0x08) != 0
    crypt_key = bytes(dh ^ sk for dh, sk in zip(dev_hash, SDAT_KEY))
    total_blocks = (file_size + block_size - 1) // block_size
    out = bytearray()
    ecb = AES.new(crypt_key, AES.MODE_ECB)
    for bn in range(total_blocks):
        off = 0x100 + total_blocks * 0x10 + bn * block_size
        length = block_size
        if bn == total_blocks - 1 and (file_size % block_size):
            length = file_size % block_size
        padlen = (length + 15) & ~15
        b_key = dev_hash[:12] + struct.pack(">I", bn)
        key_result = ecb.encrypt(b_key)
        if enc_key:
            key_final = AES.new(EDAT_KEY_0, AES.MODE_CBC, ZERO_IV).decrypt(key_result)
        else:
            key_final = key_result
        block = AES.new(key_final, AES.MODE_CBC, digest).decrypt(d[off:off + padlen])
        out += block[:length]
    return bytes(out[:file_size])

def is_module(b: bytes) -> bool:
    # 4 ila (top byte 0x42/0x43) then C0DEC0DE marker at 0x20
    if len(b) < 0x30:
        return False
    if b[0] not in (0x42, 0x43):
        return False
    return b[0x20:0x24] == b"\xc0\xde\xc0\xde"

def _inflate_at(plain: bytes, i: int):
    """Inflate the zlib stream at offset i. Return (decompressed, compressed_len) or None."""
    if not (plain[i] == 0x78 and ((plain[i] << 8 | plain[i + 1]) % 31 == 0)):
        return None
    try:
        dobj = zlib.decompressobj()
        out = dobj.decompress(plain[i:])
        out += dobj.flush()
    except zlib.error:
        return None
    return out, len(plain[i:]) - len(dobj.unused_data)

def find_modules(plain: bytes):
    """Yield each SPU module. The FSHb stores files as 32 KiB-chunked zlib
    streams: a module = its header chunk + the following continuation chunks,
    concatenated. The module's total initialised (code+rodata) image is
    `imageSize(0x14) - 32`; keep appending consecutive streams until we reach it."""
    mods = []
    i = 0
    n = len(plain)
    while i < n - 1:
        r = _inflate_at(plain, i)
        if r is None:
            i += 1
            continue
        out, clen = r
        if is_module(out):
            target = struct.unpack_from(">I", out, 0x14)[0] - 32   # image minus fixed bss tail
            buf = bytearray(out)
            i += clen
            while len(buf) < target and i < n - 1:
                r2 = _inflate_at(plain, i)
                if r2 is None:
                    i += 1
                    continue
                cont, clen2 = r2
                if is_module(cont):          # ran into the next module -> stop
                    break
                buf += cont
                i += clen2
            mods.append(bytes(buf[:target]))
            continue
        i += clen
    return mods

def main():
    hdd0 = os.environ.get("PS3_HDD0_ROOT", "C:/Users/sewshee/Desktop/rpcs3/dev_hdd0")
    usrdir = os.path.join(hdd0, "game/BCUS98148/USRDIR")
    outdir = os.path.join(os.path.dirname(__file__), "..", "lbp_spu", "jobmods")
    outdir = os.path.abspath(outdir)
    os.makedirs(outdir, exist_ok=True)

    seen = {}   # sha1 -> module bytes
    for name in ("patch.sdat", "data.sdat"):
        path = os.path.join(usrdir, name)
        if not os.path.exists(path):
            print(f"  MISSING {path}", file=sys.stderr); continue
        raw = open(path, "rb").read()
        if raw[:4] == b"FSHb":
            plain = raw                       # hdd0 patch.sdat ships already-decrypted
        else:
            plain = sdata_decrypt(raw)
        if plain is None:
            print(f"  {name}: decrypt failed / not SDATA", file=sys.stderr); continue
        magic = plain[:4]
        mods = find_modules(plain)
        print(f"  {name}: decrypted {len(plain)} bytes (magic {magic!r}), {len(mods)} module(s)")
        for m in mods:
            h = hashlib.sha1(m).hexdigest()
            seen.setdefault(h, m)

    # write, dedup by content
    written = 0
    for h, m in sorted(seen.items()):
        entry = struct.unpack_from(">I", m, 0x10)[0]
        fn = f"jobmod_{h[:12]}_e{entry:04X}.bin"
        with open(os.path.join(outdir, fn), "wb") as f:
            f.write(m)
        written += 1
    print(f"Wrote {written} unique module(s) to {outdir}")

if __name__ == "__main__":
    main()
