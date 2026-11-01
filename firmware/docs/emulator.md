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

## Sound from the GUI on WSLg (fixed 2026-11-01)

`tests/sim/play.sh` opens the GUI with host audio. It was silent because WSLg's PulseAudio server runs
as uid 1000 and refuses clients of any other uid ("Access denied"; root included), and this WSL distro
runs as root; on WSLg 1.0.79 the advertised `PULSE_SERVER` socket `/mnt/wslg/PulseServer` also does
not exist, the live one is `/mnt/wslg/runtime-dir/pulse/native` ("Connection refused"). The earlier
"WSLg audio is broken on this machine" conclusion was these two together, not a WSLg bug. `play.sh` now
runs op1emu as the socket's owner (`setpriv --reuid`) with `PULSE_SERVER` set to the live socket, after
making the home directory traversable (`chmod 711`) and the run NAND and `otp.bin` writable (op1emu
opens its OTP storage read-write). Verified: a `paplay` tone as uid 1000 reached the RDP sink, and
op1emu logs `Audio output initialized: 44100Hz, 2ch` with ~170 ms card latency (WSLg's RDP sink; fine
for listening, not for latency tests). op1emu has no device option: miniaudio plays on the server's
default sink, which on WSLg is the one `RDPSink` that forwards to Windows' default output device, so
pick the device on the Windows side. Offline WAV rendering (below) remains the exact A/B method.

## Who writes X: the write tracer (`--watch-writes`, coop patch, 2026-11-01)

The stock profiling probes fire on block-entry PCs and keep at most 262,144 hits in all, so they
cannot answer "which code stores into this buffer" over a run long enough to make sound. coop-fw adds a
write tracer to op1emu instead: `tools/op1emu-patches/0001-write-trace.patch` (applied by
`setup_op1emu.sh` after the clone; `git apply` it by hand on an existing tree and rebuild
`build/op1emu`). It is an upstream-shaped change, kept as a patch because op1emu is a third-party tree.

- **Core:** `Emulator::AddWriteTrace(lo, hi, fn)` sees every store that lands in `[lo, hi]` after it
  reached memory, at every width. The per-address `AddWriteHook` only sees 32-bit stores and
  *replaces* them (the generic 8/16-bit path never consulted it), which is why the voice bus, written
  with `W[I0++]`, needed this. Traced pages leave the JIT's direct page table, so generated code calls
  back for each store; bcore materializes `CpuState::pc` before every memory callback, so the storing
  packet's PC is exact. Any build, no profiling support needed.
- **Front end:** `--watch-writes RANGE` (repeatable; `LO-HI`, `LO+LEN` or `LO`, hex). Prints each new
  writer PC as it appears, then a table at exit: per (range, PC) the store count, **nonzero store
  count** (whether that writer ever carried signal), widths, address span, first/last value, first/last
  guest time and first/last nonzero time. `--watch-out FILE` writes the table as TSV. `--watch-log N`
  with `--watch-from SECONDS` also prints N single stores from that guest time, which gives the order
  of the writers inside one audio block (pair it with the scratch `runs.py`-style grouping: stores
  whose PCs lie within 0x100 bytes are one loop).
- **Wrapper:** `tests/sim/watch_writes.sh LDR SCRIPT RANGE...` copies the pristine NAND, runs headless
  deterministic, prints the table. A host write (DMA, the LDR loader) shows as width `host` under
  whatever PC the CPU state holds then (the loader's shows as `0xffa00000`).

Results from the first use (stock R246, chord + drum scenarios) are in `docs/stereo-bus.md` and
`symbols/l1.sym` / `symbols/sdram.sym`: the active per-voice accumulate, decimator and mix loops, with
the stock "global pan" loop proven to write zeros.

**Timing facts this corrected:** the audio callback and the per-block bus clears run from **~1.96 s
guest time**, not ~20 s; the "20 s" was wall-clock boot in deterministic mode (script `wait` is wall
time, and the guest ran ~1 s of guest time per wall second here). A chord at ~18 s wall landed at
~21-23 s guest. Trust the trace's guest-time columns over script timing.
