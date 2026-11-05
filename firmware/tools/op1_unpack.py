#!/usr/bin/env python3
"""Unpack a firmware package (.op1: CRC, LZMA, tar) into OUT/fw, with op1repacker's code.

    python3 -I tools/op1_unpack.py PACKAGE.op1 OUT_DIR        # prints a short sha256 and the path of every file

Tar members that would land outside OUT/fw, links and devices are refused (packages from the internet are untrusted
data). Used by tools/op1emu_gui.sh; stdlib + third_party/op1repacker.
"""
import sys, os, shutil, tarfile, hashlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'third_party', 'op1repacker'))
from op1repacker.op1_repack import OP1Repack

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
tmp = os.path.join(out, 'pkg.tmp')
shutil.copy(src, tmp)
r = OP1Repack()
r.remove_crc(tmp)
r.uncompress_lzma(tmp, tmp)
root = os.path.join(out, 'fw')
os.makedirs(root, exist_ok=True)
base = os.path.realpath(root)
with tarfile.open(tmp) as t:
    for m in t.getmembers():
        p = os.path.realpath(os.path.join(root, m.name))
        if p != base and not p.startswith(base + os.sep):
            sys.exit('unsafe member ' + m.name)
        if m.issym() or m.islnk() or m.isdev():
            sys.exit('link or device member ' + m.name)
    t.extractall(root)
os.remove(tmp)
for dp, dn, fn in sorted(os.walk(root)):
    for f in sorted(fn):
        p = os.path.join(dp, f)
        print('%s %s' % (hashlib.sha256(open(p, 'rb').read()).hexdigest()[:16], os.path.relpath(p, root)))
