"""pkg_check.py PKG.op1 WORKDIR: unpack the package's tar and compare every
file with the work dir and with stock op1_246 (by content)."""
import hashlib, io, lzma, os, sys, tarfile

pkg, work = sys.argv[1], sys.argv[2]
stock = os.path.join(os.path.dirname(work), 'op1_246')
data = open(pkg, 'rb').read()
# op1repacker: CRC prepended (4 bytes), then LZMA-alone.
raw = None
for skip in (4, 0):
    try:
        raw = lzma.decompress(data[skip:], format=lzma.FORMAT_ALONE)
        break
    except Exception:
        pass
assert raw, 'cannot decompress'
t = tarfile.open(fileobj=io.BytesIO(raw))
h = lambda b: hashlib.sha1(b).hexdigest()
bad = 0
names = set()
for m in t.getmembers():
    if not m.isfile():
        continue
    rel = m.name[2:] if m.name.startswith('./') else m.name
    names.add(rel)
    b = t.extractfile(m).read()
    wp = os.path.join(work, rel)
    if not os.path.exists(wp) or h(open(wp, 'rb').read()) != h(b):
        print('DIFF from work dir:', rel); bad += 1
for root, _, files in os.walk(work):
    for f in files:
        rel = os.path.relpath(os.path.join(root, f), work).replace('\\', '/')
        if rel not in names and rel != 'te-boot.ldr':
            print('MISSING in package:', rel); bad += 1
print(pkg, 'files', len(names), 'mismatches', bad)
