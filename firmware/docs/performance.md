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

### T3g: the first device numbers (2026-11-02)

`docs/device-stats/t3g_2026-11-02_fm_chord.txt`, read back from `track_4.aif`. Block budget 580,499 cycles;
the device's CYCLES includes cache misses, SDRAM contention and nested interrupts, so these are the real costs.

| Figure | Device | Emulator | Ratio |
|---|---|---|---|
| Audio callback, worst block (`cb max`) | 461,297 (**79%** of a block) | 122,376 (21%) | 3.8x |
| Record tap, worst (`rec_max`) | 48,259 (8.3%) | 13,530 | 3.6x |
| Mix replacement, worst (`mix_max`) | 46,389 (8.0%) | 16,107 | 2.9x |
| Voice hook, worst, one voice (`hook_max`) | 33,710 (5.8%) | 2,554 | **13x** |
| Tape rolling, no voices: average per block | 73-87k (12.6-15%) | 45.9k | 1.6-1.9x |
| Tape stopped: average per block | 6-18k (1-3%) | - | - |
| Node part load, 64 KiB (`load_max`) | 16.0M cycles = 40 ms | 0.22M | NAND |
| Chunk flush, 128 KiB (`flush_max`) | 30.1M cycles = 75 ms | 0.56M | NAND |
| Main-loop passes per 5 s, idle / chord window | ~600 / 371 | 625 | - |
| Longest main-loop gap, idle / chord window | 8-22 ms / **138 ms** | 8 ms | - |

Readings:
- **The audio side is the cause.** A chord with the tape rolling takes the callback to 41% in the one chord
  window the ring kept and to 79% in the worst block of the session (the recorded FM chord, whose windows
  were pushed out of the 12-window ring: 26 windows had passed). The Rings probe saw the screen starve above
  ~40%; the owner sees it here. The string model costs less per voice in stock than FM and Voltage, hence
  the difference by model.
- **The voice hook is the elephant:** up to 33.7k cycles for one voice's 64-sample difference accumulate
  (128 MACs of real work). Six voices of a chord would be ~200k, half the budget. The C loop compiles to ~40
  instructions a sample and every one of its code lines and data lines (side buses, pan table, pan state,
  all in SDRAM) is cold at the first voice of a block; the worst case likely includes a nested interrupt.
  The record tap and mix replacement are 3x the emulator, the expected cold-cache factor.
- **NAND:** a 64 KiB read stalls the main loop 40 ms, a 128 KiB flush 75 ms (both include the interrupts
  that landed in them). The T3e split cut the per-pass load stall from ~160 ms to 40 ms; flushes should be
  split the same way (two 64 KiB halves) so no single pass blocks the UI longer than a stock pass.
- The mismatch detectors stayed 0 through the session; 22 tail writes, 0 errors.
- The stats need two changes: per-stage sums per window (averages, not only maxima) and a second ring of the
  heaviest windows by `cb_max`, so a recording or chord window is never lost to the time ring.

### Milestone T4: fit the stereo stages inside the budget on the device (plan, as written before T4a)

Order, each step measured on the device with the stats:
1. Stats v3: per-stage window sums, a heaviest-windows ring, hook calls per block.
2. The per-voice difference accumulate and the mix full path as Blackfin assembly kernels (dual MAC, a few
   lines of code each, bit-exact against the C versions in op1emu: the spread-0 regression gate is bit-exact
   and the full-spread image must stay sample-identical), with their hot data (side buses 1 KiB, pan state, pan
   table, the record tap's rings and snapshots) in L1 data B: kennel's L1 pool (`kn_l1.c`: the Lander game's
   sound buffers `0xFF90081C-0xFF906908`, ~24 KB, free when the Lander is off, with its table repointed as
   kennel does) or the 512 B at `0xFF907E00`.
3. Flushes in two 64 KiB halves across passes.
4. If still short of the 0.75 ceiling with six FM voices recorded: instruction-cache way locking for the hot
   coop code (one or two 4 KB ways), which keeps it resident across the main loop's UI work.

### T4a: kernels, the L1 pool, split flushes, stats v3 (2026-11-02)

Steps 1-3 of the plan, one package (`op1_246_coop_t4a_stereo_tape.op1`):

- **Kernels.** The per-voice difference accumulate (synth and drum) and the four mix loops (A, A mono, B, B mono)
  are Blackfin inline-assembly kernels in `core/include/coop_stereo.h`: one hardware loop each, the streams through
  DAG registers, 16-bit parameter pairs in register halves, the pointers handed over in a small argument block
  (eight data registers are not enough for every operand as a "d" constraint). The C versions stay as the `_c`
  references and the host build. `tests/sim/kern_check.cc` (`run_kern_check.sh`, GNU simulator) compares every
  kernel against its reference over 3000 random blocks with the edge values in every operand: 0 mismatching blocks.
- **L1 pool** (`core/sdram/l1_pool.cc`, `coop_l1.h`): the stock Lander game's 40 sound buffers in L1 data B
  (`0xFF90081C..0xFF906908`, kennel's `kn_l1.c`), taken only when their table is exactly the stock layout, then the
  table is pointed at a dummy with size 0 (the game's sounds go silent in coop). The stereo stages' buses, decimator
  rings, DC states, pan states and sine table, and the record tap's input, rings, snapshots and lane tables live
  there (2,976 bytes of ~24 KB); every user keeps an SDRAM fallback. `l1 state 1 used 2976` in the stats.
- **Flushes in halves** (`tape_io.cc`): one 64 KiB half per main-loop pass. The request byte is cleared when a
  node's flush starts, the dirty bits stay set over the halves (the stock record sets the request byte on every
  write), and a track's bits are cleared after its second half only if no request arrived meanwhile.
- **Stats v3** (`coop_stats.cc`, `coop_stats 3`): per-window sums for the record tap, mix and hook (averages, not
  only maxima), `hook_max` per window, an `l1` line, and `top` lines: the eight heaviest windows by `cb_max`
  kept for the whole session, so a take or a chord is never pushed out by the time ring (T3g lost the recorded
  chord that way). The tail text in track 4 grows to 8 KiB (46 ms of the tape's end).
- **SysEx file channel** (milestone U1, `core/sdram/coop_sysex.cc`, `tools/coop_sysex.html`): see `docs/sysex.md`.

Emulator (op1emu, CYCLES = instructions): callback peak 117,647 (was 122,410), hook 1,943 (2,554), mix 14,284
(16,112), record tap 13,417 (13,530): the instruction count barely moves, as expected; the device gain is the L1
residency and the smaller code, which only the device can show. Gates (sequential runs, docs/emulator.md): the full-spread T4a image is sample-identical to the full-spread T3g image on the chord scenario (3,491,181 frames from 2 s); the spread-0 tape image was not compared with stock (the tape-stereo image differs from stock on the tape-monitor path by design; the mono kernels are covered by the simulator check and the full-image identity). The record scenario against the
T3g image: same level, peak and width (0.2307), the usual one-block timing shift. Stats: 19 writes, 19 tail
writes, 0 errors; the heaviest-windows ring kept the recording window.

### T4a on the device (2026-11-03)

`docs/device-stats/t4a_2026-11-03_sysex.txt`, read over USB-MIDI while the device ran. Windows are 5 s; "chord"
windows are those with a recorded six-voice FM chord (`hooks` ~6 per block, `recording 1`).

| Figure | T3g | T4a |
|---|---|---|
| Voice hook, worst / average per voice | 33,710 / - | 7,552 / 2,800 cycles |
| Record tap, worst / average per block (chord) | 48,259 / - | 31,800 / 20,000 |
| Mix replacement, worst / average per block (chord) | 46,389 / - | 26,605 / 20,000 |
| Callback, worst block | 461,297 (79%) | 514,697 (89%, the first window after boot) |
| Callback, average in a chord window | - | 49-57% (`cb_over` 3,138 of 3,451 blocks over half the budget) |
| Callback, average with the tape rolling, no voices | 12.6-15% | 15.1% |
| Main-loop passes per 5 s, idle / chord | ~600 / 371 | ~600 / 280-377 |
| Flush, 64 KiB (half) | 75 ms for 128 KiB | 85-173 ms per half under the chord |
| Node part load, 64 KiB | 40 ms | 22-28 ms |
| L1 pool | - | taken, 2,976 bytes, stereo and tape state in L1 |

Reading:
- **The kernels and the L1 move did what they should**: the hook is 4.5x cheaper at its worst and 2,800 cycles
  on average per voice; the mix and the record tap each average about 20k per block. Coop's own stages under a
  recorded six-voice FM chord: hooks ~14k + mix 20k + record tap ~13k incremental = about 47k, **8% of the block**.
- **The load is the stock engine's.** The callback averages 49-57% in those windows, so about 45% is the stock FM
  engine with six voices plus the tape path (the eight-lane mix loops are in that figure). The emulator's 13% for
  a stock chord is an instruction count; the device pays cache misses on the engine's own data.
- **The visible hitch is the flush under that load.** A 64 KiB yaffs write took 85-173 ms per pass with the audio
  at 55% (35 ms when idle): it is CPU work (page copies and ECC in software), so it stretches by the audio's share.
  Recording silence stays smooth because the audio is at 15% then. The remedy is smaller parts per pass (T4b),
  not a faster write.
- Two main-loop gaps of 0.9-1.4 s outside our step (the window after boot and the one after a take ended) are
  stock work: the tape database save when a recording stops.
- Mismatch counters 0; 15 writes, 15 tail writes, 0 errors; 3 lifts.

### T4b: loads and flushes in 16 KiB parts (2026-11-03)

`PARTS_PER_TRACK 4` in `tape_io.cc`: a node loads in 16 passes of 16 KiB, a track-chunk flushes in 4 passes of 16 KiB
with the same request/dirty protocol as the halves. Expected on the device: the longest stall per pass under a
heavy chord about 45 ms instead of 173 ms, at the cost of more passes per chunk (a node load still completes in
~16 passes, well inside the 0.74 s the lookup requests ahead). Verified in op1emu: the record scenario has the same level, peak and width as T4a (the usual one-block timing shift); the 4-second take across six chunk boundaries, recorded, power-cycled and played from the file, matches the T3g run in RMS and width (0.2316), with 25,166 samples differing by at most 13 LSB in one 1.4 s span (-68 dB; the parts land at different moments against the record head).

### T4b on the device (2026-11-03)

`docs/device-stats/t4b_2026-11-03_sysex.txt`. The 16 KiB parts did what they should on the main loop; the audio side
is as on T4a.

| Figure | T4a | T4b |
|---|---|---|
| Longest I/O step per pass under the recorded FM chord | 85-173 ms (64 KiB flush) | 26 ms, one 53 ms (16 KiB) |
| Longest main-loop gap outside our step, chord windows | 17-48 ms | 17-26 ms |
| Node part load | 22-28 ms (64 KiB) | 19-20 ms (16 KiB) |
| Main-loop passes per 5 s, chord windows | 280-377 | 387-448 |
| Callback average, recorded FM chord | 49-57% | 51-55% |
| Callback average, FM chord sounding, tape rolling, not recording | - | 51% |
| Callback worst block | 89% | 106% (one block over the budget) |
| Coop stages per block, chord (hooks + mix + record tap) | ~47k (8%) | ~60k (10%: hooks 17k, mix 21k, record tap 22k) |

So the remaining load is the stock engine's: about 41-45% of the block for six FM voices with the tape rolling.
The owner's observation (FM and Voltage heavy, string fine) points at a specific stock cost: `OP1_SINE_TABLE`,
8192 x int16 at `0x0101E5E8` in SDRAM, which the FM operators read once per operator per sample (6 voices x 4
operators x 128 samples = ~3,000 scattered lookups a block through a 16 KB data cache shared with everything
else). The string model is a physical model and reads no such table.

### T4c: the stock sine table in L1 (2026-11-03)

`l1_pool.cc`: `coop_l1_sine_copy` copies the 16 KiB table to `0xFF902900..0xFF906900` (the upper part of the Lander
buffers; the bump pool now ends at `0xFF902900`), once, from `coop_stereo_block_start` before the stock callback's
first block and from the tape mount hook at boot. `tools/l1_stereo_insert.py` (`sine_patches`) scans every loaded
block for `Px.L = 0xe5e8; Px.H = 0x0101` and retargets the pair at the copy: 27 references (26 in SDRAM code, one in
L1 code at `0xffa04bc2`), refusing the image if the count differs. The L1 copy is unconditional (it does not depend
on the pool check); with kennel's audit (`kn_l1.h`: the range is read by one Lander function only, through its
table, never written, never a DMA target) the only effect on the game is silent sound effects. `l1 ... sine 1` in
the stats. Verified in op1emu (sequential gates): the T4c full image is sample-identical to T4b on the chord scenario (3,407,390 frames from 2 s: every retargeted reader finds the copy in place), and the record scenario matches in level, peak and width.

### T4c on the device (2026-11-03)

`docs/device-stats/t4c_2026-11-03_sysex.txt`; the owner recorded chords with FM, then Voltage, then DSynth, so the
chord windows map to the engines in that order (seq 6 FM, seq 12-14 Voltage, seq 18-20 DSynth; six voices where
`hooks` is ~20,700 a window, four where ~13,800).

| Engine, six-voice chord recorded | T4b | T4c |
|---|---|---|
| FM, callback average | 51-55% | **38%** |
| Voltage, callback average | - | 50-64%, one block at 105% |
| DSynth, callback average | - | 47-59% (four voices) |
| Coop stages (hooks + mix + record tap) | ~60k (10%) | ~55k (9.5%) |
| Longest I/O step per pass | 26-53 ms | 19-52 ms |

The sine table in L1 took about 15 points off the FM chord: that was the engine's miss cost, as suspected. Voltage
and DSynth read other things per sample (their own waveforms or tables in SDRAM), and the pool has about 5 KB
left below the sine copy, so moving theirs needs first finding them and their sizes (the op1emu profiler build
can point at the hot engine code, then its pointer constants name the tables). Two blocks of 2.1 M cycles (3.7
blocks, `cb max`) sit at the engine switches, stock patch-loading work inside the callback.

### T4d: the Lander game unreachable (2026-11-03)

`core/sdram/lander_off.cc`: the KeyHandler dispatch (`CALL 0x019B6C74` at `0x019C2D3E`) goes through
`coop_key_event`, which swallows the fourth mode key when synth + drum + tape + mixer are all held (kennel's
`key_event_hook` rule) and passes everything else on. Kennel's stub screen object (`lander_stub`) was ported first
and hung the stock boot in op1emu 0.7 s after it ran (kennel's recipe leans on its other screen hooks), so the game's
screen is still created at boot; it cannot be entered, and its sound table points at size-0 dummies.

### T4e: the decimator pair as one dual-MAC kernel (2026-11-03)

`coop_hb2_state` / `coop_hb_decimate2` in `coop_stereo.h`: the left and right rings interleaved (L in the low half
of each word, R in the high half), so the 32-tap even-phase FIR runs once for both channels with `A0 += R0.L *
R2.L, A1 += R0.H * R2.L (W32)` and a 32-bit ring load per tap. Each accumulator does exactly the MAC sequence of
`coop_hb_fir`, so the output is bit-identical to two `coop_hb_decimate` calls with the same ramp and step
(`tests/sim/kern_check.cc`, `hb_pair 0` over 3000 blocks). The state lives in the L1 pool like the rest.
Emulator: the mix stage peak 11,387 cycles (14,284 before: the pair saves a fifth of the instructions; the device gain is a line load per tap instead of two). Gates (one sequential job, docs/emulator.md): the asm pair and its C path give sample-identical images; against T4d the stream is the same shifted by one audio block (the onset lands on the next block boundary; aligned on the onset the residual is at most 2 LSB, the DC blocker settling against the shifted block), and two runs of one image are identical, so the shift is the image timing, not the kernel. Record scenario: same level, peak and width as T4c.

### DSynth is compute-bound (emulator, 2026-11-03)

Why Voltage and DSynth did not move with the sine table in L1 (T4c). With DSynth selected in op1emu (sound slot 5
of the factory NAND, `tests/sim/det_dsynth_stats.txt`), a three-voice chord takes the emulated callback from 7.5%
(tape stopped, no voices) to **24.9%** average: about 34k instructions per voice per block (263 per sample), with
coop's stages at ~3%. The write tracer puts DSynth's render loop at `0x0197e648..0x0197ebb0` (SDRAM): two
oscillator streams through I1/I2 into the voice buffer, the sine table (now in L1) and three small ROM tables
(`0x0106a730`, `0x0106b3cc`, `0x0106b5cc`, a few hundred bytes to 2 KB, cache-resident after the first touch).
There is no large table left to move: the device's 47-59% at four to six voices is mostly the engine's own
arithmetic (device/emulator about 1.5-1.8x, the usual cold-cache factor on its 4 KB of SDRAM code and per-voice
state), and stock firmware pays the same minus coop's ~10%.

So the DSynth hitch is the stock engine's CPU share starving the main loop, with coop's 10% on top. What coop can
still do: its own 10% (the second record pass while the tape rolls without recording, ~1.7%; small trims), and the
I/O parts are already at 16 KiB. Instruction-cache way locking would keep DSynth's 4 KB of render code resident
across the main loop (one way of the 16 KB cache), worth at most the per-block refill, ~3-4%; it is the last lever
and the riskiest, so it waits for the owner's verdict on T4e.

### T4f: 8 KiB I/O parts (2026-11-03)

`PARTS_PER_TRACK 8` in `tape_io.cc`: a node loads in 32 passes of 8 KiB, a track-chunk flushes in 8. On T4c the
16 KiB parts took 19-52 ms per pass under a DSynth chord (the yaffs work stretches with the audio's share); 8 KiB
halves that. A node load now takes 32 passes, about 0.3 s at the idle pass rate and 0.45 s under a heavy chord,
still inside the 0.74 s the lookup requests ahead. Verified in op1emu: the record scenario matches T4e in level, peak and width (onset-aligned residual at most 9 LSB, the usual timing shift), and the 4-second take across six chunk boundaries, power-cycled and played from the file, matches the T4b run within 1 LSB.

### T4f on the device: where the lag sits now (2026-11-03)

`docs/device-stats/t4f_2026-11-03_sysex.txt`. FM is smooth while recording; a six-voice DSynth chord recorded is
"a bit laggy, not unbearable" (the owner).

| Recorded six-voice DSynth chord | T4c (16 KiB parts) | T4f (8 KiB parts) |
|---|---|---|
| Callback average / worst | 59% / 105% | 65-67% / 85% |
| Coop stages per block | ~55k (9.5%) | ~50k (8.6%) |
| Stock engine + tape | ~50% | ~57% |
| Longest I/O step per pass | 19-52 ms | 25-44 ms |
| Longest main-loop gap outside our step | 17-49 ms | 19-53 ms |
| Main-loop passes per second | 76 | 81 |

The stock DSynth engine with six voices takes about 57% of the block on its own (the 65% here minus coop's 8.6%;
the difference to T4c is the take itself, more voices sounding at once), so the main loop gets a third of the
CPU and a UI pass that costs 15 ms of work stretches to 45. That is the hitch. Stock firmware is in the same
regime nine points lower. The I/O parts no longer matter at 8 KiB: the longest gaps are now the UI's own passes.

Coop's remaining levers against this: the second record pass while rolling without recording (nothing while
recording), small trims (under 1% each), and instruction-cache way locking for the hot code (one or two 4 KB
ways of the 16 KB cache; 3-4% of the block at best, and the riskiest change in the project: the cache controller
written from the ISR in a documented sequence, untestable in the emulator, which has no cache). Everything else
is the stock engine's arithmetic. The performance work stops here unless the owner wants the way-locking
experiment.

## The DSynth lag and the rewrite programme (2026-11-04)

The owner on `p3a`: DSynth made the UI unbearably laggy, worst with cwo. The `p3m` cost line (device, six-voice
DSynth chord, block budget 580,499): slot 1's stock effects in their stereo class cwo 92 k, punch 77 k, spring 45 k,
phone 43 k, nitro 44 k, grid 32 k, delay 24 k per block; FX 2 the same again; coop's S3 stage 15 k; the six-voice
chord with no effect about 350 k (T4: about 300 k). [C: `coop_stats (6).txt`]

SilverTape beta 2 (a third-party R246 mod, analysed in `incoming/silvertape/`, nothing copied) replaces DSynth's
per-voice output loop, a software loop with four accessor CALLs per sample. cwo's 64-sample loop has the same
pattern with five CALLs per sample (not touched by SilverTape). Programme, each step A/B'd in op1emu (identical
samples) and measured:
1. **DSynth voice loop** (`coop_dsynth_voice_loop`, stereo_thunks.S; patch at `0x0197E956`; switch
   `g_coop_fast_dsynth`, 1 by default). op1emu, six voices (`tests/sim/det_dsynth6.txt`): 229 k -> 185 k emulator
   cycles per block (-44.5 k, -19 % of the callback, about 7.4 k per voice); the samples identical (the faster
   callback moves a key event by one block later in the run, as tape scenarios showed before). [C in op1emu, the
   device figure pending]
2. coop's S3 stage in assembly, its buffers in L1.
3. **cwo's stereo loop** (`coop_cwo_loop`, core/sdram/cwo_fast.S; switch `g_coop_fast_cwo`, 1 by default). The
   stereo render `0x019450FC` jumps at `0x019457D2` into the rewrite of its loop `0x019457D6..0x01945AC6` and comes
   back at `0x01945AC8`; its four `__cxa_vec_ctor` CALLs (`0x0194569A/6B6/6DC/6EE`, a zero halfword through a
   function pointer 256 times per block) call `coop_cwo_vec_ctor`, which returns at once: `0x019446E0` writes all 64
   entries of each array before anything reads them. The six helpers per sample are inlined with the stock's
   arithmetic, the allpass stages unrolled so their state accesses ride in the parallel slots. op1emu, stock cwo at
   the synth with a chord (`det_fx_stock_cwo.txt`): 54.8 k -> 28.5 k emulator cycles per call (-48 %). op1emu counts
   one cycle per issued packet, so this is the instruction count; the device adds stalls and cache misses (stock
   92 k). The samples are identical to a stock run in the same emulator state (op1emu's two-state run-to-run
   variation splits two stock runs from the chord on, too), also with the fast-moving-delay branch and the
   read-before-start branch forced on both sides (test-only byte patches) and with all four cwo knobs turned during
   the chord. [C in op1emu, the device figure pending]
4. **punch's stereo render, both channels in one pass** (`coop_punch_render_st`, core/sdram/punch_fast.S; switch
   `g_coop_fast_punch`, 1 by default). The stock stereo render `0x019E662C` is dual-mono: two CALLs of the mono render
   `0x019E6260` (this + 4 / left, this + 0xD4 / right), each a 64-sample outer loop around a serial chain of N =
   W[params + 4] >> 10 waveshaper stages (an interpolated sine-table lookup, a 32 x 32 multiply, the A / B history
   at mono this + 4 / + 0x64). Entry: the stereo class vtable's render entry (`0x01910C50`, the only way in: no CALL
   or jump in the image targets `0x019E662C`) points at coop, which jumps to the untouched stock render when the
   switch is 0, when a display slot is -1 (the stock asserts there), when both buffers are the same, or when N is
   outside 1..24 (the knob's range is exactly 0x600..0x6200, N 1..24, op1emu). No region-C code (0 bytes), 0x502
   bytes in region A. One loop for both channels: each stage of the left chain interleaved with the right's (34
   packets per stage pair, stock 2 x 22), the history A as a circular buffer (I0 / I1, L = 4N), the per-block
   values in registers or frame slots, and the lookup of A[N-1] done once per sample (the output's lookup is the
   next sample's first one). The arithmetic is the stock's with its operands, order and modes; rewritten forms that
   give the same bits: the table address as `~(v >>> 20)` from a base 4097 entries up, the weight as
   `(v.H << 12) >> 1` in 16 bits, the two interpolation products summed in A0 + A1 (neither can saturate), x * p6 as
   one 16 x 16 product and the output product without the stock's two zero terms. The table base is read at run time
   from the stock's own immediates (`0x019E636A / 6E`), so the package's L1 sine retarget carries over. op1emu, stock
   punch at the synth with a chord (`det_fx_stock_punch.txt`, N 19): **66.9 k -> 49.5 k emulator cycles per call
   (-26 %)**, the callback 112.6 k -> 95.1 k per block (-15.5 %); samples identical over the whole run. With the
   knobs turned during the chord (`det_fx_stock_punch_knobs.txt`: white swept N 19 -> 1 -> 24 -> 10, the other
   three moving): the fast run is sample-identical to one of three stock runs (the stock runs differ among
   themselves from the first knob turn: op1emu's run-to-run variation moves knob events by blocks), levels within
   0.04 % of the ab_switch stock run. A first version took the weight as `(v << 12) >> 17`, which keeps bits 5..19
   of v, not 16..19: 1-2 LSB of DC in quiet passages (the level check passed), found because the fast run matched
   no stock run. Not exercised in op1emu: the fallback cases (slot -1, shared buffers, N outside 1..24). The device
   figure is pending (stock 78 k; the lookups' DAG stalls are what the interleave is for, which op1emu does not
   model). [C in op1emu, the device figure pending]
5. The other effects by cost. 6. The base cost left (cache pressure from coop code).

2. **coop's S3 stage in assembly** (`coop_stage.S`; switch `g_coop_fast_stage`): the decimator with its rings
   circular for the whole block, the pair clear, both DC blockers and the mid / side split in one loop. Bit-exact
   with the C path (`tests/sim/kern_check.cc`, 3000 random blocks, state carried on); op1emu 11,571 -> 6,697 emulator
   cycles per block (-42 %), audio identical over a 50 s six-voice run. [C in the simulator and op1emu]
3. **DSynth's oscillator loop: withdrawn.** A rewrite (2026-11-04) hoisted a gain it took for loop-invariant;
   `W[SP+0x72]` is the high half of `[FP-0x14]` (FP = SP + 0x84 in this frame), which the loop rewrites every
   sample, so the gain follows the previous sample. The plain DSynth A/B passed because that value had settled;
   DSynth with cwo came out 22 % quieter and the bisect found it. Done right, the gain stays in the loop and the
   saving (1.2 % at best) is gone, so the loop stays stock. Lesson: check every frame halfword load for
   aliasing against the loop's own stores, and A/B each rewrite in more than one scenario.
4. cwo's loop (in progress). 5. The other effects by cost: punch, phone, nitro, spring, grid and delay are already
   compiled hardware loops without per-sample calls; a rewrite gains less there.
6. **phone's mono render, both loops** (`core/sdram/phone_fast.S`; switch `g_coop_fast_phone`, 1 by default). phone
   is dual-mono: the stereo render `0x01A1C25C` calls the mono render `0x01A1BB74` twice (its only callers). The mono
   render runs a 64-sample capture loop `0x01A1BC00..0x01A1BC58` (input into a buffer at `0x027D1FE0 + this[0x88] *
   0x89D0 + sel * 0x2DF0`, a level ramp, a trigger that closes / swaps buffers) and a 64-sample output loop LC1
   `0x01A1BCB0..0x01A1BF34`: a phase steps by the rate knob and N = 0..10 new capture samples per output sample go
   through three LC0 loops over a 32-entry ring (the read, a 32-bit high-pass with the stock 32 x 32 multiply, a 16-bit
   low-pass), then a 7-tap interpolating FIR (table `0x0106A730`, 32 phases, an 11-bit blend) and the wet / dry mix.
   Both loops are entered only by their LSETUPs (`0x01A1BBFC`, `0x01A1BCAC`), which become JUMP.Ls to two `.near` stubs
   (50 bytes each); with the switch off they set LT / LB / LC by hand and jump into the stock loop. The rewrite runs
   the three ring loops as one chain per new sample with both filter states in registers (12 packets a sample against
   about 43), the FIR with one dual 16 x 16 multiply per product pair and the table / ring loads in the parallel slots,
   and the capture loop with the level, the write pointer, the count and the trigger flag in registers (12 packets
   against about 26). Every multiply, add and shift on the signal has the stock's operands, order and mode; the
   buffer-end, buffer-swap and trigger paths are the stock's in their own branches. op1emu, stock phone at the synth with
   a chord (`det_fx_stock_phone`, from 40 s): **28.9 k -> 14.1 k emulator cycles per call (-51 %)**, the callback 74.6 k
   -> 59.7 k; with all four knobs turned during and after the chord (`det_fx_stock_phone_knobs`, from 41 s): 29.0 k
   -> 14.0 k, the callback 148.9 k -> 134.0 k, **the samples identical over the whole run**. In `det_fx_stock_phone` the
   faster callback moved the chord to another block and the levels differ by up to 3 % in the chord's seconds (the
   capture buffer closes on a fixed sample count, silence or not, so the effect's state is not shift-invariant); the
   proof there is a lockstep build (`make ... EXTRA_CXXFLAGS=-DCOOP_PHONE_CHECK`: `plugin_host.cc` runs the stock
   render on a copy of each half's object and I/O block and the rewrite on the real one every block and counts any
   difference): **0 differences in 45,488 (det_fx_stock_phone) and 43,816 (knobs) mono calls**, with every rare path
   taken (hit counters in the assembly, knobs run: 11,544 buffer ends inside a sample, 595 buffer swaps there, 619
   capture-buffer closes, 2,158 trigger swaps). Not reproduced: the stock's dead stores to `W[FP+8]` / `W[FP+0xa]`
   (the caller's argument homes) and its loop scratch `[FP-8]`; the frame constants the loops read are reused as
   scratch after they are read. [C in op1emu, the device figure pending]
7. **nitro's mono render, both loops** (`core/sdram/nitro_fast.S`; switch `g_coop_fast_nitro`, 1 by default). nitro is
   dual-mono: the stereo render `0x019A684C` calls the mono render `0x019A62A4` twice (this + 4, this + 0x40; its only
   callers). After the coefficients, two 64-sample loops: LC1 `0x019A64A8..0x019A6682` (x = the input, a sine-shaper
   lookup S(s0) of the loop's own last output against x << 14, a 32 x 32 multiply by k1, S() again, then a four-stage
   one-pole ladder y_k = sat(sat(z_k + M(y_k-1, g)) + M(sat(-z_k), g)) with the poles mixed by five coefficients,
   S(sat(acc << 2)) as 32-bit words into an L1 buffer at `0xFF801B50`) and LC1 `0x019A6698..0x019A67F6` (the same
   shape on that buffer with its own k2 / g2 and no mix, output S(y_3).H in place). S is the stock's interpolated lookup
   in the 8192-entry sine table (P5 at the loop entry, so the package's L1 retarget and its reference count are
   untouched); M is the stock's six-instruction 32 x 32 multiply. Both loops are entered only through loop 1's LSETUP
   (`0x019A64A4`), which becomes a JUMP.L to a 50-byte `.near` stub; with the switch off it sets LT1 / LB1 / LC1 and
   jumps into the stock loop (loop 2's LSETUP is stock). The rewrite keeps all 23 multiplies per sample instruction
   for instruction and cuts the bookkeeping: the three inner stages of each ladder unrolled with states and
   coefficients loaded in the parallel slots; the lookup's index as `(T + 0x2002) + 2 * ~(v >>> 20)` (4 packets
   against 5), its fraction as `v.H << 12 >> 1` (2 against 4), its two 16 x 16 products summed in A0 (both are
   non-negative fractions, so neither saturates alone and one 40-bit saturation equals the stock's two saturated
   products added with saturation); x << 14 as one integer multiply by 0x4000 (so x comes through an I-register half
   load); and loop 2's G = S(s2) is the previous sample's output lookup S(y_3) (s2 is y_3), computed once. **132 + 87
   packets per sample against the stock's 180 + 131.** op1emu, stock nitro at the synth with a chord
   (`det_fx_stock_nitro`, from 34 s): **38,605 -> 31,728 emulator cycles per call (-18 %)**, the callback 84.3 k ->
   77.4 k, **the samples identical over the whole run**; with all four knobs turned during and after the chord
   (`det_fx_stock_nitro_knobs`, from 41 s): the same per-call figures (the loops have no data-dependent branches, so
   every call costs the same), the callback 158.5 k -> 151.7 k, levels equal second by second (largest difference
   0.01 %), samples differing between 29.8 s and 48.2 s by up to 138 (a moved UI event, not an offset: lag 0 fits best;
   the same script prefix is identical in the base scenario). The proof is the lockstep build
   (`EXTRA_CXXFLAGS=-DCOOP_NITRO_CHECK`, `plugin_host.cc nitro_check`, as phone's): **0 differences in 33,972
   (det_fx_stock_nitro) and 33,830 (knobs) mono calls**, the rewrite run in every one (`g_nitro_hit_calls`); there are
   no rare branches. Not reproduced: the stock's spill `[FP+8]` (the caller's argument home, dead after the loop). The
   per-call saving is less than the loops' 30 % because the coefficient setup, the two level meters (64-sample vector
   MAX loops) and the call overhead stay stock. Region C `.near`: 0x286 bytes used. [C in op1emu, the device figure
   pending]

8. **grid's stereo loop** (`core/sdram/grid_fast.S`; switch `g_coop_fast_grid`, 1 by default). grid is a 2D wave mesh: a
   tank of two 260 x 260 planes of halfwords (coop's own per object through the 0x019AF846 thunk; cell (x, y) of plane p
   at T + p * 0x21020 + y * 520 + 2x). The stereo render `0x019AF83C` derives per block the grid size (b, a), H, q =
   0x400000 / (b - 3) and the gains, then runs the loop `0x019AF932..0x019AFCC8` (64 samples, 353 instructions, 88 frame
   accesses each): walker 1 (this[0x18/0x1c/0x08]) adds inL * K3 * 2 tabA to its cell and inR * K3 * 2 tabB to a mirrored
   cell, walker 2 (this[0x10/0x14/0x04]) runs the wave update (four neighbours of the other plane, halved, minus the old
   value, times 2 tab * K44) at its cell and its mirror, the outputs mix the two new cells with the input, and a third
   walker (this[0x20/0x24/0x0c]) clears one cell outside the active area per sample; four phases (step q, wrap at
   0x400000) index the table `0x0101E5E8`. The only way into the loop is its LSETUP (`0x019AF92E`, no other branch in the
   image targets the range), which becomes a JUMP.L to a 58-byte `.near` stub (switch 0, or both I/O buffers the same:
   LT0 / LB0 / LC0 by hand, then the stock loop); the rewrite (970 bytes, region A) returns to `0x019AFCCA`, which reads
   only I2 / I3 and the epilogue. Every multiply, add and shift on the signal is the stock's with its operands, halves,
   order and modes, and every tank access is in the stock's order wherever two can touch the same word. What changed:
   the per-block constants are a 27-word circular stream (I3 / B3 / L3 over a block below the stock frame) read in the
   third parallel slot, so the P slot carries the object fields, the phases and the tank; the addresses are
   T + 2 (260 y + x) + p * 0x21020 (one multiply and one `(R + R) << 1` per cell) and the other plane T + 0x21020 -
   p * 0x21020 (the plane fields are 0 from both constructors and only toggled, so this is the stock's
   T + (p == 0) * 0x21020); the eight neighbour loads go through I2 with M0 / M1 / 4 steps in the parallel slots; the
   walker-1 step is done first, after its fields are read; the right input is read before the left output is written,
   which is why shared buffers take the stock loop. 136 packets a sample against the stock's 353 instructions. op1emu, stock grid at the
   synth with a chord (`det_fx_stock_grid`): **19.0 k -> 11.05 k emulator cycles per call (-42 %)**, the callback
   64.7 k -> 56.7 k; with all four knobs turned during and after the chord (`det_fx_stock_grid_knobs`, from 41 s):
   19.2 k -> 11.2 k, the callback 139.1 k -> 131.1 k; **the samples identical over the whole run in both** (no lockstep
   build needed). Not exercised in op1emu: the shared-buffer fallback. The device figure is pending (stock 36 k; the
   loop's tank accesses are unchanged in number, so the cache misses that make up much of the device cost stay).
   [C in op1emu, the device figure pending]
9. **spring's stereo loop** (`core/sdram/spring_fast.S`; switch `g_coop_fast_spring`, 1 by default). spring is native
   stereo: the stereo render `0x01A32FFC` (LINK 0x40, seven registers: FP = SP + 0x5C) sets its coefficients and runs one
   64-sample LC0 loop `0x01A3310E..0x01A33694` (its LSETUP at `0x01A3310A` has the end above the start, which objdump
   prints as a negative offset and `loopscan.py` cannot parse), no branches, no calls. Per sample: four one-pole stages
   with the stock 32 x 32 multiply (this[4], [8], [0x18], [0x28]), nine delay lines in the tank (T = [FP-0x10], coop's own
   tank through `coop_tank_spring_render`; lengths 382..6898 samples, a fixed tap each, a one-pole state each), four
   halfword feedback values (this[0x38..0x3e]), nine write indices (this[0x40..0x60]), the wet pair added to L / R in
   place. The stock spends most of its 393 packets a sample on index bookkeeping: per line it reloads the index from the
   object, forms both addresses with a compare for the tap's wrap, increments the index with a compare and stores it
   (about 19 instructions a line), and passes temporaries through the frame. The rewrite keeps the 18 read / write
   pointers as addresses in a frame of its own (LINK 0x80), loaded, post-incremented and stored in the parallel slots
   (`W[P ++ P5]` with P5 = 2 for the high-half accesses), and cuts the block into segments in which no pointer reaches
   its line's end (every line is longer than 64 samples, so a pointer wraps at most once a block: 1.8 segments a block
   on average); the wraps, and the write-back of the indices, happen between segments. The signal arithmetic is the
   stock's instruction for instruction (operands, order, modes); the products M(x, c) with x a 16-bit value << 16 lose
   the stock's two zero terms (3 instructions, not 6); the stock's x << 16 shifts become the multiply writing the high
   half. **162 packets a sample against 393.** Hoisted (nothing in the loop stores to them, as words or halves; checked
   against every store of the loop: the object, the tank, I0 / I1, [FP-0xc], [SP+0x1c], [SP+0x20..0x23], [FP+0xc],
   [FP+0x10]): c = R7, R2, R3.L, g4 = [FP-4].L, gb = [FP-0x14].L, g8 = [FP-8].L, gf = [FP+8].L, T. The code after the
   loop reads P0 = this (the stock loads it inside the loop from [SP+0x24]) and the object; the rewrite sets P0. Entry:
   the LSETUP becomes a JUMP.L to a 50-byte `.near` stub (switch off: LT0 / LB0 / LC0 by hand); the stock loop also runs
   when L and R are the same buffer or an index is out of range (neither seen). The loop is entered in its third packet
   on each segment (LT0 / LB0 / LC0 set by hand, then a jump into the body), so the previous sample's output stores and
   pointer store that ride in its first packets are skipped on the first pass and done after the last.
   op1emu, stock spring at the synth with a chord (`det_fx_stock_spring`, from 34 s): **27,807 -> 13,603 emulator cycles
   per call (-51 %)**, the callback 73.5 k -> 59.3 k per block, **the samples identical over the whole run**. With all
   four knobs turned during and after the chord (`det_fx_stock_spring_knobs`, from 41 s): 27,807 -> 13,597, the callback
   147.7 k -> 133.5 k, levels equal second by second (largest difference 0.08 %), the samples differing from 0.52 s (at
   boot, before spring is selected: op1emu's run-to-run variation; three stock runs differ from each other the same
   way, max 47-83; the fast run against the ab_switch stock run: max 56). The proof is the lockstep build (`EXTRA_CXXFLAGS=-DCOOP_SPRING_CHECK`, `plugin_host.cc
   spring_check`): the stock render runs on a shadow object, shadow I/O and a shadow tank (the shadow object is in no
   tank-map entry, so the lookup returns the map's default word, pointed at the shadow tank for that call), then the
   rewrite on the real ones; object, output and the whole tank compared: **0 differences in 7,288 (det_fx_stock_spring)
   and 8,932 (knobs) calls**, every one through the rewrite, with 6,050 / 7,408 blocks cut into two segments or more and
   6,076 / 7,444 pointer wraps spread over all 18 pointers. A first version lost line 9's write-pointer step at the end
   of every segment (its store rides in the loop's first packet): the segments collapsed to one sample (the cost stayed
   at 23 k) and the check failed every call. Region C `.near`: 0x2b8 bytes used; region A 0x99a bytes. Two pointer loads
   still sit one packet before their use (DAG stalls on the device, not in op1emu). [C in op1emu, the device figure
   pending]

11. **Voltage's voice loop** (`core/sdram/voltage_fast.S`; switch `g_coop_fast_voltage`, 1 by default). Voltage (Slump.cpp)
   renders each voice with `render(this, out, params)` at `0x01A49BF4` (vtable + 0x14; no CALL or jump in the image
   targets it), 128 samples at 88.2 kHz into a 16-bit buffer, then every fourth sample into a display buffer. The setup
   (LINK 0x2c, seven registers: FP = SP + 0x48) leaves the per-block values in the frame and runs one LC0 loop
   `0x01A49CBA..0x01A49ED0`: 111 instructions a sample (the scan table's "~105"), 24 frame accesses, two conditional
   branches, and four sine lookups (not six): s1 from phase 1 alone, s2 / s3 / s4 phase-modulated by their own last
   values (this[0x18], [0x20], [8]), each index moved from a data register into a P register right before the address
   add and the load. Per sample, the pitch word d ([I1++], L1 `0xFF801950`) and d times a per-block K (the stock 32 x 32
   multiply) detune four phase accumulators; the waveshapers f(x) = u + 0.5 u, u = x - 0x2667 x x x, of |s4| and |s3| are
   XORed, scaled by params[0] and shaped again, mixed with 0.125 f(s2) and a decaying level this[0x28] (halved when phase
   1 wraps, reduced by f(|s4|) K28) into a one-pole low-pass this[0x24]; y = 0.5 s1 + z.H, out = (y >>> 1) + (y_prev >>>
   1). The only way into the loop is its LSETUP (`0x01A49CB6`), which becomes a JUMP.L to a 50-byte `.near` stub (switch
   0: LT0 / LB0 / LC0 by hand, then the stock loop); the rewrite (480 bytes, region A) returns to `0x01A49ED2`, which reads
   only R6.L, R7, P1 = out and `[SP+0x24]`. Every multiply, add, shift and saturation on the signal is the stock's with
   its operands and modes; what changed: the index as `(T + 0x2000) + 2 * (v >>> 19)` (one shift for the stock's three
   ops); the waveshapers of |s4| and |s3| as one pair in the dual MAC (s4 high, s3 low: one ABS (V), three dual
   multiplies, a vector subtract and add with (S)), f(s2) with f(w) and their 0.125 / 0.25 scales the same way, 0.5 h28
   with 0.5 s1, K40 h8 with K40 h20; the seven per-block values as a circular stream in the third slot (I3 / B3 / L3
   below the stock frame); h8 and h28 through I2 walking between this + 8 and this + 0x28 (M2 = 0x20); the wrap halving
   as a conditional move with CC held from the compare (the stock stores a flag byte, reloads it and branches over the
   halving); the output store rotated into the next pass's first packet, with I0 circular over the output (B0 = out, L0
   = 256) from out + 254, so the first pass's store lands on out[127] and the one after the loop rewrites it. **64
   packets a sample against 111.** Not reproduced: the loop's frame scratch (`[SP+0x1c]`, `[SP+0x34]`, `[SP+0x44]`) and
   its stores to the caller's argument homes (`[FP+8]`, `B[FP+0xc]`, `W[FP+0xe]`, `[FP+0x10]`), dead after the loop.
   op1emu, Voltage (factory sound slot 2; the only engine heard, so cost index 15) with a six-note chord
   (`tests/sim/det_voltage6.txt`, from 31 s): **14,371 -> 8,391 emulator cycles per voice render (-42 %)**, the callback
   155.6 k -> 119.7 k per block (-23 %); with the four engine knobs turned during the chord and notes played on and off
   (`det_voltage_knobs.txt`) the same per-render figures (no data-dependent branch but the rare wrap). Levels equal second
   by second (largest difference 0.20 % in det_voltage6, 0.36 % with the knobs); the ab_switch pair differs from frame 1 (-218 /
   -219, op1emu's two-state run-to-run variation), but in a second job **the fast det_voltage6 run is identical to one of
   two stock runs over all 3,821,838 frames** (the two stock runs differ from each other the same way). The proof is the
   lockstep build (`EXTRA_CXXFLAGS=-DCOOP_VOLTAGE_CHECK`: the synth render thunk sends the Voltage render to
   `coop_voltage_check` in `stereo_bus.cc`, which runs the stock loop on a copy of the voice (0x34 bytes) and of the
   output, then the rewrite on the real ones, and compares voice, output and display copy): **0 differences in 42,024
   (det_voltage6) and 50,295 (knobs) voice renders**, every one through the rewrite (`g_voltage_hit_calls`). Before
   op1emu, a simulator check against the stock instructions themselves (`tests/sim/run_voltage_check.sh STOCK_DIS`: the
   stock render transcribed from the disassembly into a temp directory; random voices, tables, parameters and full-range
   pitch words, both Kf cases, wraps and saturations): 0 differences in 2,400 renders, once one stock packet the
   simulator gets wrong was split (`R0 = ABS R3 (V) || W[P4 + 0x1c] = R0` stores s3, the old R0, on the hardware and in
   op1emu; bfin-elf-run stores the ABS result). Region C `.near`: +0x32 bytes (the section is 0x324 now). Each lookup's
   P register is now set 3 or more packets before its address add and the add sits 2 or more packets before the load
   (the stock loads in the packet after the add), so the DAG stalls the scan pointed at should shrink on the device too
   (not measured: op1emu counts none). [C in op1emu and the simulator, the device figure pending]

10. **delay's stereo loop** (`core/sdram/delay_fast.S`; switch `g_coop_fast_delay`, 1 by default). delay is a varispeed
   tape: a line of L 32-bit words (L = 44100 >> (k0 >>> 11), coop's own line per object through the line table) and three
   16.16 positions in the object, A (read), B (clear) and C (write), stepping together by 2 max(k1, 0x21) and wrapping at
   L << 16. The stereo render `0x0197DC42` reduces the positions, writes the mono input (k3 / 2)(inL + inR) to the L1
   buffer `0xFF801B50`, then runs LC1 `0x0197DD4A..0x0197DF7A` (64 samples, about 266 instructions a sample with its two
   LC0 loops): a 7-tap interpolating FIR read at A (table `0x0106A730`, the one phone uses, built at boot), both outputs
   dry + that tap's high half, the feedback u = sat(sat(2 M(t, k2 << 16)) + m), w = M(u, k1' << 16), line[B] = 0, and the
   same FIR scattered at C (line[C + k] = sat(sat(line + M(w, c0 << 16)) + M(w, c1 << 16))). The only way into the loop is
   its LSETUP (`0x0197DD46`), which becomes a JUMP.L to a 60-byte `.near` stub (switch 0, or a line not word-aligned:
   LT1 / LB1 / LC1 by hand, then the stock loop); the rewrite (836 bytes, region A) returns to `0x0197DF7C`, which reads
   only `[SP+0x20]` / `[SP+0x24]`. What changed: both FIRs unrolled with the table and line loads in the parallel slots,
   the line circular (B1 / L1) for the whole block, ten per-block constants a circular stream in the third slot (I3), the
   positions' integer parts turned into addresses as (int + line / 4) << 2 and 31 - ph as (~A << 16) >> 27, and every
   32 x 32 multiply (16 a sample, all by a value whose low half is 0) as one dual MAC, a shift and the add instead of six
   instructions. The stock order of every memory access that can alias is kept (A's reads, out L, in R, out R, the clear,
   each C + k read before its write and after C + k - 1's), so a shared I/O buffer and a line shorter than 7 words still
   behave. Read taps 1..6 keep the stock's packet `R1.L = R1.L * R0.H || R1.H = W[P2 ++ P3]`, which writes both halves of
   R1: op1emu merges them, the GNU simulator keeps the load's write (the multiply is lost there), so only that packet as
   it is matches the stock under both. **137 packets a sample against about 266.** Bit-exactness in the GNU simulator: a
   harness that runs the stock loop's own bytes (extracted from the stock ELF at test time, not committed) and the
   rewrite on the same frame, registers and random state, 3000 blocks with lines of 1..44,100 words, steps, knobs and
   positions random: 0 differences. op1emu, stock delay at the synth with a chord (`det_fx_stock_delay`, from 34 s):
   **17,924 -> 11,758 emulator cycles per call (-34 %)**, the callback 63.6 k -> 57.4 k, **the samples identical over the
   whole run**; with all four knobs turned during and after the chord (`det_fx_stock_delay_knobs`, from 41 s): 17,923 ->
   11,758, the callback 137.8 k -> 131.7 k, levels equal second by second (0.00 %), samples differing from 2.97 s (before
   the effect is selected: op1emu's run-to-run variation) by up to 316. The proof there is the lockstep build
   (`EXTRA_CXXFLAGS=-DCOOP_DELAY_CHECK`, `plugin_host.cc delay_check`: the stock render on copies of the object, both I/O
   blocks and the whole line, the rewrite on the real ones, every block): **0 differences in 38,136
   (det_fx_stock_delay_knobs, to 94.7 s) and 37,627 (det_fx_stock_delay, to 89 s) calls** (the whole-line copy slows op1emu, so
   those runs used the scenarios with the final waits stretched, to reach past the knob turns). That build also found the one real bug of
   the first version: the stub tests its switch through P2, the stock's table pointer, so the rewrite read its
   coefficients from its own code; the simulator harness had jumped straight into the rewrite and could not see it (the
   table now comes from `[SP+0x38]`). Not exercised in op1emu: the unaligned-line fallback, a shared I/O buffer. Region C
   `.near`: 0x2FC bytes used. The device figure is pending (stock 29 k; the line traffic, 22 SDRAM words a sample, is
   unchanged, so the cache misses stay). [C in op1emu, the device figure pending]
12. **Phase's voice loop** (`core/sdram/phase_fast.S`; switch `g_coop_fast_phase`, 1 by default). Phase (Pd.cpp) renders
   each voice with `render(this, out, params)` at `0x01A0BC5C` (vtable + 0x14; no CALL or jump in the image targets it), 128
   samples at 88.2 kHz, then adds to the global `0x01DE4B8C` (kept: the rewrite returns before it). The setup (LINK 0x24,
   seven registers: FP = SP + 0x40) runs one LC0 loop `0x01A0BCE4..0x01A0BD5A` whose body is split: part A in the loop
   range, part B out of line at `0x01A0BD88..0x01A0BE80`, run when the flag B[this + 0x14] is set, which branches back to
   the loop's last instruction (a NOP); that is the scan's "software loop". Part A: s = T[ph1 >>> 19 + 4096], ph1 += d (the
   pitch word), t = s * p2, out = t >>> 1, gH = sat(t + 0x4000); when ph1 wraps, ph2 is reset (to ph1, or to 0x80000000 when
   p3 >>> 10 is not 0), the count this[0x18] cleared and the flag set. Part B: ph2 += sat(sat(d + M(sat(d << 2), p0^2 << 16))
   + M(M(sat(d << 8), (ph1 >>> 1) + 0x40000000), p3sq)) (M the stock 32 x 32 multiply), out = T[ph2] * h * h * h * gH with
   h = sat(0x7fff - p1 * gl); each ph2 wrap counts, and the flag clears when the count passes p3 >>> 10. The stock spends
   30 instructions a sample on part A and 82 with part B, wraps not counted. The only way into the loop is its LSETUP
   (`0x01A0BCE0`), which becomes a JUMP.L to a 50-byte `.near` stub (switch 0: LT0 / LB0 / LC0 by hand, then the stock
   loop); the rewrite (0x132 bytes, region A) returns to `0x01A0BD5C`, which reads only P2 = this, P0 = out and the
   voice. Every multiply, add, shift and saturation on the signal is the stock's with its operands and modes. What changed:
   the index as `(T + 0x2000) + 2 * (v >>> 19)`; ph1 / ph2 stored and reloaded in the parallel slots (ph1 for the next pass
   in the loop's last packet, beside the output store through I1), which also removes the old / new register copies the
   wrap compares need; the per-block values in the parallel slots from two frame words the setup packs (p1 : p2 and
   0x7fff : p0^2); the flag in P4 (one `CC = P4 == 0` a sample, written back after the loop); the two wraps and the count out
   of line, without the stock's flag bytes in the frame; M(sat(d << 2), p0^2 << 16) without its two zero terms; and the
   stock's separate gl = sat((ph1.H >>> 1) + 0x4000) taken as q.H of q = sat((ph1 >>> 1) + 0x40000000) (neither can
   overflow, so the bits are the same). **13 packets a sample when the flag is 0 and 45 when it is 1, against 30 and 82.**
   Not reproduced: the frame flag bytes `B[SP+0x50/0x51]` and the stores to the caller's argument homes `[FP+8]`,
   `[FP+0xc]`, dead after the loop. op1emu, Phase (factory sound slot 7, the only engine heard: cost index 15) with a
   six-note chord (`tests/sim/det_phase6.txt`, from 31 s): **10,286 -> 5,835 emulator cycles per voice render (-43 %)**, the
   callback 131.0 k -> 104.3 k per block (-20 %); with the four knobs turned and notes played on and off
   (`det_phase_knobs.txt`) the same per-render figures in the measured window. det_phase6: the ab_switch pair differs from
   frame 0 (-182 / -181, op1emu's two-state run-to-run variation), levels within 0.09 %; in a second job **the fast run is
   identical to one of two stock runs over all 3,330,887 frames** (the two stock runs differ from each other the same way).
   Knobs: levels within 0.00 % second by second, samples differing (stock runs without the cost watch differ from each other
   too). The proof is the lockstep build (`EXTRA_CXXFLAGS=-DCOOP_PHASE_CHECK`: the synth render thunk sends the Phase render
   to `coop_phase_check` in `stereo_bus.cc`, which runs the stock loop on a copy of the voice (28 bytes) and the output,
   puts the global accumulator back, then runs the rewrite on the real ones and compares voice, output and accumulator):
   **0 differences in 42,006 (det_phase6) and 50,368 (knobs) voice renders**, every one through the rewrite, with 5,244 /
   6,420 ph1 wraps, 47,215 / 55,642 ph2 wraps and 254 / 5,769 renders starting with the flag 0. Before op1emu, the simulator
   check against the stock instructions (`tests/sim/run_phase_check.sh STOCK_DIS`, `phase_transcribe.py`, `phase_check.cc`:
   random voices, flags, counts, sine tables, parameters with p3 >>> 10 zero / positive / negative, full-range and small
   pitch words): 0 differences in 3,600 renders (93,003 ph1 wraps, 48,879 ph2 wraps); three deliberate one-instruction
   mutations of the rewrite each failed it (60 to 2,993 bad renders). No packet of the stock needed splitting for
   bfin-elf-run (its one ALU-and-store packet stores the half the ALU op leaves alone). Region C `.near`: +0x32 bytes (0x356
   now). [C in op1emu and the simulator, the device figure pending]

Correction (2026-11-04): the T4f device test already measured the six-voice DSynth chord at 65-67 % of the block
(the stock engine about 57 %, coop 8.6 %). There is no "base cost" regression since T4: the engine itself is the
cost, which is why its loops are the lever.

## Per-engine cost and the engine scan (p5a, 2026-11-04)

**Measurement.** The synth and drum render thunks (`stereo_thunks.S`) read CYCLES around every stock voice render and
hand (render function, cycles) to `coop_eng_cost` (`coop_stats.cc`). The stats file gains an `engines` line:
`synth=` and `drum=` are the cycles of all voice renders in a block (one entry per block with a voice), then one
entry per engine, per voice render, named from the render function (`fm`, `digital`, `drwave`, `string`, `cluster`,
`pulse`, `sampler`, `phase`, `ls`, `dna`, `dsynth`, `voltage`, `drumsampler`; any other prints as `r_<address>`).
The render functions are vtable + 0x14 of each engine's voice class, read from the constructors the voice getters
call (`../te-op1-cfw/docs/engine-interface.md` engine table); DrumSampler (vtable 0x0190FAC4 + 0x24) was found in
op1emu (`det_drum`). The cost index of the first engine heard is 15 (`tests/sim/ab_switch.sh SWITCH SCENARIO 15`),
the block sums are 13 (synth) and 14 (drum). The window ring keeps 24 windows (two minutes, was 12) and the
heaviest-windows lines come first, so the 8 KiB tape tail keeps them; the whole text (up to 16 KiB) is in the
yaffs file, read over SysEx.

op1emu, six-voice DSynth (`det_dsynth6`): **26.0 k emulator cycles per DSynth voice render with the stock voice loop,
18.6 k with `g_coop_fast_dsynth`** (342 renders each). The instrumentation costs about 30 cycles a block
(`tests/sim/ab_images.sh`, p4b against the p5a measurement image on `det_drum` and `det_dsynth6_cwo`: equal
levels second by second, 0.00 % and 0.14 %).

**Scan** (`loopscan.py` over the stock disassembly: every hardware loop and backward branch in each render and its
callees, the CALLs and frame accesses inside them). Only two engine loops make calls per sample: DSynth's voice
loop (already rewritten) and `ls` (`0x019D33A0`, an internal single-instance engine: `CALL (P1)` in two LC1 loops,
nine calls in a software loop). Everything else is compiled hardware loops without calls; the big per-sample
bodies are:

| Engine / effect | Hot loop | Packets per pass | Frame accesses | Notes |
|---|---|---|---|---|
| voltage | `0x01A49CBA..0x01A49ED0` (128 / render) | ~105 | 24 | six sine lookups, each through a P register loaded from a data register just before the load (DAG stalls on the device) |
| dna | `0x019B4A68..0x019B4C36` (LC1) | 146 instr | 20 | |
| string | `0x01A4F1E0..0x01A4F322` (LC1) | 102 instr | 11 | |
| fm | 36 small loops (one per operator / algorithm) | 12-18 | 0-3 | lean already; its cost was the sine table (T4c) |
| punch (mono, x2) | outer `0x019E6376..0x019E65A6` x64, inner `0x019E64CA..0x019E6520` x (param >> 10) - 1 | ~22 per stage | 0 in the inner loop | a serial chain of waveshaper stages per sample: interpolated sine lookup, a 32 x 32 multiply, history read / write |
| grid (stereo) | `0x019AF932..0x019AFCC8` x64 | 353 instr | **88** | state through the frame every sample |
| phone (mono, x2) | LC1 `0x01A1BCB0..0x01A1BF34` with four inner loops | 219 instr | 21 | |
| nitro (mono, x2) | two LC1 loops, 128 + 97 instr | | 7 / 2 | |
| filter (mono, x2) | LC1 `0x019E7C5C..0x019E7E86` | 150 instr | 11 | |
| delay (stereo) | LC1 `0x0197DD4A..0x0197DF7A` | 160 instr | 15 | |

So there is no further call-heavy outlier: the remaining gains are scheduling (state in registers, parallel loads,
the two channels of a dual-mono effect interleaved so one hides the other's stalls). The device ranking comes from
the p5a stats file's `engines` line.

**Merged-build re-gates (2026-11-04).** On the merged code (punch + phone): punch `det_fx_stock_punch` identical over
906,489 frames, 66,921 -> 49,458 per call. phone `det_fx_stock_phone_knobs` 28,932 -> 14,061 per call; this time the
two runs split from 0.6 s and differed by up to 18.6 % in seconds 46-47 (after the knob turns). Controls with the
same scenario: phone fast on both sides (only the punch word flipped) 1.32 %; **phone stock on both sides 13.6 %**
(second 46 came out 630 in one stock run and 715 in another). So that tail is op1emu's run-to-run variation acting on
phone's time-periodic capture state, the fast runs fall inside the stock spread, and the lockstep check (0
differences) stands as the proof. `tests/sim/ab_switch.sh` takes `AB_EXTRA="--set-word NAME=V ..."` for such controls.

## p5a: what the run changed, in one table (op1emu, 2026-11-04)

Every rewrite has its switch word (default 1; 0 runs the stock code), its numbered entry under "The DSynth lag and
the rewrite programme", and was re-checked on the merged build. op1emu counts one cycle per issued packet; device
figures add stalls and cache misses (the p4b rewrites came out 32-34 % cheaper on the device for 42-48 % in op1emu).

| Code | Switch | Per call, stock -> fast (op1emu) | Change | Proof |
|---|---|---|---|---|
| punch (stereo render) | `g_coop_fast_punch` | 66,921 -> 49,458 | -26 % | identical samples (merged build) |
| phone (mono render x2) | `g_coop_fast_phone` | 28,932 -> 14,061 | -51 % | lockstep 0 / ~89 k calls; A/B inside the stock spread |
| nitro (mono render x2) | `g_coop_fast_nitro` | 38,605 -> 31,728 | -18 % | identical samples (merged build); lockstep 0 |
| grid (stereo loop) | `g_coop_fast_grid` | 19,199 -> 11,187 | -42 % | identical samples (agent, two scenarios); levels 0.28 % merged |
| spring (stereo loop) | `g_coop_fast_spring` | 27,807 -> 13,601 | -51 % | identical samples (merged build); lockstep 0 incl. the tank |
| delay (stereo loop) | `g_coop_fast_delay` | 17,924 -> 11,758 | -34 % | identical samples (agent); lockstep 0 |
| Voltage voice render | `g_coop_fast_voltage` | 14,371 -> 8,391 | -42 % | identical samples (merged build); lockstep 0 / ~92 k renders; simulator |
| Phase voice render | `g_coop_fast_phase` | 10,286 -> 5,835 | -43 % | matches a stock run over all frames; lockstep 0 / ~92 k; simulator |

Whole image, every switch off against every switch on (`tests/sim/ab_images.sh`): six-voice DSynth with cwo 84.9 k ->
64.9 k emulator cycles per block on average from 34 s (idle seconds included), six-voice Voltage 160.5 k -> 119.7 k;
no faults, levels within 0.14 % and 0.00 % second by second.

**Engine ranking** (`tests/sim/eng_rank.sh`, op1emu cycles per voice render, six-note chord per factory sound slot,
before the Phase rewrite): s5 dsynth 18,616 (its voice loop already rewritten), s3 pulse 16,505 (render in L1 code,
0xFFA04B88), s7 phase 10,272, s4 drwave 9,642, s2 voltage 8,391 (rewritten), s8 sampler 7,191, s1 digital 7,101, s6 fm
5,867. The device's `engines` line will give the real ranking (FM and DSynth pay more on the device for their data).

Not done, in order of expected value: DSynth's remaining loops (its oscillator loop stays stock, step 3), pulse (its
render is L1 code: a patch there goes through the decrypted L1 image), drwave, the dual-mono zero-side shortcut
(waits for the spread decision), filter (not reachable from the stock FX list).
