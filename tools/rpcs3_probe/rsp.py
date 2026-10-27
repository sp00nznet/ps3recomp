"""rsp.py - Minimal GDB Remote Serial Protocol client for RPCS3's GDB stub.

Dependency-free (raw sockets). Speaks just enough RSP to drive RPCS3's PPU GDB
server: handshake, register-layout discovery via qXfer:features (target.xml),
read registers/memory, software breakpoints, continue/step, and async interrupt.

RPCS3 PPU state is big-endian; the `g` register block and `m` memory reads come
back as ASCII hex. Guest effective addresses are 32-bit (low word of each 64-bit
register/stack slot).

This is title-agnostic: nothing here knows about any specific game.
"""
import socket
import time
import xml.etree.ElementTree as ET


class RSPError(Exception):
    pass


def _checksum(payload: bytes) -> int:
    return sum(payload) & 0xFF


def _unescape(data: bytes) -> bytes:
    """Undo RSP run-length encoding (`x*<n>`) and `}` binary escaping."""
    out = bytearray()
    i = 0
    while i < len(data):
        c = data[i]
        if c == 0x7D:  # '}' escape: next byte XOR 0x20
            out.append(data[i + 1] ^ 0x20)
            i += 2
        elif i + 1 < len(data) and data[i + 1] == 0x2A:  # '*' RLE
            count = data[i + 2] - 29          # additional repeats
            out.append(c)
            out.extend([c] * count)
            i += 3
        else:
            out.append(c)
            i += 1
    return bytes(out)


class RSPClient:
    def __init__(self, host="127.0.0.1", port=2345, timeout=10.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self.buf = b""
        self.no_ack = False
        self.reg_layout = []   # ordered list of (name, byte_offset, byte_size)
        self.reg_by_name = {}  # name -> (byte_offset, byte_size)

    # ---- connection ---------------------------------------------------------
    def connect(self, retries=40, delay=0.5):
        last = None
        for _ in range(retries):
            try:
                s = socket.create_connection((self.host, self.port), timeout=self.timeout)
                s.settimeout(self.timeout)
                self.sock = s
                return True
            except OSError as e:
                last = e
                time.sleep(delay)
        raise RSPError(f"could not connect to {self.host}:{self.port}: {last}")

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            finally:
                self.sock = None

    # ---- low-level packet I/O ----------------------------------------------
    def _read_raw(self, n=4096) -> bytes:
        try:
            data = self.sock.recv(n)
        except socket.timeout:
            return b""
        if data == b"":
            raise RSPError("connection closed by RPCS3")
        return data

    def _send_packet(self, payload: str):
        body = payload.encode("latin-1")
        pkt = b"$" + body + b"#" + f"{_checksum(body):02x}".encode()
        self.sock.sendall(pkt)
        if not self.no_ack:
            self._wait_ack()

    def _wait_ack(self, deadline=None):
        deadline = deadline or (time.time() + self.timeout)
        while time.time() < deadline:
            while not self.buf:
                self.buf += self._read_raw()
                if not self.buf:
                    continue
            ch = self.buf[:1]
            self.buf = self.buf[1:]
            if ch == b"+":
                return
            if ch == b"-":
                raise RSPError("packet NAK")
            # stray data before ack; push back and bail
            self.buf = ch + self.buf
            return
        raise RSPError("timeout waiting for ack")

    def _recv_packet(self, deadline=None) -> bytes:
        """Read one $...#cc packet, send ack, return the unescaped payload."""
        deadline = deadline or (time.time() + self.timeout)
        # find start
        while time.time() < deadline:
            start = self.buf.find(b"$")
            if start >= 0:
                self.buf = self.buf[start:]
                break
            self.buf += self._read_raw()
        else:
            raise RSPError("timeout waiting for packet start")
        # find end '#' + 2 checksum chars
        while True:
            hi = self.buf.find(b"#")
            if hi >= 0 and len(self.buf) >= hi + 3:
                break
            if time.time() > deadline:
                raise RSPError("timeout waiting for packet end")
            self.buf += self._read_raw()
        payload = self.buf[1:hi]
        self.buf = self.buf[hi + 3:]
        if not self.no_ack:
            self.sock.sendall(b"+")
        return _unescape(payload)

    def interrupt(self):
        """Async interrupt (Ctrl-C) to halt a running target."""
        self.sock.sendall(b"\x03")

    # ---- handshake / capabilities ------------------------------------------
    def handshake(self):
        self._send_packet("qSupported:multiprocess+;xmlRegisters=powerpc;qXfer:features:read+")
        reply = self._recv_packet().decode("latin-1", "replace")
        self.features = reply
        if "QStartNoAckMode+" in reply:
            self._send_packet("QStartNoAckMode")
            self._recv_packet()  # OK
            self.no_ack = True
        # discover register layout
        try:
            self._load_registers()
        except Exception as e:
            # fall back to standard PPC layout (32 GPR x8, then pc/msr/cr/lr/ctr/xer)
            self._fallback_registers()
            self._reg_err = str(e)
        return reply

    def qxfer_read(self, obj: str, annex: str = "") -> bytes:
        out = bytearray()
        off = 0
        while True:
            self._send_packet(f"qXfer:{obj}:read:{annex}:{off:x},1000")
            r = self._recv_packet()
            if not r:
                break
            kind, body = r[:1], r[1:]
            out += body
            if kind == b"l":
                break
            if kind != b"m":
                break
            off += len(body)
        return bytes(out)

    def _load_registers(self):
        xml = self.qxfer_read("features", "target.xml").decode("latin-1", "replace")
        # target.xml may <xi:include> a core file; follow one level
        regs = self._parse_reg_xml(xml)
        if not regs:
            for annex in self._included_annexes(xml):
                regs = self._parse_reg_xml(
                    self.qxfer_read("features", annex).decode("latin-1", "replace"))
                if regs:
                    break
        if not regs:
            raise RSPError("no <reg> entries in target description")
        off = 0
        for name, bits in regs:
            size = bits // 8
            self.reg_layout.append((name, off, size))
            self.reg_by_name[name] = (off, size)
            off += size

    @staticmethod
    def _included_annexes(xml):
        annexes = []
        for m in ET.fromstring(xml).iter():
            if m.tag.endswith("include") and m.get("href"):
                annexes.append(m.get("href"))
        return annexes

    @staticmethod
    def _parse_reg_xml(xml):
        regs = []
        try:
            root = ET.fromstring(xml)
        except ET.ParseError:
            return regs
        for el in root.iter():
            if el.tag.endswith("reg"):
                name = el.get("name")
                bits = int(el.get("bitsize", "64"))
                if name:
                    regs.append((name, bits))
        return regs

    def _fallback_registers(self):
        off = 0
        names = [f"r{i}" for i in range(32)] + [f"f{i}" for i in range(32)] + \
                ["pc", "msr", "cr", "lr", "ctr", "xer", "fpscr"]
        for name in names:
            self.reg_layout.append((name, off, 8))
            self.reg_by_name[name] = (off, 8)
            off += 8

    # ---- state access -------------------------------------------------------
    def read_registers(self) -> dict:
        self._send_packet("g")
        hexdata = self._recv_packet().decode("latin-1")
        # GDB marks unavailable register bytes with 'x'/'X'; map to 00 so the
        # fixed-width layout stays aligned (those regs read back as 0).
        unavail = set()
        if "x" in hexdata or "X" in hexdata:
            clean = []
            for i, ch in enumerate(hexdata):
                if ch in "xX":
                    clean.append("0")
                    unavail.add(i // 2)
                else:
                    clean.append(ch)
            hexdata = "".join(clean)
        raw = bytes.fromhex(hexdata)
        out = {}
        for name, off, size in self.reg_layout:
            chunk = raw[off:off + size]
            if len(chunk) == size and not any((off + k) in unavail for k in range(size)):
                out[name] = int.from_bytes(chunk, "big")  # PPU big-endian
        return out

    def reg(self, regs: dict, *names):
        for n in names:
            if n in regs:
                return regs[n]
        raise KeyError(f"register not found: {names}")

    def read_mem(self, addr: int, length: int) -> bytes:
        out = bytearray()
        while length > 0:
            chunk = min(length, 512)
            self._send_packet(f"m{addr & 0xFFFFFFFF:x},{chunk:x}")
            r = self._recv_packet()
            if r[:1] == b"E":
                raise RSPError(f"memory read error @0x{addr:08x}: {r.decode('latin-1')}")
            out += bytes.fromhex(r.decode("latin-1"))
            addr += chunk
            length -= chunk
        return bytes(out)

    def read_u32(self, addr: int) -> int:
        return int.from_bytes(self.read_mem(addr, 4), "big")

    def read_u64(self, addr: int) -> int:
        return int.from_bytes(self.read_mem(addr, 8), "big")

    # ---- execution control --------------------------------------------------
    def add_breakpoint(self, addr: int, kind=4):
        self._send_packet(f"Z0,{addr & 0xFFFFFFFF:x},{kind}")
        return self._recv_packet() == b"OK"

    def remove_breakpoint(self, addr: int, kind=4):
        self._send_packet(f"z0,{addr & 0xFFFFFFFF:x},{kind}")
        return self._recv_packet() == b"OK"

    def cont(self):
        self._send_packet("c")

    def step(self):
        self._send_packet("s")

    def wait_stop(self, deadline=None):
        """Return the stop-reply payload (e.g. b'T05...') after c/s/interrupt.

        Skips empty/ack/notification packets until a real stop reply
        (S/T = signalled, W/X = exited/terminated, O = console output)."""
        deadline = deadline or (time.time() + self.timeout)
        last = b""
        while time.time() < deadline:
            try:
                pkt = self._recv_packet(deadline=deadline)
            except RSPError:
                break
            if pkt[:1] in (b"S", b"T", b"W", b"X"):
                return pkt
            if pkt[:1] == b"O":   # console output; keep waiting
                continue
            last = pkt or last
        return last
