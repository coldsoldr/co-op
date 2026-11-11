#!/usr/bin/env python3
"""Pre-flash safety gate for OP-1 firmware packages.

Both update paths (te-boot menu and the running app) only touch the bootloader
if the package contains te-boot.ldr. Writing it erases and reprograms the NAND
boot blocks (not atomic, and the app path forces the write even when the
version is unchanged), so custom packages must NOT contain te-boot.ldr at all.
See docs/brick-safety.md.

Fails (exit code 1) if:
  - te-boot.ldr is present (unless --allow-stock-bootloader and it is a known
    stock file, for checking TE's own packages)
  - the file name contains FRC (te-boot forces a bootloader rewrite) or is a
    .KEY file (te-boot runs signature key programming)
  - the outer CRC-32 is wrong, the package is over the size limit, or
    OP1_vdk.ldr is missing, not a valid LDR stream, or too big for NAND blocks 4-127

Usage:
    python tools/verify_bootloader.py [--allow-stock-bootloader] PATH [PATH ...]   # .op1 file or unpacked dir
"""

import argparse
import binascii
import hashlib
import io
import lzma
import os
import struct
import sys
import tarfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ldr_dump import LdrError, parse  # noqa: E402

# SHA-256 of known stock te-boot.ldr files
KNOWN_BOOTLOADERS = {
    'f6233b974bdb6c92f39e73401f206721924ef3ce52abd66b642a5350cebcad93': 'TE-BOOT v2.30 (stock op1_246)',
}

# op1repacker notes the OP-1 only accepts firmware files up to ~15 MB
MAX_PACKAGE_SIZE = 15 * 1024 * 1024

# OP1_vdk.ldr is written raw from NAND block 4 up to the end of the 128-block boot area (128 KB blocks)
MAX_APP_SIZE = (128 - 4) * 128 * 1024


def read_package(path):
    """Decode an .op1 file in memory. Returns ({name: bytes or None} for the .ldr files, list of problems)."""
    problems = []
    with open(path, 'rb') as f:
        data = f.read()

    if len(data) > MAX_PACKAGE_SIZE:
        problems.append('package is {} bytes, over the {} byte limit'.format(len(data), MAX_PACKAGE_SIZE))

    stored_crc = struct.unpack('<L', data[:4])[0]
    calced_crc = binascii.crc32(data[4:])
    if stored_crc != calced_crc:
        problems.append('outer CRC-32 mismatch (stored {:#010x}, calculated {:#010x})'.format(stored_crc, calced_crc))

    files = {'te-boot.ldr': None, 'OP1_vdk.ldr': None}
    try:
        raw = lzma.decompress(data[4:], format=lzma.FORMAT_ALONE)
        with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
            names = tar.getnames()
            for name in files:
                files[name] = tar.extractfile(name).read() if name in names else None
    except (lzma.LZMAError, tarfile.TarError, EOFError) as e:
        problems.append('package does not decode: {}'.format(e))
        return None, problems
    return files, problems


def read_directory(path):
    files = {}
    for name in ('te-boot.ldr', 'OP1_vdk.ldr'):
        file_path = os.path.join(path, name)
        if os.path.isfile(file_path):
            with open(file_path, 'rb') as f:
                files[name] = f.read()
        else:
            files[name] = None
    return files, []


def check_file_name(path):
    """te-boot looks for these markers in file names on the USB disk (strstr, case-sensitive; checked here case-insensitively)."""
    name = os.path.basename(path).upper()
    problems = []
    if 'FRC' in name:
        problems.append('file name contains "FRC", which makes te-boot force a bootloader rewrite')
    if name.endswith('.KEY'):
        problems.append('.KEY files make te-boot run signature key programming')
    return problems


def verify(path, allow_stock_bootloader):
    if os.path.isdir(path):
        files, problems = read_directory(path)
    else:
        files, problems = read_package(path)
        problems += check_file_name(path)
    if files is None:
        return problems

    boot = files['te-boot.ldr']
    if boot is None:
        print('  te-boot.ldr: absent (bootloader will not be touched)')
    else:
        digest = hashlib.sha256(boot).hexdigest()
        if digest not in KNOWN_BOOTLOADERS:
            problems.append('te-boot.ldr is present and NOT a known stock bootloader (sha256 {})'.format(digest))
        elif allow_stock_bootloader:
            print('  te-boot.ldr: {} [{}...]'.format(KNOWN_BOOTLOADERS[digest], digest[:16]))
            print('  WARNING: install only via the te-boot menu (COM at power-on); the app path rewrites te-boot')
        else:
            problems.append('te-boot.ldr is present ({}); custom packages must omit it'.format(
                KNOWN_BOOTLOADERS[digest]))

    vdk = files['OP1_vdk.ldr']
    if vdk is None:
        problems.append('OP1_vdk.ldr is missing')
    else:
        if len(vdk) > MAX_APP_SIZE:
            problems.append('OP1_vdk.ldr is {} bytes, over the {} byte NAND boot area'.format(len(vdk), MAX_APP_SIZE))
        try:
            dxes = parse(vdk)
            print('  OP1_vdk.ldr: valid LDR, {} DXE(s), {} blocks'.format(len(dxes), sum(len(d) for d in dxes)))
        except LdrError as e:
            problems.append('OP1_vdk.ldr is not a valid LDR stream: {}'.format(e))

    return problems


def main():
    parser = argparse.ArgumentParser(description='Pre-flash safety gate for OP-1 firmware packages.')
    parser.add_argument('paths', nargs='+', help='.op1 file or unpacked firmware directory')
    parser.add_argument('--allow-stock-bootloader', action='store_true',
                        help='accept a byte-identical stock te-boot.ldr (for checking TE packages)')
    args = parser.parse_args()

    failed = False
    for path in args.paths:
        print('Checking {}'.format(path))
        problems = verify(path, args.allow_stock_bootloader)
        for problem in problems:
            print('  FAIL: ' + problem)
        print('  => {}\n'.format('FAIL - DO NOT FLASH' if problems else 'PASS'))
        failed = failed or bool(problems)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
