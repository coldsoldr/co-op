# Performance: the no-compromise constraint

coop-fw must never compromise performance; the UI must always feel smooth (no-compromise item 8).
This is a hard gate on every milestone, not an afterthought.

## The budget

- Audio period: **580,499 cycles/block** (64 samples at 44.1 kHz, 1.45 ms), measured (kennel `kn_shed.h`).
- **Ceiling: coop audio stays under 0.75 of that (~435,000 cycles/block).** The budget is per-block
  PEAK, not average: one note-on block can take 29-75% of a period, and on overload the firmware
  silently drops blocks (pops, pumping, cut tails).
- **UI floor: >= 40 fps idle, >= 27 fps under load.** The UI thread starves first (stock falls
  47 -> 27 -> 13 fps as audio load rises), so fps is the smoothness signal.

## Rules for every DSP stage

- Fixed-point only (Q15, `core/include/coop_fixed.h`), no float, no libc, no divides in hot loops.
- Hot code small: under ~12 KB fits the 16 KB I-cache; keep two heavy stages from evicting each other.
- Prefer trampolining plain SDRAM graph functions (cheap) over rewrites; L1-resident kernels only
  where the per-sample cost demands it.
- Reuse kennel's `kn_shed` governor only as a last-resort safety net, never as the normal path.

## How we measure each milestone (no guessing)

1. **Our callback times the block.** `core/sdram/coop_cb.cc` records per-block cost of the audio
   callback in `g_last`/`g_max`/`g_blocks` (region A, `0x02c000a0..a8`) via the `CYCLES` counter.
   A milestone wraps or extends this, so its cost is measured directly.
2. **op1emu gives exact guest cycles.** Built with `-DENABLE_PROFILING=ON` and run with
   `--deterministic`, op1emu's `--profile-marks` report cumulative guest `cycles=` at named guest-PC
   ranges (`tools/op1prof`), and `--profile-probes` can read guest memory (e.g. `g_max`) at a block
   entry. Two marks around the audio callback difference to its per-block guest cycles, deterministic
   and repeatable. This is the gate's number.
3. **A milestone that exceeds the 0.75 ceiling, or drops fps below the floor, is a no-go** until it is
   optimised or its quality is deliberately cut. Record the figure in `docs/device-tests.md` /
   `docs/phase2.md` per milestone.

Final confirmation of smoothness (and sound) is on the device; the emulator is the fast gate.

## Measured: milestone A (master suite), transparent, 2026-11-01

First real measurement via the profiling op1emu (`--profile-probes` reading our counters at the audio
callback entry, `--deterministic`):

| Metric | Value |
|---|---|
| Blocks our suite ran | 27,504 (0 -> 27,503) — runs every audio block |
| Peak cost / block | 33,827 cycles = **5.8% of the 580,499-cycle budget** |

Under the 0.75 ceiling with room. But 5.8% is high for a transparent pass (copy in/out + env follower
+ bypass biquad on 64x2 samples): the biquad and Q31 multiplies used int64 math, which GCC lowered to
libgcc 64-bit multiplies (a call plus ~50-100 cycles each) instead of the Blackfin's native fractional
MAC. **Done (see next section):** the biquad and Q31 mul were rewritten to the hardware fractional MAC.

## Fractional-MAC rewrite of the DSP primitives, 2026-11-01

`coop_fixed.h`'s `coop_mul_q31`/`coop_mul_qn` and `coop_master.h`'s biquad were moved off int64
multiplies onto the Blackfin 1.31 fractional MAC (`__builtin_bfin_mult_fr1x32x32`, `coop_mul_fr`). The
host build keeps a bit-identical int64 reference, so the host tests still validate exactly what runs on
the device. `coop_mul_qn` and the biquad rescale the MAC result by the extra fractional bits
(coefficients stay Q28, `|coef| < 8`).

Codegen (bfin-elf-g++ -O2 -mcpu=bf524):

| Function | Before | After |
|---|---|---|
| `coop_mul_q31` | 35 instr + libgcc call | 7 instr, no call |
| `coop_biquad_sample` | 128 instr + 5 libgcc calls | 86 instr, no calls |

Verified bit-exact on the Blackfin simulator (`make sim-test`, `tests/sim/fr_check.cc`): 200,049
multiply vectors with 0 mismatches vs the int64 reference, biquad max difference 0 LSB over 100,000
samples, and `(-1)*(-1)` saturates to `0x7fffffff`.

Effect on the transparent master suite (same op1emu method, pristine NAND per run):

| Metric | int64 | MAC | 
|---|---|---|
| Suite peak / block | 33,827 (5.8%) | **18,546 (3.2%)** |
| Whole coop callback, idle | 62,718 (10.8%) | 47,387 (8.2%) |
| Whole coop callback, chord | 95,366 (16.4%) | 80,044 (13.8%) |
| Net added over stock, idle | +20,770 (+3.6%) | **+5,439 (+0.9%)** |
| Net added over stock, chord | +21,081 (+3.6%) | **+5,759 (+1.0%)** |

The suite's own cost dropped 45%, and the coop callback's overhead over stock fell from ~3.6% to ~1%
of budget. The suite cost is load-independent (fixed 64-sample loop). The remaining ~18.5k cycles are
the block copy in/out, the compressor envelope follower, and the (still transparent) comp/EQ math;
processing the master pair in place would trim the copy further if ever needed. Headroom is now very
large for the FX, stereo-bus and LFO milestones. (The compressor's one per-sample divide, taken only
above threshold, is the next int64/libgcc item to replace with a reciprocal when real params land.)

## Baseline: stock callback vs. coop callback, 2026-11-01

The user asked for the right comparison: measure the stock audio callback's cost first, then measure
the coop additions relative to it, under the same load. Both measured with the profiling op1emu
(`--deterministic`, `--profile-probes` reading our counters at the audio-callback entry `0xffa04804`).
A fresh NAND is restored before each run (pristine snapshot `sha256 d2ee748e...`) so runs are
comparable.

- **Stock** = the m1 wrapper (`coop_cb.cc`) times the whole stock callback `0xFFA04804`, nothing else
  changed.
- **Coop** = the combined image (`make -C core/sdram combined`): the same whole-callback timer, plus
  the master suite wired in by retargeting the stock master-EQ call (`0xffa017ae`, was calling the
  stock EQ `0xffa04280`) to `coop_master_apply`. So the suite *replaces* the stock master EQ with
  comp -> drive -> EQ; the delta already nets out the displaced stock EQ.
- **Load:** idle at the synth screen, vs. a held three-note chord (c4+e4+g4) rendering voices.

| Scenario | Stock callback | Coop callback | Net added by coop | Suite's own cost |
|---|---|---|---|---|
| Idle          | 41,948 (7.2%)  | 62,718 (10.8%) | +20,770 (+3.6%) | 33,827 (5.8%) |
| Synth chord   | 74,285 (12.8%) | 95,366 (16.4%) | +21,081 (+3.6%) | 33,827 (5.8%) |

(peak cycles/block; % of the 580,499-cycle budget.)

Findings:
- The coop master suite adds a steady **~3.6% of budget** on top of stock, the same whether idle or
  playing (its 64x2-sample loop is load-independent). Net of the stock EQ it displaces, the suite's
  raw self-timed cost is 5.8%, so the stock master EQ was ~2.2%.
- The whole coop callback peaks at **16.4%** under a chord — far under the 0.75 (75%) ceiling. There
  is ample headroom for the remaining milestones (FX x2, stereo bus, LFO x2) before the governor is
  ever needed.
- The int64 biquad/Q31 MAC rewrite (previous section) is still worth doing for margin, but is no
  longer urgent: even untouched, the master suite is a small fraction of the budget.

Reproduce: `make -C core/sdram combined`, then inject/decrypt-ldr/l1_insert (fn `coop_master_apply`,
call-site `0xffa017ae`, stock `0xffa04280`), run under op1emu with probes on `g_max 0x02c01144` (whole
callback) and `g_master_cycles 0x02c0114c` (suite only). Addresses come from `bfin-elf-nm` on the
combined ELF and shift if the image changes.

## Milestone B: second FX slot (coop stereo delay), 2026-11-01

A coop stereo feedback delay (`core/include/coop_fx.h`, host-tested in `tests/host/fx_test.cc`) runs
as a second master insert slot, in series before the master suite, on the real master bus. Its delay
line (64 KiB/channel) lives high in free SDRAM region A (`0x02d00000`).

**Buffer correction found during this milestone:** the master suite and FX were first written for
`e34`/`f34` (`0xff800e34`/`f34`), which op1emu probing showed are **always zero** (that is the
USB-audio bus). The live codec/speaker master bus is **`d34`/`db4` (`0xff800d34`/`0xff800db4`, int16,
64 samples)**. Milestone A's transparent boot only proved "no crash," not signal flow; it was
processing silence. The suite + FX now widen d34/db4 int16 -> Q31, process, and narrow back. See
`docs/audio-graph.md` and `symbols/l1.sym`.

Validation (op1emu, pristine NAND per run): at idle the delay line stays zero (no audio to delay);
during a held chord it fills (20,976 of 33,809 samples non-zero) -- i.e. the FX is reading and
processing the live master audio. A mute build (write 0 to d34/db4) left d34/db4 fully zero with no
later overwrite, confirming the coop insert owns the master-bus content. Final audible confirmation is
a device test (the owner flashes).

Cost (peak cycles/block, % of the 580,499-cycle budget), replace-mode insert at `0xffa017ae`:

| Stage | Idle | Chord |
|---|---|---|
| FX slot (stereo delay) | 10,107 (1.7%) | 11,131 (1.9%) |
| Master suite (comp/drive/EQ) | 17,083 (2.9%) | 18,991 (3.3%) |
| Whole coop callback | 57,205 (9.9%) | 92,628 (16.0%) |

Two coop stages (delay + suite) add ~5% of budget over stock; the whole coop callback peaks at 16.0%
under a chord, far under the 0.75 (75%) ceiling. The insert *replaces* the stock master
compressor/level stage (`0xffa04280`, the comp DSP on `0xff801880`), which is exactly item 11 (the
coop master suite takes over master comp/drive/EQ); it writes only comp state, not meters, so memorize
is unaffected.

## Milestone C1: stereo instrument bus (op1emu CYCLES, 2026-11-02)

Measured on the full-spread test image (`g_coop_spread=32767`, 3-voice chord, `tests/sim/det_chord.txt`)
by watching the coop counters (`g_stereo_hook_cycles_max`, `g_stereo_mix_cycles_max`) with the write
tracer; op1emu's CYCLES is an instruction-count model, so treat these as relative until a device test.

| Stage | Peak cycles/block | % of budget |
|---|---|---|
| Voice hook, one panned voice (128-sample difference accumulate) | 2,552 | 0.44% (x6 synth = 2.6%, x12 drums = 5.3%) |
| Mix replacement, mono fast path (no panned voice) | 1,567 | 0.27% |
| Mix replacement, full path (2 x decimate + 2 x DC block + stereo mix) | 15,711 | 2.7% |

At spread 0 the hooks return after a table lookup and only the mono fast path runs: the coop image is
sample-identical to stock and costs ~0.3% of the budget. Worst case (12 panned drum voices + full mix)
is ~8%, inside the ceiling; the per-voice accumulate is the first candidate for a vector-MAC rewrite if a
device measurement says so.

## Phase 3: second LFO slot (op1emu CYCLES, 2026-11-02)

| Stage | Peak cycles/block | % of budget |
|---|---|---|
| LFO2 process, stock tremolo class, one voice | 6,974 | 1.2% (x6 synth = 7.2%, x12 drums = 14.4%) |

This is the stock LFO class's own cost (slot 1 pays the same); slot 2 off costs a few loads per voice.
Worst case (stereo bus full + 12 drum LFO2 voices) stays near 22% of the budget.

## Phase 3: stereo tape, T3c image (op1emu CYCLES, 2026-11-02)

Whole-callback peak (`g_max`) through `tests/sim/det_tape_rec.txt` (a chord recorded from the synth page, then
played back), three images from the same ELF; the stock chord figure is the 2026-11-01 baseline above.

| Image | Peak cycles/block | % of budget |
|---|---|---|
| Stock, chord (baseline) | 74,285 | 12.8% |
| Coop stages, spread 0, no tape stereo | 89,900 | 15.5% |
| + stereo tape, spread 0 | 100,222 | 17.3% |
| + full spread (the device package) | 122,376 | 21.1% |

Stage peaks in the full image: record tap (two stock record passes + ring/state copies) 13,530 (2.3%), mix
replacement full path 16,107 (2.8%), voice hook 2,554 per panned voice. Main-loop I/O step (coop_tape_io_step,
instruction-count model, no NAND time): a node load (4 x 128 KiB reads + de-interleave) 811-857k cycles,
a chunk flush (interleave + 128 KiB write) 419k. Stock's step moves 4 x 64 KiB per node load and 64 KiB per
flush, but flushes the chunk being recorded on every pass; coop holds a saved node for 1 s.

The owner reports the tape animation goes choppy while a chord is recorded (T3c, device). The emulator cannot
show the two device-only costs: instruction-cache misses on coop's SDRAM code (every coop stage runs cold each
block after the main loop evicts it; ~100-190 cycles a line) and NAND time inside the I/O step. Hence:

### The device stats file (T3d)

`core/sdram/coop_stats.cc` writes its text from the main loop (the tape I/O step) every 5 s while nothing is
recording: cumulative peaks and a ring of the last twelve 5-second windows. Since T3g it goes into the last
4 KiB of track 4's audio in the tape image, the one channel disk mode copies (`tape/track_4.aif`, read with
`tools/tape_tail_stats.py`); the yaffs copy `/yaffs2/user/coop_stats.txt` is **not visible in disk mode** (see
T3g). Lines are `name value` pairs:

- `cb blocks/max/last`: the whole coop audio callback (CYCLES, device time including cache misses); the
  ceiling is 435,000 (0.75 of a block).
- `stages`: per-stage peaks (`rec_max` record tap, `mix_max` mix replacement, `hook_max` voice hook, `lfo2_max`,
  `master_max`, `fx_max`) and the cumulative voice-hook calls / side-bus blocks (T3f).
- `tape`: record calls and the two state-mismatch detectors of the right-lane pass (`mismatch*`, must stay 0);
  `io`: loads / flushes / held / errors and the step / gap / load / flush peaks; `edits`: lift / drop / erase counts;
  `stats`: writes / errors / windows / ticks (main-loop passes seen).
- `win ...`: one line per 5-second window, oldest first (format 2, T3f): `blocks` (audio blocks in the window),
  `cb_max`, `cb_sum` (the callback's cycles summed over the window: `cb_sum / (blocks x 580,499)` is the
  average audio load), `cb_over` (blocks over half the budget), `rec_max`, `mix_max`, `hooks` (voice-hook
  calls: how many panned voices rendered), `side` (record-tap blocks with a live side bus), `recs` (record
  calls), `io_steps` (main-loop passes in the window: the UI rate's ceiling), `io_step_max`, `io_gap_max` (the
  longest interval from the end of one I/O step to the start of the next: the rest of a main-loop pass, i.e.
  the UI's own work plus the interrupts that landed in it; a choppy animation shows here or in `io_step_max`),
  `io_load_max`, `io_flush_max`, `loads`, `flushes`, `recording` (1 if a take ran in the window).

Reading: a main-thread interval that an audio interrupt lands in includes that interrupt, so an `io_step_max`
near `cb_max` is the interrupt, not the step (emulator idle: step 47,625 = callback 45,900 + the walk). The
window lines with `recording 1` are the ones to compare against the idle windows: the `cb_max` difference
is the audio-side cost of a recorded chord, the `io_steps` drop and `io_load_max` / `io_flush_max` the
main-thread side. Verified in op1emu (T3d): 20 writes, 0 errors, playback unchanged.

### T3e: node loads in eight parts (2026-11-02)

The I/O step loaded a node in one main-loop pass: 4 tracks x 128 KiB of file plus the de-interleave, twice
the bytes stock moves per pass (4 x 64 KiB) and the longest single block of main-thread work coop added. It now
loads one part per pass: half a track-chunk, 64 KiB of file plus 16,384 frames de-interleaved, the same read
size as stock's step. The lookup requests the next chunk 0.74 s ahead and eight passes take about 60 ms, so
the node is ready long before the head reaches it; the node stays invalid until its last part, the stock cancel
protocol is honoured at the end, a node whose chunk changed under us restarts from part 0, and a node in
progress is finished before another is started. Flushes are unchanged (128 KiB per dirty track, held to once a
second per node while recording, all at once when the take ends; stock writes the recorded chunk on every pass).

Verified in op1emu against the previous image: the record scenario and the power-cycle playback have the same
level, peak and width (sample positions shift by one block as the main-loop timing changed); a 4-second take
across six chunk boundaries (`tests/sim/det_tape_rec_long_quit.txt`, then `det_tape_play_start.txt` from the
file) plays back continuously with the same envelope (within 1%) and width 0.25-0.27 on both images. In the
stats file `io_load_max` is now per part.

### T3f: the stats file names its build and measures the main loop's gaps (2026-11-02)

The owner's T3e-era report: no `coop_stats.txt` in the disk root (the file lives under `user/` on the disk-mode drive, as `coop_l1.bin` did; the root was the wrong place to look) (which package was on the device for that
run is not known: T3c has no writer), and the lag is specific to voices: recording with nothing playing keeps
the tape animation smooth, and it starts lagging as soon as voices play. The cycle counter is not the reason
for a missing file: the Rings probe in `../te-op1-cfw` read 399 MHz from CYCLES on this device without
enabling it, and kennel's held saves and crash marker depend on it. So T3f makes the file unambiguous and
makes it answer the voices question directly:

- The first line is `coop_stats 2 build t3f`, and the file is also written once after 64 main-loop passes
  (the L1 dumper's proven point), so any boot that lasts a few seconds leaves a file naming the build; a
  missing file after that means the flash, not the window logic.
- Per window: `cb_sum` and `cb_over` (average load and the number of heavy blocks, not just the peak),
  `hooks` / `side` / `recs` (were voices panned, was the side bus live, was the tape recording), `loads` /
  `flushes` (the main-thread I/O that ran), and `io_gap_max` (the longest stretch of the main loop outside
  our step: the UI's own pass plus the interrupts in it).

What the numbers decide: if the windows with `hooks > 0` show `cb_sum` well above the idle windows (the
emulator says 21% peak; a 2-4x device factor from cold caches, as Rings and Clouds showed, would put the
average above the ~40% at which the Rings probe saw the screen starve), the audio side is the cause and the
levers are the coop code's cache footprint (the combined ELF is 19 KB of text; the per-block hot set must stay
well inside the 16 KB instruction cache, with the main loop evicting it every block) and the per-voice hook.
If instead `io_gap_max` / `io_step_max` grow only in the `recording 1` windows with `flushes > 0`, it is the
128 KiB flush, and flushes get the T3e split treatment. Verified in op1emu (format 2, `tests/sim/det_tape_rec.txt`): 18 writes, 0 errors, 19 windows, the first file at pass 64; audio against the T3e image: same RMS, peak and width (0.2299), the take shifted by one block as the main-loop timing changed.

### T3g: the read-back channel is the tape audio (2026-11-02)

T3f on the device: still no file. The owner's point, which the T3d-T3f packages ignored: **disk mode does not
expose yaffs** [C]. The drive is a FAT the stock disk layer (`src/disk/`) synthesises: `synth/` and `drum/`
from the patch database and `user/audio/...raw`, `tape/` from the tape image (it even appends the clip table
from `tape.db` to each track file: `track_1.aif` came back 264 bytes longer than the empty tracks), `album/`
from the album image. The L1 dumper (Test 64) never ran, so "copy it in disk mode" was never proven; the
`user/` folder does not exist on the drive. Nothing written to `/yaffs2/user/` can be read back that way.

What disk mode does copy unchanged is the tape audio. So `write_file` now also writes the text (NUL padded to
4 KiB) into the last 4 KiB of track 4's audio data (`coop_tape_write_tail`, `core/sdram/tape_io.cc`: track 4,
chunk 484, the final 1024 frames = 23 ms of the tape; audible only if track 4 is played to its very end, and
erased by recording there). `tools/tape_tail_stats.py track_4.aif` prints it. The `stats` line counts
`tail_writes` / `tail_errors`. A tape rebuild (first boot of a new image layout) clears it; the next window
write (5 s) puts it back.

Also from the owner: the lag is worse with the FM and Voltage synth models than with the string model, with
which the animation looks fine. Those engines cost more per voice in stock, so the lag tracks the total audio
load, not the tape I/O: the audio-side theory (the coop stages' device cost on top of the engine's) is the
one the window figures should confirm.

Verified in op1emu (`tests/sim/det_tape_rec.txt`): 19 tail writes, 0 tail errors, audio identical in RMS, peak and
width (0.2307) to the T3f image. `tools/tape_tail_stats.py` was checked on a synthetic track file with the same
chunk layout and clip trailer.
