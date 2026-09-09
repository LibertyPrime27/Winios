#!/usr/bin/env python3
"""7z fixtures for tests/test_un7z.c, written by hand against the format
specification with liblzma producing the compressed streams -- so the decoder
in tools/import/un7z.c is checked against an independent encoder, byte for
byte. Deterministic: liblzma's output is a function of its input and settings.

    python3 tests/import/make_7z_fixtures.py

What each one is for:
  copy.7z            the container alone: names, a directory, an empty file, CRCs
  lzma1.7z           LZMA1, solid: three files in one folder
  lzma2_bcj.7z       hello64.exe through the x86 branch filter, then LZMA2
  delta_lzma2.7z     a Delta filter in front of LZMA2
  encoded_header.7z  the header itself LZMA-compressed
  sfx_lzma1.exe      lzma1.7z appended to a (fake) self-extractor stub
"""
import lzma, os, struct, zlib
HERE = os.path.dirname(os.path.abspath(__file__))

def num(v):
    """7z's variable-length number."""
    if v < 0x80: return bytes([v])
    for n in range(1, 9):
        if v < (1 << (7 * (n + 1))) or n == 8:
            first_bits = 8 - n - 1
            if n == 8: return bytes([0xFF]) + struct.pack("<Q", v)
            hi = v >> (8 * n)
            first = (0xFF << (8 - n)) & 0xFF | hi
            return bytes([first]) + v.to_bytes(n, "little")[:n] if False else bytes([first]) + (v & ((1 << (8 * n)) - 1)).to_bytes(n, "little")
    raise ValueError
def bits(flags):
    out = bytearray(); b = 0; m = 0x80
    for f in flags:
        if f: b |= m
        m >>= 1
        if not m: out.append(b); b = 0; m = 0x80
    if m != 0x80: out.append(b)
    return bytes(out)

def lzma1_props(dict_size, lc=3, lp=0, pb=2):
    return bytes([(pb * 5 + lp) * 9 + lc]) + struct.pack("<I", dict_size)
def lzma2_prop(dict_size):
    for b in range(41):
        d = 0xFFFFFFFF if b == 40 else (2 | (b & 1)) << (b // 2 + 11)
        if d >= dict_size: return bytes([b])
    return bytes([40])
def raw(filters, data):
    c = lzma.LZMACompressor(format=lzma.FORMAT_RAW, filters=filters)
    return c.compress(data) + c.flush()

class Coder:
    def __init__(self, cid, props=b"", nin=1, nout=1): self.id, self.props, self.nin, self.nout = cid, props, nin, nout
def folder_bytes(coders, binds, packed):
    out = num(len(coders))
    for c in coders:
        flags = len(c.id) | (0x10 if (c.nin, c.nout) != (1, 1) else 0) | (0x20 if c.props else 0)
        out += bytes([flags]) + c.id
        if flags & 0x10: out += num(c.nin) + num(c.nout)
        if c.props: out += num(len(c.props)) + c.props
    for i, o in binds: out += num(i) + num(o)
    if len(packed) > 1:
        for p in packed: out += num(p)
    return out

def streams_info(pack_pos, pack_sizes, folders, unpack_sizes, folder_crcs, sub_sizes, sub_crcs):
    """folders: list of folder_bytes; unpack_sizes: per folder list; sub_sizes: per folder list of sizes (None = one stream)."""
    s = bytes([6]) + num(pack_pos) + num(len(pack_sizes)) + bytes([9]) + b"".join(num(x) for x in pack_sizes) + bytes([0])
    s += bytes([7, 11]) + num(len(folders)) + bytes([0]) + b"".join(folders)
    s += bytes([12]) + b"".join(num(x) for fl in unpack_sizes for x in fl)
    if folder_crcs: s += bytes([10, 1]) + b"".join(struct.pack("<I", c) for c in folder_crcs)
    s += bytes([0])
    if sub_sizes is not None:
        s += bytes([8, 13]) + b"".join(num(len(x)) for x in sub_sizes)
        s += bytes([9]) + b"".join(num(sz) for x in sub_sizes for sz in x[:-1])
        s += bytes([10, 1]) + b"".join(struct.pack("<I", c) for c in sub_crcs) + bytes([0])
    return s + bytes([0])
def files_info(entries):
    """entries: (name, kind, data) kind in file/dir/empty."""
    s = num(len(entries))
    empty = [k != "file" for _, k, _ in entries]
    if any(empty):
        v = bits(empty); s += bytes([14]) + num(len(v)) + v
        ef = bits([k == "empty" for _, k, _ in entries if k != "file"]); s += bytes([15]) + num(len(ef)) + ef
    names = b"\x00" + b"".join(n.encode("utf-16-le") + b"\x00\x00" for n, _, _ in entries)
    s += bytes([17]) + num(len(names)) + names
    attrs = b"\x01\x00" + b"".join(struct.pack("<I", 0x10 if k == "dir" else 0x20) for _, k, _ in entries)
    s += bytes([21]) + num(len(attrs)) + attrs
    return s + bytes([0])
def archive(pack, header):
    start = struct.pack("<QQI", len(pack), len(header), zlib.crc32(header))
    return b"7z\xbc\xaf\x27\x1c" + b"\x00\x04" + struct.pack("<I", zlib.crc32(start)) + start + pack + header

def pattern(n, k): return bytes(((i * k + (i >> 8)) & 0xFF) for i in range(n))
FILES = [("readme.txt", "file", b"hello from a 7z archive\n"), ("data/level.bin", "file", pattern(70000, 7)), ("data/notes", "dir", b""),
         ("data/empty.txt", "empty", b""), ("music/theme.raw", "file", pattern(300000, 3))]
def solid_payload(entries): return b"".join(d for _, k, d in entries if k == "file")

def make_copy():
    payload = solid_payload(FILES)
    fb = folder_bytes([Coder(b"\x00")], [], [0])
    files = [d for _, k, d in FILES if k == "file"]
    hdr = bytes([1, 4]) + streams_info(0, [len(payload)], [fb], [[len(payload)]], None, [[len(d) for d in files]], [zlib.crc32(d) for d in files]) + bytes([5]) + files_info(FILES) + bytes([0])
    return archive(payload, hdr)
def make_lzma1(header_encoded=False):
    payload = solid_payload(FILES)
    dict_size = 1 << 20
    packed = raw([{"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": 3, "lp": 0, "pb": 2}], payload)
    fb = folder_bytes([Coder(b"\x03\x01\x01", lzma1_props(dict_size))], [], [0])
    files = [d for _, k, d in FILES if k == "file"]
    hdr = bytes([1, 4]) + streams_info(0, [len(packed)], [fb], [[len(payload)]], None, [[len(d) for d in files]], [zlib.crc32(d) for d in files]) + bytes([5]) + files_info(FILES) + bytes([0])
    if not header_encoded: return archive(packed, hdr)
    hpacked = raw([{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 16, "lc": 3, "lp": 0, "pb": 2}], hdr)
    hfb = folder_bytes([Coder(b"\x03\x01\x01", lzma1_props(1 << 16))], [], [0])
    enc = bytes([23]) + streams_info(len(packed), [len(hpacked)], [hfb], [[len(hdr)]], [zlib.crc32(hdr)], None, None)
    return archive(packed + hpacked, enc)
def make_bcj_lzma2():
    exe = open(os.path.join(HERE, "..", "win32", "hello64.exe"), "rb").read()
    dict_size = 1 << 23
    packed = raw([{"id": lzma.FILTER_X86}, {"id": lzma.FILTER_LZMA2, "dict_size": dict_size}], exe)
    # coder 0: BCJ (in 0 -> out 0, the folder's output); coder 1: LZMA2 (in 1 <- packed, out 1 -> bound to in 0)
    fb = folder_bytes([Coder(b"\x03\x03\x01\x03"), Coder(b"\x21", lzma2_prop(dict_size))], [(0, 1)], [1])
    entries = [("hello64.exe", "file", exe)]
    hdr = bytes([1, 4]) + streams_info(0, [len(packed)], [fb], [[len(exe), len(exe)]], None, [[len(exe)]], [zlib.crc32(exe)]) + bytes([5]) + files_info(entries) + bytes([0])
    return archive(packed, hdr)
def make_delta_lzma2():
    data = bytes(((i // 3) & 0xFF) for i in range(200000))          # ramps: what Delta is for
    dict_size = 1 << 22
    packed = raw([{"id": lzma.FILTER_DELTA, "dist": 1}, {"id": lzma.FILTER_LZMA2, "dict_size": dict_size}], data)
    fb = folder_bytes([Coder(b"\x03", bytes([0])), Coder(b"\x21", lzma2_prop(dict_size))], [(0, 1)], [1])
    entries = [("ramp.bin", "file", data)]
    hdr = bytes([1, 4]) + streams_info(0, [len(packed)], [fb], [[len(data), len(data)]], None, [[len(data)]], [zlib.crc32(data)]) + bytes([5]) + files_info(entries) + bytes([0])
    return archive(packed, hdr)

def main():
    out = {"copy.7z": make_copy(), "lzma1.7z": make_lzma1(), "encoded_header.7z": make_lzma1(True),
           "lzma2_bcj.7z": make_bcj_lzma2(), "delta_lzma2.7z": make_delta_lzma2()}
    stub = open(os.path.join(HERE, "fake_7zsfx.exe"), "rb").read()
    out["sfx_lzma1.exe"] = stub + out["lzma1.7z"]
    for name, data in out.items():
        with open(os.path.join(HERE, name), "wb") as f: f.write(data)
        print(f"{name}: {len(data)} bytes")
if __name__ == "__main__": main()
