"""pkg_extract.py - extract files from a PS3 .pkg (no RPCS3 GUI needed).

Handles both finalized (retail, AES-128-CTR with the public PS3_GPKG key) and
non-finalized/debug (SHA-1 keystream) packages. DRM-free (type 3) content needs
no per-title RAP/klicensee, so the whole PlayStation Minis catalog opens up.

Files are written under out_dir with the package's own directory tree intact.
Auto-detects the keystream by checking that the decrypted file table yields ASCII
filenames. Both keystreams are per-block seekable, so with --only we decrypt just the
file table plus the one requested file (a few MB) instead of the whole package -- and the
AES runs through pycryptodome when present, so a multi-GB PKG that used to take an hour of
pure-Python AES now finishes in seconds. Usage:
    python pkg_extract.py <in.pkg> <out_dir> [--only EBOOT.BIN]
"""
import hashlib, os, struct, sys

PS3_GPKG_KEY = bytes.fromhex("2e7b71d7c9c9a14ea3221f188828b8f8")
# PSP/PS1-Classic packages (pkg_type==2, ext-header key_type 1) use the same
# CTR construction with a different key.
PSP_PKG_KEY  = bytes.fromhex("07f2c68290b50d2c33818d709b60e62b")

try:                                    # fast C AES; a project dependency (requirements.txt)
    from Crypto.Cipher import AES as _AES
    _HAVE_PYCRYPTO = True
except ImportError:
    _HAVE_PYCRYPTO = False

# --- pure-python AES-128 ECB fallback (encrypt path only, for the CTR keystream) -------
_sbox = []
def _init_aes():
    p = q = 1
    sbox = [0]*256
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1; q ^= q << 2; q ^= q << 4; q &= 0xFF
        if q & 0x80: q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        sbox[p] = (x ^ 0x63) & 0xFF
        if p == 1: break
    sbox[0] = 0x63
    return sbox
def _xtime(a): return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else (a << 1)
def _aes_expand(key):
    sbox = _sbox
    rcon = 1; w = [list(key[i*4:i*4+4]) for i in range(4)]
    for i in range(4, 44):
        t = list(w[i-1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [sbox[b] for b in t]
            t[0] ^= rcon; rcon = _xtime(rcon)
        w.append([w[i-4][j] ^ t[j] for j in range(4)])
    return w
def _aes_encrypt_block(key_sched, block):
    sbox = _sbox
    s = [[block[r+4*c] for c in range(4)] for r in range(4)]
    def addrk(rnd):
        for c in range(4):
            for r in range(4):
                s[r][c] ^= key_sched[rnd*4+c][r]
    addrk(0)
    for rnd in range(1, 10):
        for r in range(4):
            for c in range(4):
                s[r][c] = sbox[s[r][c]]
        s[1] = s[1][1:] + s[1][:1]; s[2] = s[2][2:] + s[2][:2]; s[3] = s[3][3:] + s[3][:3]
        for c in range(4):
            a = [s[r][c] for r in range(4)]
            s[0][c] = _xtime(a[0]) ^ (_xtime(a[1]) ^ a[1]) ^ a[2] ^ a[3]
            s[1][c] = a[0] ^ _xtime(a[1]) ^ (_xtime(a[2]) ^ a[2]) ^ a[3]
            s[2][c] = a[0] ^ a[1] ^ _xtime(a[2]) ^ (_xtime(a[3]) ^ a[3])
            s[3][c] = (_xtime(a[0]) ^ a[0]) ^ a[1] ^ a[2] ^ _xtime(a[3])
        addrk(rnd)
    for r in range(4):
        for c in range(4):
            s[r][c] = sbox[s[r][c]]
    s[1] = s[1][1:] + s[1][:1]; s[2] = s[2][2:] + s[2][:2]; s[3] = s[3][3:] + s[3][:3]
    addrk(10)
    return bytes(s[r][c] for c in range(4) for r in range(4))


def _ecb_encrypt(key: bytes, blocks: bytes) -> bytes:
    if _HAVE_PYCRYPTO:
        return _AES.new(key, _AES.MODE_ECB).encrypt(blocks)
    global _sbox
    if not _sbox:
        _sbox = _init_aes()
    sched = _aes_expand(key)
    return b"".join(_aes_encrypt_block(sched, blocks[i:i+16]) for i in range(0, len(blocks), 16))


def _keystream(used, riv, hdr60, start_block, nblocks) -> bytes:
    """Keystream for blocks [start_block, start_block+nblocks) -- seekable, so only the
    range we actually XOR is generated."""
    if used in ("finalized", "psp"):
        base = int.from_bytes(riv, "big")
        ctrs = b"".join(((base + start_block + i) & ((1 << 128) - 1)).to_bytes(16, "big")
                        for i in range(nblocks))
        return _ecb_encrypt(PS3_GPKG_KEY if used == "finalized" else PSP_PKG_KEY, ctrs)
    # Debug/non-finalized: SHA-1 keystream over a 0x40-byte block built from the
    # 16-byte QA digest at header 0x60 as qa[0:8] qa[0:8] qa[8:16] qa[8:16], the
    # rest zero, with the block index in the last 8 bytes. (Matches RPCS3's
    # PKGLoader; hashing the raw 0x40 bytes at 0x60 instead does not decrypt.)
    qa = hdr60[0x00:0x10]
    out = bytearray()
    for i in range(nblocks):
        k = bytearray(0x40)
        k[0x00:0x08] = qa[0:8]
        k[0x08:0x10] = qa[0:8]
        k[0x10:0x18] = qa[8:16]
        k[0x18:0x20] = qa[8:16]
        k[0x38:0x40] = ((start_block + i) & ((1 << 64) - 1)).to_bytes(8, "big")
        out += hashlib.sha1(bytes(k)).digest()[:0x10]
    return bytes(out)


def out_path(outd, name):
    """Map a PKG entry name onto a path under `outd`, keeping its directories.

    Basenames are not unique: PDIPFS stores files as A/YG, B/YG, C/YG ..., so
    flattening silently overwrites most of the package. Entry names come from
    the (decrypted, but unauthenticated) file table, so components that would
    escape `outd` -- "..", a drive letter, a leading slash -- are dropped.
    """
    parts = []
    for p in name.replace("\\", "/").split("/"):
        p = p.strip()
        if not p or p in (".", "..") or os.path.splitdrive(p)[0]:
            continue
        parts.append(p)
    if not parts:
        raise ValueError(f"package entry has no usable name: {name!r}")
    return os.path.join(outd, *parts)


def main():
    inp, outd = sys.argv[1], sys.argv[2]
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]

    f = open(inp, "rb")
    hdr = f.read(0xA0)
    assert hdr[:4] == b"\x7fPKG", "not a PKG"
    rev = struct.unpack(">H", hdr[4:6])[0]
    item_count = struct.unpack(">I", hdr[0x14:0x18])[0]
    data_off = struct.unpack(">Q", hdr[0x20:0x28])[0]
    data_size = struct.unpack(">Q", hdr[0x28:0x30])[0]
    riv = hdr[0x70:0x80]; hdr60 = hdr[0x60:0xA0]

    def raw(off, n):
        f.seek(data_off + off)
        return f.read(n)

    def decrypt_range(used, start, length):
        sb = start // 16
        off = start - sb * 16
        nb = (off + length + 15) // 16
        chunk = raw(sb * 16, nb * 16)
        ks = _keystream(used, riv, hdr60, sb, min(nb, len(chunk) // 16 + 1))
        m = min(len(chunk), len(ks))
        a = int.from_bytes(chunk[:m], "big") ^ int.from_bytes(ks[:m], "big")
        dec = a.to_bytes(m, "big")
        return dec[off:off + length]

    def parse_table(used):
        hdr_len = min(data_size, item_count * 0x20 + (1 << 20))   # entries + name area
        dec = decrypt_range(used, 0, hdr_len)
        # A PSP/PS1-Classic package mixes both keys: the entry structs are PSP-keyed,
        # but each entry's *name and payload* pick their key from bit 28 of its flags
        # (set -> PSP key, clear -> PS3 key). A plain PS3 package never sets bit 28,
        # so the same rule leaves it on the PS3 key throughout.
        alt = {used: dec}
        def with_key(k, off, n):
            if k not in alt:
                alt[k] = decrypt_range(k, 0, hdr_len)
            return alt[k][off:off + n]
        names = []
        for i in range(item_count):
            e = dec[i * 0x20:(i + 1) * 0x20]
            if len(e) < 0x20:
                return None
            no, ns = struct.unpack(">II", e[:8])
            fo, fs = struct.unpack(">QQ", e[8:0x18])
            flags = struct.unpack(">I", e[0x18:0x1C])[0]
            k = used if used == "debug" else ("psp" if flags & 0x10000000 else "finalized")
            nm = with_key(k, no, ns)
            if not nm or any(c < 0x20 or c > 0x7E for c in nm):
                return None
            names.append((nm.decode(), fo, fs, k))
        return names

    used = None
    for cand in (("debug" if rev == 0 else "finalized"), "finalized", "psp", "debug"):
        names = parse_table(cand)
        if names:
            used = cand
            break
    assert used, "could not decrypt file table with either keystream"
    print(f"rev=0x{rev:04X} keystream={used} items={item_count}"
          f"{'  (pycryptodome)' if _HAVE_PYCRYPTO else '  (pure-python AES)'}")

    # --list: the file table alone. Decrypting a multi-GB package to find out what is
    # in it is a slow way to ask a cheap question.
    if "--list" in sys.argv:
        for nm, fo, fs, k in names:
            print(f"  {fs:>12}  {nm}  [{k}]")
        return

    os.makedirs(outd, exist_ok=True)
    for nm, fo, fs, k in names:
        if only and os.path.basename(nm) != only:
            continue
        if fs <= 0:
            continue
        blob = decrypt_range(k, fo, fs)
        op = out_path(outd, nm)
        os.makedirs(os.path.dirname(op), exist_ok=True)
        open(op, "wb").write(blob)
        tag = "  <-- SELF" if blob[:4] == b"SCE\x00" else ""
        print(f"  {nm}  ({fs} bytes){tag}")


if __name__ == "__main__":
    main()
