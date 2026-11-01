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
