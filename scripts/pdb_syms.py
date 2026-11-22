# pdb_syms.py — 手动解析 PDB 公钥/全局符号流，RVA -> 函数名
# 用法: python pdb_syms.py <rva_hex> [rva_hex ...]
import struct
import bisect
import sys

PDB = r"D:\llm-rerank\rime-llm-ime\librime\build\bin\Release\rime.pdb"

pdb = open(PDB, "rb").read()
bs, fbb, nb, dirsz, unk, bmaddr = struct.unpack_from("<6I", pdb, 0x20)
n_dir_blocks = (dirsz + bs - 1) // bs
dir_blocks = struct.unpack_from("<%dI" % n_dir_blocks, pdb, bmaddr * bs)
dirdata = bytes().join(pdb[bk * bs:(bk + 1) * bs] for bk in dir_blocks)[:dirsz + bs]
n_streams = struct.unpack_from("<I", dirdata, 0)[0]
sizes = struct.unpack_from("<%dI" % n_streams, dirdata, 4)
off = 4 + 4 * n_streams
streams = []
for s in sizes:
    nb2 = (s + bs - 1) // bs
    blocks = struct.unpack_from("<%dI" % nb2, dirdata, off)
    off += 4 * nb2
    streams.append(bytes().join(pdb[bk * bs:(bk + 1) * bs] for bk in blocks)[:s])
print("n_streams =", n_streams)

dbi = streams[3]
symrec_idx = struct.unpack_from("<H", dbi, 12)[0]
public_idx = struct.unpack_from("<H", dbi, 20)[0]
print("dbi: symrec=%d public=%d" % (symrec_idx, public_idx))

LEAF = {0x110e: "S_PUB32", 0x1110: "S_GPROC32", 0x110f: "S_LPROC32",
        0x110d: "S_GDATA32", 0x110c: "S_LDATA32"}
symbols = []
for label, idx, skip in (("global", symrec_idx, 0), ("public", public_idx, 16)):
    data = streams[idx]
    pos = skip
    n = len(data)
    cnt = 0
    while pos + 4 <= n:
        reclen = struct.unpack_from("<H", data, pos)[0]
        if reclen < 2 or pos + 2 + reclen > n:
            pos += 4
            continue
        leaf = struct.unpack_from("<H", data, pos + 2)[0]
        payload = data[pos + 4: pos + 2 + reclen]
        rva = name = None
        if leaf in (0x1110, 0x110f):
            if len(payload) >= 22:
                rva = struct.unpack_from("<I", payload, 16)[0]
                ne = payload.find(b"\x00", 22)
                if ne > 0:
                    name = payload[22:ne]
        elif leaf in (0x110e, 0x110d, 0x110c):
            if len(payload) >= 8:
                rva = struct.unpack_from("<I", payload, 4)[0]
                ne = payload.find(b"\x00", 8)
                if ne > 0:
                    name = payload[8:ne]
        if rva and name:
            try:
                nm = name.decode("utf-8", "replace")
            except Exception:
                nm = "?"
            symbols.append((rva, LEAF.get(leaf, hex(leaf)), nm))
            cnt += 1
            pos = pos + 2 + reclen
            continue
        pos += 4
    print(label, "rva-symbols:", cnt)

symbols.sort()
rvas = [s[0] for s in symbols]
print("total =", len(symbols))
for arg in sys.argv[1:]:
    tgt = int(arg, 16)
    j = bisect.bisect_right(rvas, tgt) - 1
    if j >= 0:
        rva, leaf, name = symbols[j]
        print("rva=0x%x -> %s (%s) +0x%x" % (tgt, name, leaf, tgt - rva))
    else:
        print("rva=0x%x -> no earlier symbol" % tgt)
