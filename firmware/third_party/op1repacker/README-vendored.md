# Vendored op1repacker 0.2.6

op1hacks' OP-1 firmware repacker, copied unchanged from the GitHub master branch
(https://github.com/op1hacks/op1repacker) at version 0.2.6. Licence: MIT (`LICENSE`).
Only its packaging files (`setup.py`, `setup.cfg`, `release.sh`, its `.gitignore`) were
dropped; nothing in `op1repacker/` is modified.

Who uses it:

| Path | Use |
|---|---|
| `op1repacker/op1_repack.py` | `tools/build_package.py` unpacks and repacks firmware with `OP1Repack`, dropping `te-boot.ldr` (brick-safety rule 1). op1repacker's own `repack` command keeps the bootloader file: never use it |
| `op1repacker/op1_db.py`, `op1_gfx.py`, `op1_patches.py` | the builder app's mods (`app/backend/mods.py`): iter, filter, the tape and iter graphics |
| `op1repacker/assets/presets/iter/*.aif` | the iter factory presets the app's librarian copies and lists |
| `op1repacker/assets/display/` | the iter-lab screen and the graphic patches |

The kennel plugins (`ports/knl/`) do not use it; it belongs to the firmware-image path.
