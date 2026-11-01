# op1emu: the emulator as coop-fw's test harness

> **CONFIRMED WORKING (2026-11-01).** op1emu boots our decrypted OP-1 R246 firmware headless and
> reaches the main UI (the tape screen). Full pipeline, no device dump: public OTP key
> (Tolsi/op1dumps) -> verified with `l1_decrypt.py check-key` against kennel's pairs ->
> `otp_txt_to_bin.py` otp.bin -> `provision_nand.py` nand.bin from the stock `op1_246.op1` ->
> `l1_decrypt.py decrypt-ldr` plaintext.ldr -> `op1emu nand.bin plaintext.ldr --headless`. Built with
> g++-10 + LLVM 15; `tools/setup_op1emu.sh` does it all. Boot to the tape screen ~3 min headless
> (first boot recreates the tape FAT image; a persisted NAND boots faster). This is the Phase 2 dev
> loop.


[op1emu](https://github.com/op1emu/op1emu) (MIT) is a working BF52x emulator for the OP-1: a JIT CPU
(its `bcore` and `bfin_sim` submodules), the boot ROM, NAND, OTP, PPI/DMA, SPORT audio, the display,
and USB/IP. It boots stock R246 to the main screen, desktop or in the browser (WebAssembly). It is
almost certainly the private emulator hundekraft's kennel 3.1 sources refer to (its comments cite
`OP1-Emulator run/...` paths, and op1emu went public on the day kennel 3.1 shipped). This is the
device-test harness `te-op1-cfw/docs/emulation-feasibility.md` rated as "weeks to months"; someone
built it, so coop-fw uses it instead of re-deriving it.

## Why it matters for coop-fw

- **A second, independent check of the Phase 1 dump.** With the owner's `otp.bin` and NAND image,
  op1emu boots the owner's real firmware; its decrypted L1 can be read out of emulator memory and
  compared against the device MDMA dump. Two independent recoveries agreeing is a stronger gate than
  one.
- **The main dev loop for Phase 2 onward.** Headless, deterministic and scriptable: `tap`, `wait`,
  `wait-frames`, `screenshot`, plus SPORT audio capture. A coop-fw build injected into the NAND image
  boots in the emulator and is driven by a script, so most flash-and-photograph cycles disappear. The
  owner's device stays the final word on timing and the brick-safety checklist.
- **Deterministic regression runs.** Same NAND + OTP + input script + RTC epoch replays the same
  instructions and frames, so a coop-fw change can be diffed frame by frame.
- **A browser build** that pairs naturally with co-op (also browser) for a future "try coop-fw"
  preview.

## It does not replace the device dump

op1emu needs two things only the owner's device provides:

1. **`otp.bin`** (0x200 pages x 16 bytes = 8 KiB; page 0xD0 is the XTEA key). The Phase 1 dumper's
   `DUMP_OTP` build captures the whole OTP; `tools/mk_otp_bin.py` writes `otp.bin` from the
   `coop_l1.bin` dump. The OTP is the gating secret, the same one the audio-core work needs.
2. **A NAND image.** op1emu boots from a NAND image plus `te-boot.ldr`/`bootrom.bin`, not from a
   `.op1` package directly. Getting one means either a device NAND dump or booting te-boot inside the
   emulator and letting it flash a stock `.op1` (to be worked out; op1emu has no public NAND-image
   builder yet). This is the open integration task.

## Caveats

- It is work in progress; this project has not run it on hardware-identical output yet.
- Timing is "faster than the device whenever the host is", so it is for correctness, UI and
  regression, not the final CPU budget. coop-fw's budgets (0.75 of 580,499 cycles/block, the fps
  floor) are still confirmed on the device. The plan already budgets the sim at 1.2-4x.
- MIT, so it is fine as a dev-only tool beside GPL-3.0-or-later coop-fw. It is never linked into
  firmware; nothing of op1emu ships in a `.op1`.

## How coop-fw will use it (planned)

- Phase 1: after `otp.bin` and a NAND image exist, add a second gate that reads op1emu's decrypted L1
  and compares it to the device dump.
- Phase 2+: an `op1emu`-based harness (likely under `tests/sim/` with a checked-out op1emu) that boots
  a coop-fw NAND image headless, runs an input script, and captures screens and audio for the host
  test suite and CI (where a NAND image is available to the runner; never a committed one).

## Setup (run it yourself; it builds third-party code)

`tools/setup_op1emu.sh` does the whole thing in WSL: installs deps, builds op1emu and `mkyaffs2`,
provisions `nand.bin` from your stock `op1_246.op1`, writes `otp.bin` from a community OTP dump with
`tools/otp_txt_to_bin.py`, verifies the key against kennel's pairs, and boots headless to a
screenshot. Because it clones and builds external projects (op1emu, GLFW, yaffs2utils), Claude Code's
auto mode will not run it for you; run it yourself or grant the permission:

```sh
wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/coop-fw && bash tools/setup_op1emu.sh'
# or point it at a local OTP dump instead of fetching the public one:
wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/coop-fw && OTP_TXT=/path/to/otp.txt bash tools/setup_op1emu.sh'
```

Requirements it can't self-provide: CMake >= 3.20 (it installs via pip), a C++17 compiler, and passwordless
`sudo` for apt. If the headless boot fails only with a garbled/kacked screen, the OTP byte order is
reversed for your dump: rerun `otp_txt_to_bin.py` with `--no-reverse`.
