# coop-fw (2026-11-22, efficiency audit finding 14): the check that section GC changed no code.
#   python3 -I tools/plugin/same_code.py OLD/<id>.ro NEW/<id>.ro     (pack.py --keep leaves <id>.ro)
"""same_code.py A.ro B.ro: every function and object the two relocatable links share is the same bytes
with the same relocation targets (as symbol+offset, a string literal as its text); a call one side resolved at assembly
time (same section) is decoded and compared as its target. Lists what only one side has (the code GC dropped); exit 1 when a shared one differs.

Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
"""
import sys, struct
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pack as P
PCREL = (0x0a, 0x0d)

def load(path):
    e = P.Elf(open(path, "rb").read())
    secs = {i: s for i, s in enumerate(e.sh) if s["flags"] & P.SHF_ALLOC}
    named = {}
    for s in e.syms:
        if s["shndx"] in secs and s["type"] in (1, 2) and s["name"]:
            named.setdefault((s["name"], s["shndx"]), []).append(s)
    bysec = {}
    for (n, sh), l in named.items():
        for s in l:
            bysec.setdefault(sh, []).append((s["value"], s["size"], n))
    for l in bysec.values(): l.sort()
    rel = {}
    for rs in e.sh:
        if rs["type"] == P.SHT_RELA and rs["info"] in secs:
            for off, info, add in e.relas(rs):
                rel.setdefault(rs["info"], {})[off] = (info & 0xFF, e.syms[info >> 8], add)
    def at(sh, off):
        for v, sz, n in bysec.get(sh, []):
            if v <= off < v + max(sz, 1): return ("sym", n, off - v)
        data = e.contents(e.sh[sh])
        tab = rel.get(sh, {})
        if off in tab:                                  # an unnamed table of addresses (a switch): its targets
            words = []
            while off in tab and len(words) < 64:
                t, sym, a = tab[off]
                words.append(where(sym, a)); off += 4
            return ("table", e.sh[sh]["name"], tuple(words))
        if e.sh[sh]["type"] != P.SHT_NOBITS and off < len(data):
            end = data.find(b"\0", off)
            return ("bytes", e.sh[sh]["name"], data[off:end if end >= 0 else off + 16])
        return ("anon", e.sh[sh]["name"], off)
    def where(sym, add):
        if sym["shndx"] == P.SHN_UNDEF: return ("import", sym["name"], add)
        if sym["type"] != 3 and sym["name"]: return ("sym", sym["name"], add)
        return at(sym["shndx"], sym["value"] + add)
    out = {}
    for (n, sh), l in named.items():
        for k, s in enumerate(l):
            nob = e.sh[sh]["type"] == P.SHT_NOBITS
            data = b"\0" * s["size"] if nob else e.contents(e.sh[sh])[s["value"]:s["value"] + s["size"]]
            rr = {o - s["value"]: (t, sym, a) for o, (t, sym, a) in rel.get(sh, {}).items() if s["value"] <= o < s["value"] + s["size"]}
            out[(n, e.sh[sh]["name"], k)] = (data, rr, sh, s["value"])
    return out, where, at

def site(side, o, t):
    data, rr, sh, base = side[0]
    if o in rr:
        tt, sym, a = rr[o]
        return (tt, side[1](sym, a))
    if t in PCREL and o >= 2:                       # resolved by the assembler: decode it
        hw0, hw1 = struct.unpack_from("<HH", data, o - 2)
        d = ((hw0 & 0xFF) << 16) | hw1
        d -= (1 << 24) if d & 0x800000 else 0
        return (t, side[2](sh, base + o - 2 + 2 * d))
    return (t, ("none",))

def compare(x, y, wx, ax, wy, ay):
    sites = {o: t for o, (t, _, _) in x[1].items()}
    sites.update({o: t for o, (t, _, _) in y[1].items()})
    if len(x[0]) != len(y[0]): return "size"
    dx, dy = bytearray(x[0]), bytearray(y[0])
    for o, t in sites.items():
        tx, ty = site((x, wx, ax), o, t), site((y, wy, ay), o, t)
        tx = (0x0a if tx[0] in PCREL else tx[0], tx[1]); ty = (0x0a if ty[0] in PCREL else ty[0], ty[1])
        if tx != ty: return f"+{o:#x}: {tx} vs {ty}"
        for i in ((o - 2, o, o + 1) if t in PCREL else (o, o + 1) if t in (6, 7) else range(o, o + 4)):
            if 0 <= i < len(dx): dx[i] = dy[i] = 0
    if dx != dy:
        return "bytes at " + ", ".join(hex(i) for i in range(len(dx)) if dx[i] != dy[i])[:200]
    return None

(a, wa, aa), (b, wb, ab) = load(sys.argv[1]), load(sys.argv[2])
bad = 0
for k in sorted(set(a) & set(b)):
    r = compare(a[k], b[k], wa, aa, wb, ab)
    if r: bad += 1; print("DIFFERS", k, r)
for k in sorted(set(a) - set(b)): print("only A", k, len(a[k][0]))
for k in sorted(set(b) - set(a)): print("only B", k, len(b[k][0]))
print(f"{len(set(a) & set(b))} shared, {bad} differ")
sys.exit(1 if bad else 0)
