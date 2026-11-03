#!/usr/bin/env python3
"""Build an op1emu NAND image from the stock .op1 package plus an overlay directory (e.g. plugins/ for coop plugins).

    python3 -I tools/make_nand.py --provision ~/op1emu-work/provision_nand.py --firmware ~/Downloads/op1_246.op1 \
        --mkyaffs2 ~/op1emu-work/yaffs2utils/mkyaffs2 --overlay build/nand_overlay --output ~/op1emu-work/plug.bin

The overlay's tree lands at the yaffs root (overlay/plugins/x.coop -> /yaffs2/plugins/x.coop). Uses the functions of
op1emu's provision_nand.py (tools/setup_op1emu.sh fetches it); the stock package is never copied anywhere else.
"""
import argparse, importlib.util, shutil, sys, tempfile
from pathlib import Path

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--provision", required=True, help="path of op1emu's provision_nand.py")
    ap.add_argument("--firmware", required=True, type=Path)
    ap.add_argument("--mkyaffs2", required=True)
    ap.add_argument("--overlay", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    a = ap.parse_args()
    spec = importlib.util.spec_from_file_location("provision_nand", a.provision)
    pn = importlib.util.module_from_spec(spec); spec.loader.exec_module(pn)
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "root"; root.mkdir()
        pn.unpack_op1(a.firmware, root)
        for src in a.overlay.rglob("*"):
            if src.is_file():
                dst = root / src.relative_to(a.overlay)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, dst)
        interleaved = Path(tmp) / "yaffs.img"
        pn.build_yaffs(root, interleaved, a.mkyaffs2)
        if a.output.exists(): a.output.unlink()
        n = pn.provision(interleaved, a.output)
    print(f"{a.output}: {n} pages, overlay {a.overlay}")

if __name__ == "__main__":
    main()
