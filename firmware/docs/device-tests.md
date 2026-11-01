# Device tests

coop-fw packages are built here and flashed by the owner. Every test package:
- leaves out `te-boot.ldr`;
- passes `tools/verify_bootloader.py`;
- decodes to stock firmware 246 with only `OP1_vdk.ldr` changed (`tools/pkg_check.py`).

Recovery always works the same way: hold COM at power-on, choose "1. Upload firmware", and copy stock
`op1_246.op1`. That path runs before any app code (`docs/brick-safety.md`). Proven on the owner's unit
2026-10-14 (te-op1-cfw Test 1).

## Protocol

Each test gets a numbered section below: package name, what changed against stock (blocks, patched words),
the procedure as numbered steps, what to read back (screen page, beep sequence, file on the disk-mode drive),
and the result as numbered points matching the steps. Numbering continues from te-op1-cfw's device tests, which
ended at Test 63.

How results come back without JTAG:
- **screen**: a readout page drawn with the stroke font (white click pages, as te-op1-cfw cpuprobe);
- **file**: written to `/yaffs2/user/` by a main-loop hook, copied off in disk mode;
- **beeps**: `te-op1-cfw/tools/decode_probe_beeps.py` on a phone recording;
- **photo**: a phone photo of the screen when a number must be exact.

## Test 64: L1 dumper (Phase 1)

**Package:** built from `tools/dump_l1/` injected into region C; see `tools/dump_l1/README.md` for
the exact build, inject, package and verify commands, and the on-device read-back.

What changed against stock (confirmed by `pkg_check.py`): only `OP1_vdk.ldr`, and inside it only the
main-loop tape-service CALL at `0x019F0A24` (retargeted to the payload's `ServiceHook`, stock target
`0x01A54710` re-checked by `inject_app.py`) plus the appended region-C code/FILL blocks. The package
passes `verify_bootloader.py` (no `te-boot.ldr`).

**Procedure (owner):**
1. Prove stock recovery once (COM at power-on, flash stock `op1_246.op1`).
2. Flash the dumper package through the te-boot menu.
3. Boot normally, wait a few seconds.
4. Disk mode, copy `user/coop_l1.bin` off.
5. Reflash stock.

**Read back / HARD GATE (offline):**
- `l1_decrypt.py check-key` with the key bytes from `coop_l1.bin` must print a matching arrangement.
- `l1_decrypt.py compare` of the decrypted CALLBACK blocks against the `0xffa00000` region in the
  dump must report **zero mismatches**. Phase 2 does not start until it does.

**Run 1 (2026-11-01): HANG on the loading screen.** Two bugs, both fixed:
1. The dumper read L1 through MDMA0 (`0xffc00f00`), which is the firmware's display-copy engine
   (memory-map.md 5; confirmed: MDMA0 registers have many references in the disassembly, MDMA1 none).
   Seizing it mid-boot broke the display DMA, hence the hang. Fix: the minimal build no longer uses
   DMA at all (L1 data and the key are read by ordinary loads); only the optional `DUMP_L1CODE`
   build reads L1 instruction SRAM, and it uses MDMA1 with a bounded wait.
2. `ServiceHook` was declared `(void* self, int arg)`, but the stock tape service takes one `int` in
   R0 (kennel's `STOCK_SERVICE` is `int(*)(int)`). The forwarded call passed the wrong registers.
   Fix: `ServiceHook(int arg)`, forwarding `((int(*)(int))0x01A54710)(arg)`.

**Run 2 plan (one thing at a time):**
- `op1_246_coop_canary.op1`: the hook only forwards to the stock service (72 bytes). Boots cleanly =>
  the CALL-retarget hook and signature are sound.
- then `op1_246_coop_dump.op1`: DMA-free capture of the key + L1 data, writes `coop_l1.bin`.
- then `op1_246_coop_dump_otp.op1` / `_full.op1`: add the OTP (boot ROM) and L1 instruction SRAM
  (MDMA1) once the minimal path works.

**Result:** pending run 2.
