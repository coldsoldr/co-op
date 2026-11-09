# coop-fw: working notes for agents

Custom firmware for the original OP-1 (R246). Stock firmware stays the OS; coop-fw replaces the audio core and
adds a plugin runtime. The plan of record lives outside the repo (the owner's plan file); `docs/` holds the working
documents. Read `docs/brick-safety.md` before producing anything that goes on the device.

## Hard rules
- Never commit Teenage Engineering firmware bytes, `.op1`/`.ldr` files, decrypted L1 dumps or the OTP key.
  `.gitignore` enforces the obvious cases; think before adding binary fixtures.
- Every package: no `te-boot.ldr`, `tools/verify_bootloader.py` passes, diff against stock shows only intended changes.
- Patch targets are checked against stock bytes before writing (inject_app.py does this); never patch blind.
- Nothing is "done" without a device result recorded in `docs/device-tests.md`. The owner flashes; agents build.
- Audio code: fixed point only, no float, no libc, no static constructors, hot loops small (12 KB fits the I-cache).

## Where things are
- Sibling research repo: `../../te-op1-cfw` (docs/, tools/, ports/ with the DSP cores and host tests).
- The librarian is the repo root (`../web`, `../shared`: profiles, USB SysEx client, plugin schema). The repo is
  public (co-op is MIT, this folder GPL-3.0-or-later): the hard rules above matter more, not less.
- hundekraft 3.1 source: `../../te-op1-cfw/incoming/hundekraft/3.1/` (gitignored); vendored subset in `third_party/kennel/`.
- Toolchain: WSL, `~/op1-toolchain/bfin/bin` (bfin-elf GCC 14.2, binutils 2.43, gdb + simulator).
- Build from Windows: `wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make <target>'`.

## Conventions
- Firmware addresses come from `symbols/*.sym` through generated headers; cite the source doc or the L1 map next to
  any hand-written address.
- Python tools are stdlib only, Python 3.8 compatible (WSL has 3.8.2).
- Docs state confidence: [C] confirmed on device or in bytes, [I] inferred.
- UI features and plugin screens are mocked in the playground (`tools/playground/coop-playground.html`) before they
  are built; the owner's UI decisions are in `docs/ui-features.md`, the plugin rules in `guide/plugin-guidelines.md`.
