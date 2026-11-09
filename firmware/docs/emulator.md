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
wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && bash tools/setup_op1emu.sh'
# or point it at a local OTP dump instead of fetching the public one:
wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && OTP_TXT=/path/to/otp.txt bash tools/setup_op1emu.sh'
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
  whatever PC the CPU state holds then (the loader's shows as `0xffa00000`). MMR writes are traced too
  (the JIT's MMR fast path is covered), so `--watch-writes 0xffc00c00+0x400` lists every DMA register
  write with its PC: that is how the SPORT TX ring was found.
- **Hear a headless run:** `--audio-dump PATH` writes the SPORT transmit frames (what the codec plays)
  as a 16-bit stereo 44.1 kHz WAV instead of opening a sound device, in headless and deterministic runs
  too, and logs the frame count and how many frames were nonzero: the end-to-end "does it make sound"
  test (`tests/sim/hear.sh LDR SCRIPT OUT.wav`). `--audio` opens the real device in a headless run.
  Milestone B's image passed every bus probe and failed this test with 0 nonzero frames
  (`docs/audio-graph.md`, second correction).

Results from the first use (stock R246, chord + drum scenarios) are in `docs/stereo-bus.md` and
`symbols/l1.sym` / `symbols/sdram.sym`: the active per-voice accumulate, decimator and mix loops, with
the stock "global pan" loop proven to write zeros.

**Timing facts this corrected:** the audio callback and the per-block bus clears run from **~1.96 s
guest time**, not ~20 s; the "20 s" was wall-clock boot in deterministic mode (script `wait` is wall
time, and the guest ran ~1 s of guest time per wall second here). A chord at ~18 s wall landed at
~21-23 s guest. Trust the trace's guest-time columns over script timing.

## Deterministic scenarios, parallel runs, stereo dumps (2026-11-02)

- **Guest-timed input.** The headless script command `at MS` schedules the following `press`/`release`/
  `tap` lines for guest time MS instead of wall time, so two runs of two images produce frame-aligned
  audio. `tests/sim/det_chord.txt` is the reference scenario (synth at 30 s, C-E-G at 33-34 s); with it
  `tools/wav_compare.py A.wav B.wav --skip-seconds 5` compares sample for sample (exit 0 = identical
  within `--max-diff`). Wall-time scripts (`cap.txt`, `hear.txt`) land the note in a different block each
  run; `--align THRESH` lines them up on the onset, but the stock engine's own time-dependent
  modulation still leaves a residue (max 85 LSB seen), so they cannot prove exactness.
- **Parallel runs.** `tests/sim/hear.sh` and `tests/sim/watch_writes.sh` copy the pristine NAND to a
  per-process file (`hear_nand_$$.bin`, deleted on exit) and `hear.sh` logs next to its WAV, so several
  emulator runs can overlap.
- **Stereo in the dump.** The firmware configures the SPORT with two words per frame and no stereo
  frame sync, and writes L to word 0 and R to word 1 (`0xffa01642` / `0xffa01662`). op1emu's WAV dump
  and host playback took "no stereo frame sync" to mean mono and duplicated word 0; the coop patch
  (`tools/op1emu-patches/0001-write-trace.patch`, second commit) takes word 1 as the right channel.
  Separately, the firmware only runs its stereo conversion when the output-mode byte `0x018b621d` is
  nonzero (headphones/line out); op1emu never sets it and the speaker path sums `(e34 + f34) / 2`, so
  **a stereo check in the emulator needs an image with `g_coop_force_stereo_out=1`** (`stereo_bus.cc`;
  `tools/build_image.sh OUT.ldr --set-word g_coop_force_stereo_out=1`). Images for the regression gate
  leave it 0.
- **One-command images.** `tools/build_image.sh OUT.ldr [--set-word NAME=VALUE ...] [--device OUT_enc.ldr]`
  runs the whole pipeline (inject, decrypt with the key read from `otp.bin`, master imm patch, stereo
  insert) and never stores the key.

## Diagnosing a tape/record difference (lesson from milestone T1, 2026-11-02)

When a coop image differs from stock only in the tape playback, compare the **recorded RAM data**, not
only the WAV: `tests/sim/watch_writes.sh IMG tests/sim/det_tape_rec.txt 0x000e2940+0xc00100` with
`WATCH_LOG=200000 WATCH_FROM=33.0` logs every record store (`pc=0xffa065de`, node data in heap 0;
the 24 node blocks are 0x40000 apart on stock and 0x80000 apart with the 8-lane cache), and a short
Python pass indexes them by `(addr - node_data) >> 1` to compare sample values at the same tape
positions between two runs. Two traps met on the way: a trace range that ends inside the last node
looks like a recording hole, and a probe that switches one coop path off through a control word other
coop code also reads (here the lane gain layout) measures the wrong thing. The per-block tape and record
state words (`0xff802974`, `0xff80298c`, `0x018c17d8`, `0x018c17ec`) compared identical between the
runs in that case; the recorded values did not, which is what pointed at the record pass itself.

## Power-cycle scenarios (NAND_KEEP)

`NAND_KEEP=/path/nand.bin tests/sim/hear.sh ...` (and `watch_writes.sh`) runs on a persistent NAND copy
(made from `pristine.bin` when missing, never deleted): run 1 `tests/sim/det_tape_rec_quit.txt` records
and quits after 45 s of wall time for the write-back, run 2 `tests/sim/det_tape_play_start.txt` boots the
same NAND, STOPs twice (back to the start marker) and plays. A script line `quit` must follow a `wait`;
`at MS quit` quits immediately (the `at` queue carries key events only). The tape position is restored
from `tape.db` at boot, so play without the STOPs starts wherever the last save left it.

**Loop keys (2026-11-02):** the OP-1 has no loop key; on the tape page the sound keys `s1` / `s2` / `s3` are
loop in / loop out / loop on-off (key codes 0x2d-0x2f). `tests/sim/det_tape_loop.txt` sets a loop round a
take; the coop images emulate slower than stock, so its tail `wait` is 150 s to capture five passes.
`tests/sim/det_tape_rec_long_quit.txt` records a 4-second take (six chunk boundaries) for a power-cycle run
followed by `det_tape_play_start.txt`: the file path of every chunk.

## Gate runs and the kernel check (T4a, 2026-11-02)

- Two emulator runs are frame-aligned only when the script schedules its events in guest time (`at`, as
  `det_chord.txt`) **and** the runs happen one at a time: `wait` lines are wall time, so a parallel run or a build
  shifts the end of a run and any wall-timed event. Run gate scenarios sequentially on an idle machine.
- `tests/sim/run_kern_check.sh` builds `kern_check.cc` for the Blackfin and runs it in the GNU simulator: the T4
  assembly kernels of `coop_stereo.h` against their C references over 3000 random blocks with edge values.
- A one-block shift between two images ("63 vs 64 at 31.6 s", max diff ~5000) is the onset landing on the next block boundary, not a DSP difference: compare again with `--align 200`; a true kernel difference shows as small diffs from the onset on without the shift. Two runs of one image in one job are identical.

## Selecting a synth engine headless (2026-11-03)

- `tools/op1emu-patches/0002-headless-encoders.patch` adds `turn ENC cw|ccw [STEPS]` to the headless frontend
  (guest-timed, after an `at`): each step advances the two-pin Gray sequence the GUI's clicks produce, 20 ms apart.
  The stock engine list (shift + key 1 on the synth page) moves one item per 8 steps, but while it is shown the
  keys play nothing in the emulator (stock image too) and nothing closes it (key 1, shift, the encoder push, the
  synth key were tried: `tests/sim/explore_engine*.txt`), so it is no use for a scenario.
- What works: the eight sound slots (`s1`..`s8`) on the synth page load the factory patches of the pristine NAND
  with their engines: 1 digital, 2 voltage, 3 pulse, 4 string, **5 dsynth** (`tests/sim/explore_slots.txt`,
  screenshots). `tests/sim/det_dsynth_chord.txt` / `det_dsynth_stats.txt` play a DSynth chord that way (wall-paced:
  `at` events do not block, so a screenshot after an `at` lands at the start of the run; pace with `wait`).
- `tools/wav_envelope.py` / `wav_compare.py` read the dumps; `scratchpad ppm2png` is not in the repo: the headless
  `screenshot` writes a P6 PPM, which any image viewer opens.

## A NAND with files for the emulator (2026-11-03)

`tools/make_nand.py` builds an op1emu NAND from the stock `.op1` package plus an overlay directory whose tree lands
at the yaffs root (`overlay/plugins/x.coop` -> `/yaffs2/plugins/x.coop`), through op1emu's `provision_nand.py`
functions; `docs/plugins.md` has the command. Two NANDs boot on different block boundaries, so compare their dumps
with `--align`.

## Any firmware in the window, with real controls (2026-11-05)

`tools/op1emu_gui.sh PACKAGE.op1 [NAME]` runs a firmware package in op1emu's GLFW window (WSLg on Windows 11): it
unpacks the package (`tools/op1_unpack.py`, refuses tar members outside the target), decrypts the L1 with the key in
`otp.bin`, builds a NAND from that package's own content once (`FRESH=1` rebuilds it) and keeps both in
`~/op1emu-work/fw_NAME/`. Third-party firmwares run as they are: SilverTape beta 2 boots (its tape screen shows its
own "PREPARING STEREO" first-boot step), no fault.

`tools/op1emu-patches/0003-gui-controls.patch` gives the window usable controls:
- **Keyboard** (`gui/ui.json` `"keymap"`, computer key -> OP-1 button name, editable): tracker layout, F3..E4 on
  `z s x d c v b h n j m`, F4..E5 on `q 2 w 3 e 4 r t 6 y 7 u`; F1-F4 synth / drum / tape / mixer, F5-F8 T1-T4;
  keypad 1-8 or Ctrl+1..8 the sound slots; Shift; Space play, Enter rec, Backspace stop, the arrows; Home lift,
  End drop, Page Down split; Tab seq, F9 mic, F10 com, F11 tempo, F12 help. A key release goes to the button its press
  pressed, so changing a modifier in between leaves nothing stuck.
- **Encoders**: drag up or right turns clockwise, one Gray transition per 4 px (two per detent); the mouse wheel turns
  one detent per notch; a click without moving is the encoder's push. Transitions go out at most four per frame,
  2 ms apart in guest time (edges sent at once would be lost by the firmware's GPIO interrupt). The volume knob drags
  and wheels too. The old click areas are replaced (their anticlockwise step also indexed out of its table).
