# drive

An oversampled soft-clip overdrive, true stereo at every position: a quintic soft clipper at 88.2 kHz (2x halfband
up and down), a bias that makes the clip lopsided (odd to even harmonics), a tilt EQ about 1 kHz after it, an
auto-levelled wet and a dry / wet mix. The DSP is coldsoldr's te-op1-cfw `ports/fx/src/drive` (commit 9ced415;
design, mappings and measurements in te-op1-cfw `docs/drive-port.md`), as hundekraft's kennel plugin
`ports/knl/drive` 0.2 used it. GPL-3.0-or-later; the halfbands use Mutable Instruments Clouds' coefficients (MIT,
`LICENSE-MIT`).

## Controls (guidelines §3.2)

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | drive | tone | bias | mix |
| Click | — | — | — | — |
| Shift + turn | drive fine | tone fine | bias fine | mix fine |
| Shift + click | — | — | — | — |
| fn + turn | gain (input, -6..+6 dB, 0.5 dB a detent) | — | — | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | gain fine (0.1 dB a detent) | — | — | — |
| Shift + fn + click | — | — | — | — |

- **drive**: 0..+36 dB into the clipper, linear in dB. **tone**: a tilt, dark (+6 dB lows, -6 dB highs) .. flat at
  the centre .. bright. **bias**: the clipper's operating point 0..0.5 (more even harmonics). **mix**: dry .. wet;
  at the bottom the output is the input bit for bit. Defaults as kennel's: drive 35 %, tone flat, bias 0, mix wet.
- **gain** (fn + blue, word 5, Q15, default 16384 = 0 dB, saved with the sound, not randomised): the input gain into
  the clipper only. The dry side of the mix and the auto-level don't see it, so at mix 0 it changes nothing. -6 dB at
  0, +6 dB at the top (kennel `knl::GainQ14`). fn + blue moves it 0.5 dB a detent (`"step": 1365`), shift + fn + blue
  0.1 dB (`"fine_step": 273`), kennel's steps (the owner, 2026-11-11): blue's
  shift + fn layer is gain fine (the owner's decision, `docs/ui-features.md` §14), so nothing else may sit there.
  The core shows `GAIN nn` top left in blue while it moves.
- No latches: nothing here is cleared when the stock switches synth ↔ drum, beyond the effect's own state starting
  again there (the stock restarts the instrument effect; the smoothed knobs glide back in ~12 ms).

**Upstream mapping.** Kennel drive: the four turns as here (Q15 through `op1_fx_param_q15`); a blue press flipped
blue to page 2, where it turned the input gain (word 5), and shift + turn there gave a tenth-of-a-dB step through
`knl::Fine`, which read the stock shift byte (0x01DD7F78) and rewrote word 5. coop retires the page-2 toggle
(guidelines §4: every layer is held): the gain moves to fn + blue with `"fine": true`, 1365 Q15 (0.5 dB) a detent
and 273 (0.1 dB, kennel's click) a fine detent. No firmware address is left: the knob ranges are the
constant filter-type row (`kRange`, the same row kennel read at 0x01DE5C50 and the core's `fx_rng`), normalised once a
block (cached while the words stand still). Nothing else in the upstream control set: green, white and orange had no
presses (`OP1_PRESS_NONE`), so nothing else moved. Mix stays on the orange turn (§4.3).

**plugin.json.** No `"stereo"` flag: kennel's flag means an `op1_fx_stereo_t` entry, and coop's loader would check
callback pointers where a `coop_fx_t` has its magic and api, and refuse the file. A `coop_fx_t` is stereo by ABI.
`-DOP1FX_DRIVE_ASM_ONLY` builds the assembly passes only (the C references are for the tests).

## Stereo

Upstream already is stereo: every pass runs both channels in one instruction (L in the low halves, R in the high:
vector adds, dual multiplies, dual MACs; `docs/plugin-performance.md` rule 7). coop runs that path at every
position (guidelines §2.2), so the instrument positions, which kennel fed mono, now keep their per-voice placement
through the clipper. Nothing sums L and R. Kennel's mono path (the block's two halves side by side, and a fused mono
wet kernel) is cut: coop has no mono position.

Not taken: a mono pass when L == R (`docs/plugin-performance.md` "Process only what is there"). Upstream's mono path
costs ~30 % less, but switching between it and the stereo path restarts the control ramps and copies L's filter
states into R, so it would not be the same sound sample for sample, and the stereo cost is already a fifth of the
30 k budget (5.5 k still, 8 k peak moving, simulator).

## Screen

None yet: effects show the stock filter page until coop has an effect-screen hook (`docs/plugins.md` "Effect
controls"), with the core's value popup over it. The brief for when it lands (manifesto, guidelines §5): kennel's
design A "Schematic" (te-op1-cfw `ports/knl/drive/gen_drive_knl_svg.py`, coldsoldr's own art, not carried here
unchecked): the circuit left to right, the pot's wiper moving with the gain, the diodes filling from the tip with
drive, the triode's filament and glow warming with bias, the tilt's S curve flipping about its pivot with tone, the
mix gauge rising; the gain's dB readout while fn is held (`coop_scr_mods`), replacing kennel's page-2 state.

## Memory, L1, cost
- **e8 (2026-11-11):** the Drive, the block copies and the param cache are in the slot's position buffer (plugin.json `"scratch": 1600`, `coop_fx_scratch`); the file is relocated once and its `.bss` is the five buffer pointers (20 bytes); init resets the state there at every selection. The figures below are from before e8 (state in `.bss`, five relocations). File 9,464 bytes, load 8,480 once.

- File 9,608 bytes (0.1); `.text` 6,932, `.rodata` 1,432, `.data` 0, `.bss` 1,584 (the Drive object, 1.3 KB of
  histories and kernel blocks, the block's int16 copies, the param cache). Load size 9,948 bytes, x 5 relocations =
  49,740 bytes of heap 0.
  No imports; libgcc's `__divsi3`, `__divdi3`, `__muldi3` are linked in and run in the control path only (the gain
  divides 1 / K once a block while the knobs move; the glue's six divides only when a word changes), never per sample.
- L1: none (effects can't lease yet). The per-block working set is the 1.5 KB `.bss`, read and written once a pass.
- Cost per 64-sample block. Upstream (te-op1-cfw `docs/drive-port.md`, bfin-elf-run): stereo 4,651 still, 5,009 moving
  (max 5,029); mono 3,235 / 3,605. Kennel ran the mono figure at the synth and drum, the stereo one at the master;
  coop runs stereo everywhere: x 1.4 at the instrument positions, not x 2, because the stereo path already pairs the
  channels in the dual MAC. This port (`make -C tests/host/drive sim`, bfin-elf-run: no cache, no SDRAM):
  **5,539 a block with the knobs still; 7,578 average, 8,042 peak with drive sweeping every block and tone and bias
  stepping** (against upstream's 4,651 still, the int16 copies in and out are about 900 [I]; moving adds upstream's
  control groups, the 64-bit divide for the gain's 1 / K and the glue's divides, which the still case skips). About a
  quarter of the 30 k budget at the peak [I until the device]. Room left if it's ever needed: the divides by multiplies, the copies folded into the first
  and last passes. Device: pending (the `cost` line, `fx1_plugin=` / `s2_instr=` / `s2_master=`); budget under 30 k, over 60 k
  doesn't ship. The device runs code with memory traffic 10-35 % over the simulator, more when the I-cache misses.

## Tests

- `make -C tests/host/drive all sim` (WSL; from a worktree add `TE=/mnt/i/GitHub/coldsoldr/te-op1-cfw/ports/fx`):
  - `tables`: `src/drive_tables.h` (generated by `gen_drive_tables.py`: coop allows no double, so upstream's
    constexpr double tables are integers here) against upstream's constexpr tables, every entry.
  - `host`: the plugin's render on the int32 bus against upstream's DSP behind kennel's stereo glue, 20,000 blocks
    (sines, noise, full scale, silence; knobs jumping, sweeping and out of range; the gain word changing and out of
    range; re-inits), bit for bit; and mix 0 = the input bit for bit.
  - `sim`: the same in the GNU simulator with the plugin built as the device builds it (the assembly passes) against
    upstream's C passes, and the render's cycles.

## op1emu gate (not run yet: runs go one at a time)

Build the core (`make -C core/sdram combined`) and `make -C plugins`, then a NAND with drive alone in
`/yaffs2/plugins` (`docs/plugins.md` "Building and testing": `build/nand_overlay/plugins/drive.coop`,
`tools/make_nand.py ... --output ~/op1emu-work/plug_drive.bin`). The stock FX list is alphabetical, so "drive" is
row 2 (cwo, delay, drive, grid ...), not the last row test_fx and stereo_delay sat on: the scenarios that select by
turning to the end (`cw 96`) would land on spring.

1. `tests/sim/det_fx_drive_select.txt` (new: det_fx_select with `turn enc1 cw 16`; synth FX 1, a chord through it):
   no fault, `plugins seen 1 loaded 1 failed 0`, `plugin_blocks` counting, `fxctl fx 1`, p0's w5 16384, the stats
   `cost` line's `fx1_plugin=` (an emulator figure). Check the screenshot shows the filter page with drive selected
   [I: the row is worked out from the alphabetical order, not run].
2. All five positions: `tests/sim/det_fx_controls.txt` with its FX-1 list turns (lines 32, 118, 166: `cw 96`)
   changed to `cw 16`, and its FX 2 picks (lines 78, 140: shift + blue `cw 6`) checked against where drive sits in
   the FX 2 list [I]. Only fn + blue (and its fine) is declared, so the gain moves as test_fx's did, and every other
   layer the scenario touches is undeclared and ignored. Expected `fxctl` per position w5 / w6 / w7 / latch:
   p0 11197/0/0/0, p3 19114/0/0/0, p2 15019/0/0/0, p4 16384/0/0/0, p1 17749/0/0/0; `fine 1`, `clicks 0`,
   `latches 0`, `ignored` above test_fx's 1 (the green and orange clicks and the white turns are undeclared) [I:
   the exact count not worked out]; `master_saves 1`. Write the edited copy as `tests/sim/det_fx_drive_controls.txt`
   once the rows are confirmed.
3. `tests/sim/det_fx_reload.txt` (power cycle on the same NAND, `NAND_KEEP`): the five w5 words above come back.
4. A pristine-NAND `tests/sim/det_chord.txt`: unchanged against the last gate (`plugins seen 0`).
Read `fx1_plugin=` / `s2_instr=` / `s2_master=` in the cost line from 1 and 2.
