"""Test meshes for the NIF string-index crash (NiStream's string reader, AE 70352): copies of a mesh whose root
block's name index points past the file's string table.
  py mknif.py <src nif> <out dir>   ->  <stem>_edge.nif (index = numStrings), <stem>_far.nif (index = 0x10000000)"""
import struct, sys
from pathlib import Path


def header_end(b):
    """-> (offset of block 0's data, numStrings); Skyrim SE layout (20.2.0.7, user 12, stream 100)."""
    p = b.index(b"\n") + 1
    ver, endian, user, nblocks = struct.unpack_from("<IBII", b, p); p += 13
    assert ver == 0x14020007 and user == 12, (hex(ver), user)
    bs, = struct.unpack_from("<I", b, p); p += 4
    for _ in range(3):                                  # author, process script, export script (byte-length strings)
        p += 1 + b[p]
    ntypes, = struct.unpack_from("<H", b, p); p += 2
    for _ in range(ntypes):
        n, = struct.unpack_from("<I", b, p); p += 4 + n
    p += 2 * nblocks + 4 * nblocks                      # block type index, block sizes
    nstr, _maxlen = struct.unpack_from("<II", b, p); p += 8
    for _ in range(nstr):
        n, = struct.unpack_from("<I", b, p); p += 4 + n
    ngroups, = struct.unpack_from("<I", b, p); p += 4 + 4 * ngroups
    return p, nstr


if __name__ == "__main__":
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    b = bytearray(src.read_bytes())
    at, nstr = header_end(b)
    old, = struct.unpack_from("<I", b, at)
    assert old < nstr, f"block 0 does not start with a string index: {old} of {nstr}"
    for tag, idx in (("edge", nstr), ("far", 0x10000000)):
        c = bytearray(b); struct.pack_into("<I", c, at, idx)
        f = out / f"{src.stem}_{tag}.nif"; f.write_bytes(c)
        print(f"{f}: root name index {old} -> {idx} (file has {nstr} strings)")
