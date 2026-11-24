# Brick safety

Goal: no custom build can ever leave the OP-1 unable to reach te-boot (the bootloader), because te-boot is what lets us reflash stock firmware. Everything below comes from te-boot v2.30 / firmware 246, by disassembly (`tools/ldr_dump.py --elf` + `bfin-elf-objdump`). Addresses are runtime addresses.

## TL;DR rules

1. **Custom packages never contain `te-boot.ldr`.** Build them with `python tools/build_package.py <unpacked_dir> <out.op1>`, which drops it and runs the guard. With no `te-boot.ldr` in the package, *neither* update path touches the bootloader (proof below).
2. **Every package must pass `python tools/verify_bootloader.py <file.op1>` before it goes on the device.**
3. **Package file names must not contain `FRC`**, which forces a bootloader rewrite. Never put a **`.KEY`** file on the OP-1 disk: it triggers irreversible OTP key programming. (`RBT` in the name only adds a reboot after the update, so it's harmless but best avoided.) The guard checks all of this.
4. **Install only through the te-boot menu:** hold COM while switching on, then "1. Upload firmware". Don't use the app-side update path (see "Path B"), which forces a bootloader rewrite whenever the package contains te-boot.ldr.
5. ✅ *Done 2026-10-14: stock flash showed "te_boot skipped", so the unit runs v2.30 and recovery works.* **Before any custom flash, check the unit's bootloader is v2.30** (shown on the te-boot screen), and **prove recovery once by flashing stock `op1_246.op1` through the te-boot menu**. With a v2.30 unit and the stock file name, te-boot skips the bootloader write (see "Decision logic").
6. Don't use te-boot "7. Factory reset" or "8. Format drive" during development. They wipe YAFFS2, including `/yaffs2/cmel_gamma.dat` and `/yaffs2/samsung.dat`, the display gamma calibration files.
7. **No odd buffer reaches a yaffs read or write** (the stock NAND driver asserts on it, at every boot if the read is a
   boot read). See "Even buffers for the stock file layer".

## Update pipeline

### Path A: te-boot menu (te-boot `0x0006xx`–`0x0009xx`, update routine `0xefca`)
1. Scans the USB disk for files by name (strstr, case-sensitive):
   - `.KEY` leads to "Found KEY file...", key-file decryption, then **OTP key programming** through the Boot ROM `bfrom_OtpWrite` (`0xEF00001C`). Irreversible.
   - `.OP1` leads to "Found firmware package..."
   - name contains `RBT`: reboot after a successful update (`0x169ec`)
   - name contains `FRC`: **force flag = 1**
2. `0xefca(pkg, len, force)`:
   - CRC-32 of the package, then "Wrong CRC-32, aborting!" or "CRC-32 OK"
   - **unlinks the old `/yaffs2/te-boot.ldr` and `/yaffs2/OP1_vdk.ldr`**
   - LZMA decode and untar into YAFFS2 (`0xe8a0`, `0xede8`); on failure, "Decode failed, aborting!"
   - `access("/yaffs2/te-boot.ldr")` (`0x2350c`). **Only if the file exists**: "Updating te-boot...", then `0x1068(path, force)`
   - `access("/yaffs2/OP1_vdk.ldr")`. If present: "Writing firmware..." `0x1388`, then "firmware written."

### Path B: running app (OP1_vdk)
- At startup (`OP_Main`, around `0x19f0d0c`), "Found firmware..." calls the package routine `0x1a5dedc(buf, len, force=1)` with **force hard-wired to 1**.
- A second caller, the PC sync path (`pc_bf_sync.cpp`, `0x1a0a242`), passes force=0.
- `0x1a5dedc` mirrors path A: CRC, unlink both `.ldr` files, decode and untar, then `access("/yaffs2/te-boot.ldr")` (`0x1a5e062`). **Only if it exists** does it call the writer `0x19a1900(path, force)`.
- So on this path a package that contains te-boot.ldr **rewrites the bootloader every time, even when it's identical**.

### Decision logic: te-boot writer `0x1068` (app copy: `0x19a1900`)
The float-comparison helpers are ROM-style routines in L1. The direction of the `> 1.1` and `> 2.05` checks is inferred from the surrounding messages.
```
read up to 256 KB of /yaffs2/te-boot.ldr
if size >= 0x3ff00:                      "te-boot too large, abort..."      -> fail
if memmem(img, "te_boot_version:2.30") and !force:                         -> SKIP ("te_boot skipped.")
v = strtof(after "te_boot_version:")
if v > 1.1:  copy running unit's calibration block into img (after "Test results goes here:")
if v > 2.05: copy running unit's 54-byte CMEL gamma (from L1 0xff8029c6) into img after "CMEL gamma values:"
             (no marker in img and unit has gamma -> "No room for gamma calibration" -> fail)
if v < 2.3:  "version number %1.2f is too low."                            -> fail (no downgrades)
erase + program NAND boot blocks in 128 KB chunks (0x833c), pad 0xFF       -> "te_boot written."
unlink /yaffs2/te-boot.ldr
```
- In te-boot, the compared string is the *running* bootloader's own version. In the app copy, it's the version the app was built with (also 2.30).
- The write is **not atomic**: losing power or crashing mid-write leaves a half-written bootloader. That's the brick scenario.
- The bootloader image on each unit carries **unit-specific calibration**, merged in at write time. You can't reconstruct it from TE's file alone.
- NAND boot blocks are write-locked except in this routine: `nand.cpp` asserts `(block >= NUM_NAND_BOOT_BLOCKS) || !write_lock_enabled`.

### Why omitting te-boot.ldr is safe
- Both paths unlink `/yaffs2/te-boot.ldr` *before* untarring, and the writer unlinks it afterwards. So the stock steady state is "no te-boot.ldr on YAFFS2", and nothing reads it except the writer.
- Both paths gate the writer on `access(...) == 0`. With no file, the writer is never called.
- Verified: `tools/build_package.py` output decodes to the 117 stock entries minus `te-boot.ldr` (116), all byte-identical.

## Even buffers for the stock file layer

Not a brick (te-boot is untouched), but a boot loop: TE's NAND driver asserts `te_yflash2.c:600
!((unsigned int)data & 1)`, so a yaffs read or write whose data buffer starts at an odd address halts the device, and
if the read happens at boot it halts every boot until the file is removed (stock firmware through te-boot, or a
coop with the fix). Found by **op1-sdk** (MIT; `docs/lessons.md`, 2026-11-15, "PLAY: a boot failure from an odd buffer
address"): their `SONG.BIN` buffer was even in one build and odd in the next. [C]: the halt on their device; the assert
string `!((unsigned int)data & 1)` with `te_yflash2` is in the stock R246 `OP1_vdk.ldr`.

Why it is accidental in coop: a `uint8_t` / `char` array has alignment 1 on the Blackfin (no `DATA_ALIGNMENT` bump; a
test compile puts a one-byte stack local at `SP + 0x17`), and `-fdata-sections` gives every static its own section, so
an array lands wherever the objects before it end. Before this fix one 32-byte array of the image was odd
(`g_saved_once`, not a file buffer), and SysEx FILE_GET read every chunk into `out + 5`: always odd.

**The rule:** every buffer coop hands to the stock's yaffs read or write starts even. Static and stack byte buffers
carry `__attribute__((aligned(4)))`; heap, arena and fixed-address buffers (plugin staging at `COOP_POOL_LO`, the
tape `g_bounce`s) are aligned by construction; never pass a string literal or a pointer into the middle of a buffer
(`out + 5`): copy through an aligned buffer.

**The guards:**
- `tools/even_bufs.py`, run by every `make combined` / `combined-release` link: every data object of two bytes or more
  in the ELF starts even, or the build fails and the ELF is deleted (`tests/tools/test_even_bufs.py`).
- `core/include/coop_yio.h`: `coop_y_read` / `coop_y_write` pass an even buffer straight to the stock (0x01A99C90 /
  0x01A99F84) and move an odd one through a 64-byte aligned stack bounce, counted in `g_coop_odd_io` (0 in a good
  image). `Y_READ` / `Y_WRITE` are these in coop_place, coop_runner, coop_sysex, coop_tape_ui, tape_edit and tape_io.
  coop_settings, plugin_host and coop_stats still call the stock directly; their buffers are aligned and checked by
  `even_bufs.py` (plugin_host's stack buffers by their attribute only). A new file that calls yaffs includes
  `coop_yio.h`.

## Remaining risks / open questions
- ~~Does te-boot verify the app image?~~ **Resolved: no.** See `docs/app-boot-chain.md`. The app's L1 code is XTEA-encrypted with a key held in OTP, but nothing is authenticated. Patch only plain SDRAM blocks and carry the encrypted CALLBACK blocks over unchanged.
- **te-boot is reached before any app code runs.** The init DXE reads the COM key and either boots the app or falls into the te-boot menu, so a broken custom app can't block recovery.
- **App size limit:** the app may use NAND boot blocks 4–127 (≈15.5 MB). The flasher has no size check of its own, so `verify_bootloader.py` should enforce it. Stock is 2.2 MB.
- A failed custom **app** is recoverable only if te-boot still starts and accepts USB uploads. te-boot is independent of the app (the Boot ROM loads it from the NAND boot blocks), so this should hold. Prove it once with stock (rule 5).
- Last resort if te-boot itself is ever damaged: JTAG or the BF524 boot-mode pins. Whether either is reachable on the OP-1 PCB is unknown.
