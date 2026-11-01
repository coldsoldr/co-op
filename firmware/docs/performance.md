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
