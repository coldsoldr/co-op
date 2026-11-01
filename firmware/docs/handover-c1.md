# coop-fw handover — continuing at milestone C1 (stereo instrument bus)

Written 2026-11-01 for a fresh context. Supersedes `docs/handover-2026-11-01.md` for current state.
Self-contained: a new session should be able to continue from here plus the repo + memory.

> **Resume point (end of 2026-11-01, tree clean at `cc45c51`).** Done today: the op1emu write tracer
> (`--watch-writes`) and WAV output (`--audio-dump`, `tests/sim/hear.sh`), both as the patch set in
> `tools/op1emu-patches/`; the stock voice-sum / decimator / mix PCs pinned (§4 resolved,
> `docs/stereo-bus.md`); GUI sound on WSLg fixed (`tests/sim/play.sh`); and the milestone-B silence
> found and fixed (§3 correction, image `mbG.ldr`, coop stage at the getter site). Listen first:
> `~/Downloads/mbG_chord.wav` vs `stock_chord.wav`, or `wsl -e bash -lc '~/op1emu-work/play.sh'`.
> Then: C1 takeover mechanism (`docs/stereo-bus.md` "C1 takeover mechanism"), gated by `hear.sh`
> output, never a bus probe. Open thread for item 11: the stock level stage `0xffa04280` must stay
> until its d34/db4 -> e34/f34 volume mapping is reproduced.

Branch: `phase0-scaffold-and-l1-dumper`. Working tree clean. Owner: Byron
(GitHub coldsoldr). The owner flashes the device;
agents build and verify in the emulator.

---

## 1. What coop-fw is

Custom firmware for the original OP-1 (ADSP-BF524 Blackfin, ~400 MHz, no FPU, 16 KB I-cache, 48 KB L1
instruction SRAM, 44 KB L1 data, 64 MB SDRAM). **Stock R246 stays the OS; coop-fw replaces the audio
core and adds a plugin runtime.** The full plan is in the owner's plan file (also pasted into the first
message of the originating session); `docs/` holds the working documents. Read `docs/brick-safety.md`
before anything that goes on the device.

No-compromise feature list: 4 stereo tape tracks, stereo synths, two FX slots, two LFO slots,
hundekraft/opus plugin compat via a porting guide, USB upgrade, co-op integration, **never compromise
performance**, strict plugin UI guide, specific controls, master bus suite (comp/drive/EQ), keep stock
functions.

Execution order (sequential milestones): A master suite → B two FX slots → **C stereo instrument bus
(current)** → then LFO×2, stereo tape, plugin system, USB/co-op.

---

## 2. Status — done this session (newest first)

| Commit | What |
|---|---|
| `6ab6c7e` | FX ABI: stereo in/out chain + dual-mono hosting decided; drums pannable (docs/fx-abi.md) |
| `9f959e4` | Milestone C start: stereo-bus design (docs/stereo-bus.md) + voice DSP (coop_voice.h) + tests |
| `cc40d23` | tests/sim/play.sh — launch the interactive op1emu GUI on a coop image |
| `7152977` | **Milestone B: coop stereo FX slot + corrected the master bus to d34/db4** |
| `2401668` | Rewrote biquad + Q31 mul onto the Blackfin fractional MAC (45% faster) |
| `38f4b69` | Baseline measured: stock callback vs coop callback under load |
| `7ed9ee6` | Milestone A measured |

**Done and working:** Phase 1 (L1 decryption, gate met). Phase 2 milestone A (master suite,
transparent) and milestone B (coop stereo FX slot) integrated on the real master bus and measured; the
FX slot is audibly validated (see §6). Milestone C foundation: pipeline traced, stereo DSP built and
host-tested, stereo FX architecture decided.

**Loose ends / not yet real:** the master suite (comp/drive/EQ) runs transparent — its parameters are
not wired, so it makes no audible change yet. Milestone C1 (the L1 takeover) is NOT done — see §4.

---

## 3. THE master-bus correction (critical — do not regress)

> **CORRECTED AGAIN later on 2026-11-01 (write tracer + `--audio-dump`, see `docs/audio-graph.md`
> "second correction").** Both buses are real and in series: d34/db4 (int16) is the mixed master bus;
> the stock level/comp stage `0xffa04280` reads it and WRITES e34/f34 (int32), which the SPORT TX
> conversion loop (`0xffa0171c`) and the USB stream consume. e34/f34 are not "USB only" and not zero;
> the probe that saw zeros ran at the callback entry, before the stage. **The milestone-B image (mbF)
> replaced `0xffa04280`, so e34/f34 were never written and the codec output was SILENT** (0 nonzero
> SPORT frames vs 603k for stock); its "audible validation" was a reconstruction from d34/db4, not the
> output. Fixed in image **mbG**: `coop_master_apply` now replaces the trivial SDRAM getter called at
> `0xffa017ac` (`l1_insert.py --imm-pair-site 0xffa017a4 --imm-stock 0x019d90e8 --fn <addr>`), processes
> d34/db4 before the stock stage, and returns the getter's value (0xff80187c); `0xffa04280` stays and
> runs at its original SP. SPORT output of mbG under the chord: 419,813 nonzero frames. The text below
> is kept as history; where it says e34/f34 are the USB bus or zero, it is wrong.

An earlier pass mislabeled `0xff800e34`/`0xff800f34` as the "master pair (int32 Q31)". **That is
WRONG.** op1emu probing proved:

- **The live codec/speaker master bus is `d34`/`db4` = `0xff800d34` (L) / `0xff800db4` (R), int16, 64
  samples, planar** (db4 = d34 + 0x80). It carries audio; the stock master FX render is handed d34/db4;
  the mix/downmix reads them.
- **`0xff800e34`/`0xff800f34` is the USB-audio stream bus** (`fw_usb_audio_stream 0x01915608`), ZERO
  when no USB-audio host streams (0/36341 samples under a chord). NOT the speaker master.
- **`0xffa04280` is the master COMPRESSOR/level DSP** (own frame, operates on `0xff801880`), NOT an EQ.
  The coop master suite + FX insert at `0xffa017ae` *replaces* this call (correct: item 11 = suite
  takes over master comp/drive/EQ). It writes only comp state, not meters, so memorize is unaffected.

Milestone A's "transparent boot" only proved "no crash," not signal flow — it had been processing the
zero e34/f34. The suite + FX now operate on d34/db4 (widen int16→Q31, process, narrow back with
saturation). See `symbols/l1.sym`, `docs/audio-graph.md` (top correction block), `op1-firmware-facts`
memory. **Rule learned: verify a buffer carries signal (op1emu memory probe) before trusting an
insert.**

---

## 4. Milestone C — current state and THE BLOCKER

Goal: true stereo synths — per-voice panning into a stereo bus — plus the voice-spread control (random
per-voice pan scaled by an amount; `synth-voice-spread-feature` memory). Drums pannable too (each drum
voice). Design + staged plan in `docs/stereo-bus.md`.

**Stock pipeline (traced statically, confirmed enough to design):**
1. Per-voice render: vtable dispatch `render = *(vtable+0x24)` (param `W[vtable+0x20]`), around
   `0xffa008f2`. Writes the voice into a buffer.
2. Per-voice mono accumulate: a loop (~`0xffa00912`) multiplies rendered samples by the voice gain
   (`0xff801d50`, int16) and sums into the **mono** bus `0xff800bb4` (128×int16 @ 88.2 kHz).
3. Decimate + **single global pan**: `0xffa00ef0`..`0xffa00f86`, a 64-iteration loop reads the mono
   bus, multiplies by one global left gain (`R0.H`) and one right gain (`R2.H`), accumulates into
   d34/db4 while decimating 88.2→44.1. So stock "stereo" = mono sum + one pan.

**C1 goal:** replace the per-voice mono accumulate with a per-voice *stereo* accumulate (pan each voice
into a stereo bus), then decimate that stereo bus into d34/db4. Centered pan (spread 0) must reproduce
stock mono — the regression gate — before any spread is enabled.

**Foundation built (committed, `core/include/coop_voice.h`, `tests/host/voice_test.cc`):**
equal-power pan law (quarter-sine Q15 table, interpolated), per-voice spread (xorshift RNG, pan =
random×spread at note-on; 0 = centered, full = hard L..R), per-voice stereo mix (pan+gain accumulate
into int32 stereo bus). Centered pan ⇒ L==R. Native Blackfin multiplies, no libgcc. `COOP_MAX_VOICES=12`
covers synth (6) + drums (12).

> **RESOLVED later on 2026-11-01.** The write-hook tracer exists (`--watch-writes`, patch in
> `tools/op1emu-patches/`, wrapper `tests/sim/watch_writes.sh`, writeup in `docs/emulator.md`) and
> answered the question on the stock image: synth per-voice sum = `0xffa00d5a`/`0xffa00d70` (loop
> `LSETUP 0xffa00d4a..0xffa00d62`), drum sum = `0xffa0093e` (the loop static analysis had taken for the
> synth), in-place 128->64 decimator in SDRAM (`0x01967586..0x0196760e`), active mix = loop B
> `0xffa00fca` (L) / `0xffa00fd2` (R) adding the SAME value to both channels; the "global pan" loop A
> (`0xffa00f76`/`0xffa00f7e`) wrote zero in every block. Stock synth/drum output is mono. The "~20 s
> boot" below was wall time; the callback runs from ~1.96 s guest time. Everything is in
> `docs/stereo-bus.md` (pipeline + C1 mechanism options) and `symbols/*.sym`. The text below is kept
> as history of the blocker.

**THE BLOCKER (why C1 stalled):** to place the takeover I must find *which* L1 code actually does the
per-voice sum and the pan-mix (the master stage has many conditional branches; static disassembly can't
tell which run). I tried the emulator's profiling probes and hit two hard walls:
- **Probe hit cap is 262,144 total** (`--profile-trace-events` does NOT raise it; the error is "probes
  keep at most 262144 hits in all").
- **The audio callback does not start until ~20 s of guest boot.** Any run long enough to produce
  sound (≥20 s) overflows the probe cap before the note plays. Scenarios with <20 s boot produce
  silence (the callback `0xffa04804` itself gets 0 hits — this fooled me for several runs).

So probe-surveying cannot answer this. **The `--profile-census` output is region-level, not per-PC** for
memory; its `cen.pc.blk` is a binary list of executed *block-entry* PCs (20-byte entries: `<u32 pc><u64
runs><u64 packets>`, little-endian) but a census run that didn't boot ≥20 s shows almost no voice/master
blocks, same root cause.

**THE UNBLOCK (do this first next session):** build a **write-hook PC-logger in op1emu**. The emulator
already has the machinery: `Emulator::AddWriteHook(u32 addr, std::function<void(u32,u32)>)`
(src/cpu/emu.h:79), called from `MemoryWrite32` (src/cpu/emu.cpp:149) — **check the 16-bit path too**
(the voice bus is written with 16-bit stores `W[I0++]`; confirm `MemoryWrite16`/the generic write also
consults `writeHooks`, and if not, add the check). The writing PC is available as `cpu.State().pc` /
`Emulator::PC()` (virtual, src/cpu/emu.h:27; cpu.h:77) — in a JIT it may be the block-entry PC, which
is still enough to locate the block. Plan:
1. In the headless setup (gui/headless.cpp / gui/main.cpp, after the CPU+emulator are created), install
   a write hook (guard behind an env var like `COOP_WATCH=0xff800bb4`) that logs the first ~16 distinct
   writer PCs with their value, once each.
2. Rebuild (`build-prof` or `build`), run with `COOP_WATCH=0xff800bb4` then `=0xff800d34` on a chord
   that boots ≥20 s and sounds (use the proven `chord.txt`/`note.txt` style, 20 s boot + a note).
3. The logged PCs are the active voice-sum and pan-mix code. Then implement the C1 takeover there
   (retarget/trampoline via `tools/l1_insert.py`, as milestone B did), centered-pan first, verify
   against stock in op1emu (delay-line-style probe + a rendered WAV A/B), then enable spread (C2).

Reusable for the drum takeover and all future "who writes/reads X" questions — worth doing well.

---

## 5. Key reverse-engineering facts (addresses)

All audio DSP is in XTEA-encrypted L1 (decrypted; see below). Plain SDRAM code is patchable with no
integrity check.

- **Master bus (speaker):** d34 `0xff800d34` L / db4 `0xff800db4` R, int16×64 planar. §3.
- **USB-audio bus (not speaker):** `0xff800e34`/`0xff800f34`, int32, zero without USB host. §3.
- **Mono voice bus:** `0xff800bb4`, 128×int16 @ 88.2 kHz (voices summed here). Non-zero under a chord
  (~772M peak via 32-bit probe = two packed int16).
- **Audio callback:** `0xffa04804` (fires every block once booted). Installed via imm16 pair at plain-
  SDRAM `0x0191AB52` (R1.L=0x4804 @ +2, R1.H=0xFFA0 @ +6).
- **Master stage:** `0xffa00e50` (mixer+comp+drive+pan-mix, recursive).
- **Our insert site:** `0xffa017ae` = `CALL 0xffa04280` (the master comp/level, §3). Retargeted every
  block by `l1_insert.py`. Preceded by coeff-build `CALL 0x019d90e8` at `0xffa017ac`.
- **Master FX render:** `0xffa017e6` = `CALL (P0)`, `P0=*(instance+0x14)`, handed d34/db4 (set up at
  `0xffa017bc`/`c6`). Stereo.
- **Per-voice render:** vtable `*(vtable+0x24)` around `0xffa008f2`. **Mono accumulate loop ~`0xffa00912`.
  Decimate+global-pan `0xffa00ef0`.** (These exact PCs still need dynamic confirmation — §4.)
- **Mix/downmix read of d34/db4:** `0xffa072c8` (conditional; not always the active output path).
- Per-voice gain array `0xff801d50` (int16); comp/drive words `0xff80189c`/`0xff801880`; EQ coeffs
  `0xff801b50` (built by `0x019d91ea` from `0xff8017cc`).
- Free SDRAM: **region A `0x02c00000`–`0x02ffffff` (4 MB, PROVEN by running our code there).** Our
  payloads live at the region base (~5 KB); the FX delay line is at `0x02d00000` (region A high).
  Region B `0x02800000` and C `0x01e00000` are documented-free but region A is the trusted one.
- Free L1 trampoline range `0xffa0bfb4`–`0xffa0c000` (kennel ships a plain block here; we use it).
- **Budget: 580,499 cycles/block** (64 samples @ 44.1 kHz). Perf gate: coop audio ≤ 0.75 of that
  (PEAK), fps ≥ 40 idle / 27 loaded. Measured coop callback peaks at 16.0% under a chord — lots of room.

---

## 6. Environment, tools, and exact recipes

**Paths.** Repo: Windows `I:\GitHub\coldsoldr\coop-fw`, WSL `/mnt/i/GitHub/coldsoldr/coop-fw` (the
Bash tool is Git Bash where the drive is `/i`; inside `wsl -e bash -lc '...'` it is `/mnt/i`). Toolchain:
WSL `~/op1-toolchain/bfin/bin` (bfin-elf GCC 14.2 + sim). Scratchpad (session-specific, gitignored
intermediates): the session's scratchpad dir — a fresh session gets its own; rebuild artifacts there.
Python tools are stdlib-only, run with `python3 -I`.

**Build firmware (WSL):** `make -C core/sdram combined` builds `build/core/coop_combined.elf` — the m1
whole-callback timer (CoopAudioCb) + master suite (coop_master_apply) + FX slot, one OP1X header. Read
symbol addresses with `bfin-elf-nm` (they shift when the image changes). `make host-test` (18 python +
C++ unit tests), `make sim-test` (verifies the fractional MAC on the bfin simulator, bit-exact).

**Image pipeline (plaintext for op1emu):** from the stock encrypted LDR
`<scratchpad>/dump_build/OP1_vdk.ldr`:
```
python3 -I tools/inject_app.py  OP1_vdk.ldr out.ldr build/core/coop_combined.elf   # region A + imm seam
python3 -I tools/l1_decrypt.py  decrypt-ldr out.ldr <KEY> out_plain.ldr --patches third_party/kennel/synth_sdk/l1_patches.json
python3 -I tools/l1_insert.py   out_plain.ldr out_ins.ldr --imm-pair-site 0xffa017a4 --imm-stock 0x019d90e8 --fn <coop_master_apply addr>
```
(The old `--call-site 0xffa017ae --call-stock 0xffa04280` replace mode silences the codec: §3 correction.
Hear the result without a GUI: `op1emu ... --headless --deterministic --audio-dump out.wav --input-script cap.txt`.)
OTP/XTEA key (page 0xD0, same on every unit; from the public Tolsi/op1dumps reference). **NEVER write
the key into a committed file.** Read the 16 raw bytes at byte offset 3328 of `~/op1emu-work/otp.bin`
at run time (`xxd -s 3328 -l 16 -p ~/op1emu-work/otp.bin`) and pass them as `<KEY>` to `decrypt-ldr`;
`--patches` auto-picks the byte arrangement. The key lives only in the gitignored op1emu work tree. `l1_insert.py` is REPLACE-only (augment mode
was tried and removed: it hangs because `0xffa04280` is sensitive to the caller stack frame, and
replace is the correct design for that stage anyway).

**op1emu (MIT emulator).** Source + builds at `~/op1emu-work/op1emu` (needs LLVM-15 + g++-10; set up by
`tools/setup_op1emu.sh`). `build/op1emu` (GUI), `build-prof/op1emu` (profiling, `-DENABLE_PROFILING=ON`).
Runs from its own dir (reads `gui/ui.json` + `otp.bin` from cwd). NAND: keep `~/op1emu-work/pristine.bin`
pristine (sha `d2ee748e97ebb3ce3acfd05bb09f3c5737c088d5165a76a48646b1cd6c626fa8`); **copy it to a run
NAND before every run** (op1emu mutates NAND via YAFFS writeback). Marks/probe files need
`"nand_sha256"` matching the run NAND.

Headless + profiling run:
```
cp pristine.bin run_nand.bin
build-prof/op1emu run_nand.bin IMAGE.ldr --headless --deterministic \
  --profile-marks MARKS.json --profile-probes --profile-trace OUT.json --input-script SCRIPT.txt
```
Marks/probe JSON: `{"nand_sha256":"...","marks":[{"name","lo","hi"}],"probes":[{"name","pc","mem":[{"name","addr"}]}]}`.
A probe fires on PC execution and reads its `mem` words into the event's args. **Gotchas (hard-won):**
probe names must avoid reserved words; **max 16 mem words per probe**; **max 262,144 probe hits total
(not raisable)**; **boot needs ≥20 s guest time before the audio callback runs** (use `wait 21000`
after `wait-frames 1 300000`). Input-script commands: `accel ax ay az`, `wait-frames N TIMEOUT`,
`wait MS`, `tap KEY [MS]`, `press KEY`, `release KEY`, `screenshot PATH.ppm`, `keys`, `quit`. Key names:
notes `c4 c4# d4 ... c5`, buttons `synth drum tape mixer seq t1..t4 shift help lift drop split rec play
stop left right enc1..enc4 s1..s8`.

**Hearing it.** The GUI (`tests/sim/play.sh`) now has sound: PulseAudio refused because WSLg's server
is uid 1000 and the distro runs as root, and `PULSE_SERVER` named a missing socket; `play.sh` runs
op1emu as uid 1000 on the live socket (docs/emulator.md "Sound from the GUI on WSLg"). For exact A/B
comparisons still render a WAV offline:
capture d34/db4 over a scenario (probe at `0xffa04804` reading 64 L samples from `0xff800d34` as 32
words + 64 R from `0xff800db4`; split into FOUR probes of 16 words each at the same PC because of the
16-word cap), reconstruct a 44.1 kHz stereo WAV in Python (`struct`+`wave`), copy to
`C:\Users\byron\Downloads\`. This produced the milestone-B echo A/B that the owner confirmed. The GUI
launcher is `tests/sim/play.sh [LDR]` (mouse-driven; volume knob defaults mid).

**Verify DSP on the device target:** `make sim-test` runs `tests/sim/fr_check.cc` on the bfin simulator
(`bfin-elf-run`, needs `-msim`). Use this pattern to prove any new fixed-point matches its int64
reference bit-for-bit.

---

## 7. File inventory (coop-fw)

- `core/include/`: `coop_fixed.h` (Q15/Q31 + `coop_mul_fr` fractional MAC), `coop_dsp.h` (pan helpers),
  `coop_master.h` (comp→drive→EQ suite), `coop_fx.h` (stereo feedback delay FX slot), `coop_voice.h`
  (NEW: equal-power pan, spread, stereo mix), `fw_addrs.h`.
- `core/sdram/`: `coop_cb.cc` (m1 callback wrapper, imm-seam patch), `master_suite.cc`
  (`coop_master_apply`: widen d34/db4 int16→Q31, FX slot then suite, narrow back; FX delay line at
  `0x02d00000`), `coop_cb.ld`, `Makefile` (targets `all`, `combined`).
- `tools/` (stdlib Python 3.8): `inject_app.py` (`--region`), `l1_decrypt.py` (xtea; `decrypt-ldr`,
  `encrypt-ldr`, `check-key`, `--patches`), `l1_insert.py` (trampoline + CALL retarget, replace-only),
  `ldr_dump.py`, `patch_app.py`, `build_package.py`, `verify_bootloader.py`, `pkg_check.py`,
  `mk_otp_bin.py`, `otp_txt_to_bin.py`, `setup_op1emu.sh`, `dump_l1/` (on-device dumper, optional).
- `tests/host/`: `fixed_test.cc`, `master_test.cc`, `fx_test.cc`, `voice_test.cc` (all pass).
  `tests/sim/`: `fr_check.cc` + `run_fr_check.sh` (MAC verify), `play.sh` (GUI), `boot_op1emu.sh`.
- `symbols/`: `l1.sym` (corrected master-bus labels), `sdram.sym`.
- `docs/`: `stereo-bus.md` (C design + plan), `fx-abi.md` (stereo/dual-mono decisions + porting rules),
  `performance.md` (all measurements), `audio-graph.md` (corrected), `l1-map.md`, `phase2.md`,
  `emulator.md`, `brick-safety.md`, `device-tests.md`, this file.
- `third_party/kennel/synth_sdk/l1_patches.json` (XTEA known-pairs for key check).

Never commit: TE firmware bytes, `.op1`/`.ldr`, decrypted L1, the OTP key (`.gitignore` + NOTICE
enforce). Commit attribution lines in use: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` +
`Claude-Session: https://claude.ai/code/session_01QeF9pDwViPuyTxhhhhVbSr`.

---

## 8. Decisions locked (don't re-litigate)

- **Whole insert/master chain is stereo in / stereo out**, insert FX after the per-voice pan (so spread
  survives). **Mono effects hosted dual-mono** (run twice, independent per-channel state). coop FX are
  stereo-native. 3rd-party ports MUST adhere to the rules in `docs/fx-abi.md`. (`stereo-fx-chain-decision`
  memory.)
- **Per-voice pan applies to drums too** (each drum voice pannable + spread).
- Master suite *replaces* the stock master comp (`0xffa04280`) — intended (item 11).
- Fixed-point only, no float/libc, hot loops < 12 KB; prefer the fractional MAC (`coop_mul_fr`) over
  int64; verify new DSP on the bfin sim.

---

## 9. Immediate next actions

1. ~~Build the op1emu write-hook PC-logger~~ DONE (`--watch-writes`; `tests/sim/watch_writes.sh`).
2. ~~Pin the active per-voice-sum and pan-mix PCs~~ DONE (`docs/stereo-bus.md`, `symbols/*.sym`).
3. Implement C1: choose the takeover mechanism (`docs/stereo-bus.md` "C1 takeover mechanism": the
   accumulate is inline code, so either redirect the stock sum via its bus immediates + a vtable render
   wrapper, or overwrite the loop with a trampoline call), pan voices into a stereo bus using
   `coop_voice.h`, decimate both channels to d34/db4, **centered pan matching stock loop B** (regression
   gate — write tracer + rendered-WAV A/B vs stock).
4. C2: wire the spread control to a real parameter (+ UI later). C3: stereo engines/FX.
5. Separately (small win any time): wire real parameters into the master suite so it stops being
   transparent (comp/drive/EQ from `0xff801880`/`0xff8017cc`, add a drive page).
