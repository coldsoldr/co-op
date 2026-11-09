#!/usr/bin/env python3
"""Build a flashable .op1 from an unpacked firmware directory, without te-boot.ldr.

Leaving te-boot.ldr out means neither update path touches the bootloader (see
docs/brick-safety.md). Packing reuses op1repacker's tar/LZMA/CRC code, and the
result must pass tools/verify_bootloader.py.

Usage:
    python tools/build_package.py UNPACKED_DIR OUTPUT.op1
"""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'third_party', 'op1repacker'))
from op1repacker.op1_repack import OP1Repack  # noqa: E402

EXCLUDED = ('te-boot.ldr',)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    source, output = sys.argv[1], os.path.abspath(sys.argv[2])

    if not os.path.isfile(os.path.join(source, 'OP1_vdk.ldr')):
        print('ERROR: {} has no OP1_vdk.ldr'.format(source))
        sys.exit(1)
    if os.path.exists(output):
        print('ERROR: {} already exists'.format(output))
        sys.exit(1)

    repacker = OP1Repack()
    with tempfile.TemporaryDirectory() as temp:
        stage = os.path.join(temp, 'fw')
        shutil.copytree(source, stage, ignore=lambda d, names: [n for n in names if d == source and n in EXCLUDED])
        print('Packing {} (excluding {})...'.format(source, ', '.join(EXCLUDED)), flush=True)
        repacker.compress_tar(stage, output)
        repacker.compress_lzma(output)
        repacker.add_crc(output)

    print('Wrote {} ({} bytes)\n'.format(output, os.path.getsize(output)), flush=True)
    verify = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'verify_bootloader.py')
    result = subprocess.run([sys.executable, verify, output])
    if result.returncode != 0:
        os.remove(output)
        print('Removed {} because it failed verification.'.format(output))
    sys.exit(result.returncode)


if __name__ == '__main__':
    main()
