# L1 dumper (Phase 1)

Gets the OP-1's runtime-decrypted L1 and its OTP page-0xD0 key off the owner's own device, so
coop-fw can reverse-engineer and replace the audio core. Everything it produces belongs to the
owner's device and is gitignored; nothing here contains Teenage Engineering firmware or any key.

## What it does

A tiny payload (700 bytes of code, 128 KiB staging buffer) linked into free SDRAM region C
(`0x01e00000`). `tools/inject_app.py` retargets the main-loop tape-service CALL at `0x019F0A24`
(stock target `tape_c::service`, `0x01A54710`) to the payload's `ServiceHook`. On the first main-loop
pass it:

1. MDMA-copies L1 instruction SRAM (`0xffa00000`, 48 KiB; not readable by ordinary core loads) to an
   SDRAM staging buffer, and plain-copies L1 data A, L1 data B and the scratchpad;
2. reads the 16-byte OTP key from the te-boot context block at `0x03ffff10`;
3. after ~64 passes (the filesystem has settled) writes `/yaffs2/user/coop_l1.bin`;
4. forwards to the stock tape service every pass, so the device runs normally.

No code runs in the audio interrupt. Region C is used, not region A, because a `CALL` from the hook
site reaches only +/-16 MB and region A (`0x02c00000`) is ~19 MB away.

### `coop_l1.bin` format (little-endian)

```
u32 magic "C1DP", u32 version (1), u32 region_count
region_count x { u32 addr, u32 len }        the table
u8  key[16]                                 OTP page 0xD0 as the context block holds it
raw region bytes, in table order            (0xffa00000, 0xff800000, 0xff900000, 0xffb00000)
```

## Build

```sh
wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/coop-fw/tools/dump_l1 && make'      # minimal: key + L1
wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/coop-fw/tools/dump_l1 && make otp'  # also full OTP (for op1emu)
# -> build/dump_l1.elf / build/dump_l1_otp.elf  (OP1X header, render=0, one CALL-retarget patch)
```

Flash order after a hang (one variable at a time): `canary` (hook only) -> minimal (key + L1 data,
DMA-free) -> `otp` / `full`. **Never use MDMA0 (`0xffc00f00`): it is the firmware's display engine and
seizing it hangs the device on the loading screen.** The `full` build reads L1 instruction SRAM through
MDMA1 (unused by the firmware) with a bounded wait. The `otp` build additionally
reads the whole OTP through the boot ROM so `tools/mk_otp_bin.py` can produce op1emu's `otp.bin`
(see `docs/emulator.md`); OTP reads are non-destructive.

## Make the package (over the owner's OWN stock image)

`STOCK` is an unpacked stock `op1_246` directory (`OP1_vdk.ldr`, `te-boot.ldr`, `content/`). Never
commit it.

```sh
STOCK="/path/to/op1_246"
python tools/inject_app.py "$STOCK/OP1_vdk.ldr" /tmp/work/OP1_vdk.ldr \
    tools/dump_l1/build/dump_l1.elf --region 0x01e00000:0x02000000 --fill-pattern 0xa5a5a5a5
cp -r "$STOCK" /tmp/work-pkg && cp /tmp/work/OP1_vdk.ldr /tmp/work-pkg/OP1_vdk.ldr
python tools/build_package.py /tmp/work-pkg /tmp/op1_246_coop_dump.op1   # drops te-boot.ldr
python tools/verify_bootloader.py /tmp/op1_246_coop_dump.op1            # must PASS
python tools/pkg_check.py /tmp/op1_246_coop_dump.op1 "$STOCK"           # only OP1_vdk.ldr differs
```

## On the device (owner)

1. Prove recovery first: COM at power-on, flash stock `op1_246.op1` through the te-boot menu (see
   `docs/brick-safety.md`). Do this once before any custom flash.
2. Flash `op1_246_coop_dump.op1` through the te-boot menu.
3. Let it boot normally and sit for a few seconds.
4. Enter disk mode (Shift + COM, then DISK). Copy `coop_l1.bin` off the `user` folder.
5. Reflash stock when done (the dumper is a throwaway build).

## Verify the dump offline (the HARD GATE)

```sh
# 1. Confirm the key decrypts the bytes kennel's patches reveal, before trusting anything:
python tools/l1_decrypt.py check-key "$STOCK/OP1_vdk.ldr" \
    third_party/kennel/synth_sdk/l1_patches.json <KEYHEX from coop_l1.bin>
#    -> prints the key arrangement (--swap/--order/--words) that matches.

# 2. Decrypt the shipped CALLBACK blocks and byte-compare against the device dump of the
#    instruction SRAM region (the gate: zero mismatches):
python tools/l1_decrypt.py compare "$STOCK/OP1_vdk.ldr" <KEYHEX> region_ffa00000.bin \
    --base 0xffa00000 --swap <s> --order <o> --words <w>
```

`tools/mk_otp_bin.py coop_l1.bin out/` unpacks the dump: `out/key.hex`, each `out/region_*.bin`,
and `out/otp.bin` (from an `otp` build). A byte-exact `compare` match proves the key, proves the
dump, and proves we can re-encrypt our own L1 blocks later. Do not start Phase 2 until it matches.
With `out/otp.bin` and a NAND image, op1emu boots the owner's firmware as a second check and becomes
the Phase 2+ dev loop (`docs/emulator.md`).

## Open item before the first flash

`g_patches[0].stock` is `0x01A54710`, decoded from kennel's recorded CALL bytes at `0x019F0A24`;
`inject_app.py` re-checks it against the owner's image and the injection above passed, so it is
confirmed for R246. If a future image differs, `inject_app.py` will refuse and print the expected
bytes.
