#!/usr/bin/env python3
"""Read one file out of an op1emu NAND image (yaffs2, op1emu's split data/OOB layout) after a run.

    python3 -I tools/nand_file.py ~/op1emu-work/run_nand.bin coop_run.txt          # prints it
    python3 -I tools/nand_file.py NAND coop_stats.txt -o stats.txt

Layout (op1emu provision_nand.py): 4096 blocks x 64 pages of 2048 bytes, then the 64-byte OOB of every page; the yaffs2
packed tags sit at OOB + 4: sequence, object id, chunk id, byte count (u32 LE). A chunk id with bit 31 set is an
object header (the object id's top nibble then carries the type); its data holds the name at +10 and the file size
at +292 (yaffs_obj_hdr). Data chunk n (n >= 1) holds bytes (n - 1) * 2048 onwards. For every (object, chunk) the
highest sequence wins; a name that several objects carried (a file deleted and made again) resolves to the object
whose header was written last.
"""
import argparse, struct, sys

PAGE, OOB, PAGES = 2048, 64, 4096 * 64
TAG_OFF = 4


def scan(path):
    with open(path, 'rb') as f:
        f.seek(PAGE * PAGES)
        oob = f.read(OOB * PAGES)
        headers, chunks = {}, {}
        for p in range(PAGES):
            seq, oid, cid, nbytes = struct.unpack_from('<IIII', oob, p * OOB + TAG_OFF)
            if seq == 0xFFFFFFFF or seq == 0:
                continue
            if cid & 0x80000000:
                obj = oid & 0x0FFFFFFF
                f.seek(p * PAGE)
                hdr = f.read(PAGE)
                key = (obj, 0)
                if key not in headers or headers[key][0] <= seq:
                    headers[key] = (seq, hdr)
            else:
                key = (oid & 0x0FFFFFFF, cid)
                if cid == 0:
                    f.seek(p * PAGE)
                    hdr = f.read(PAGE)
                    if (key not in headers) or headers[key][0] <= seq:
                        headers[key] = (seq, hdr)
                    continue
                if key not in chunks or chunks[key][0] <= seq:
                    chunks[key] = (seq, p, nbytes)
        return headers, chunks


def read_file(path, name):
    headers, chunks = scan(path)
    best = None
    for (obj, _), (seq, hdr) in headers.items():
        nm = hdr[10:10 + 256].split(b'\0', 1)[0].decode('latin-1')
        if nm == name and (best is None or seq > best[0]):
            best = (seq, obj, hdr)
    if best is None:
        return None
    _, obj, hdr = best
    size = struct.unpack_from('<I', hdr, 292)[0]
    out = bytearray()
    with open(path, 'rb') as f:
        n = 1
        while len(out) < size:
            c = chunks.get((obj, n))
            if c is None:
                break
            f.seek(c[1] * PAGE)
            out += f.read(c[2])
            n += 1
    return bytes(out[:size])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('nand')
    ap.add_argument('name', help='the file name (no directory): coop_run.txt, coop_stats.txt')
    ap.add_argument('-o', '--out')
    a = ap.parse_args()
    data = read_file(a.nand, a.name)
    if data is None:
        sys.exit('%s: no file named %s' % (a.nand, a.name))
    if a.out:
        open(a.out, 'wb').write(data)
    else:
        sys.stdout.write(data.decode('latin-1'))


if __name__ == '__main__':
    main()
