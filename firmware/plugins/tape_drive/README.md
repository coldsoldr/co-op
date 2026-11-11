# tape_drive: tape saturation, wobble and age (coop effect)

A port of te-op1-cfw's kennel effect **tape_drive 0.3** (coldsoldr; `ports/knl/tape_drive/tape_knl.cc`, DSP
`ports/fx/src/tape`, design and measurements in te-op1-cfw `docs/tape-port.md`). On kennel it was tested on the device
and called "good" (te-op1-cfw `docs/device-tests.md` Test 57, then renamed in Test 61) [C kennel]. In coop it is
built and host-tested; the op1emu gate and the device run are still to do.

| File | What |
|---|---|
| `tape_drive.cc` | coop's glue: the `coop_fx_t`, the knob mapping, the int32 to int16 bus and back |
| `src/tape.cc`, `src/tape.h`, `src/tape_layout.h`, `src/tape_tables.h` | te-op1-cfw's DSP. Only the include paths and one initialiser changed (below) |
| `src/tape_bfin.S` | te-op1-cfw's three kernels (stereo delay, mono delay, lane). Only the include path changed. `tools/asm_lint.py`: 0 flagged |
| `src/gen_tape_tables.py` | the offline table generator (plain Python, not built) |
| `op1/support.cc` | `memcpy` / `memset` / `memmove` for GCC's struct copies (the same file as `plugins/rings/op1/support.cc`) |
| `LICENSE-MIT` | Beads' WowAndFlutter terms (the wobble's design) |

Licence: the DSP and the glue are coldsoldr's (GPL-3.0-or-later with the OP-1 firmware permission). The wobble is
Mutable Instruments Beads' `WowAndFlutter` (MIT), rewritten in fixed point (`LICENSE-MIT`). The halfband uses hiir's
coefficient design (Laurent de Soras, WTFPL).

## Contract

### Action table

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | saturate | wobble | age (clockwise = newer) | mix |
| Click | — | — | — | — |
| Shift + turn | saturate fine (stock) | wobble fine (stock) | age fine (stock) | mix fine (stock) |
| Shift + click | — | — | — | — |
| fn + turn | gain, -6..+6 dB (continuous, saved) | — | — | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | **gain fine** (the same control, 1/16 step: `"fine": true`) | — | — | — |
| Shift + fn + click | — | — | — | — |

Blue's shift + fn layer is the gain's fine adjustment. This is the owner's decision (`docs/ui-features.md` §14) and
the e6 `"fine"` pairing (`guide/plugin-guidelines.md` §4.1), so nothing else can sit there.

### Controls (`plugin.json`)

```json
"controls": [
  {"id": "gain", "name": "gain", "action": "fn_turn", "encoder": "blue", "word": 5, "kind": "continuous",
   "default": 16384, "fine": true, "random": false}
]
```

- **The turns** are `params[0..3]`. They arrive in the filter type's range units
  ({64..14528}, {0..24576}, {0..22528}, {8192..32767}, `sdk/coop_plugin.h`).
  - The render maps them to 0..32767 with kennel's `op1_fx_param_q15` formula (`(v - lo) * 32767 / (hi - lo)`,
    clamped), so the DSP gets the same numbers it got on kennel.
  - Defaults are Q15 `[13107, 8192, 22937, 32767]`, as upstream: saturate 0.4, wobble 0.25, age 0.3 worn
    (word 0.7), mix full.
  - Age is mirrored as upstream: turning clockwise gives a newer, brighter tape (the owner, 2026-10-27).
- **The gain** is word 5: `gain_oct = (clamp(params[5]) - 16384) * 4`. That is Q16 octaves added to the saturation
  drive's exponent only (about -6..+6 dB, 16384 = 0 dB).
  - The level make-up and the dry side of the mix never see it, so at mix 0 the gain changes nothing.
  - It is not randomised (`"random": false`, the owner: a random draw would be a level jump).
- **Words 6, 7 and the latch word** are unused: upstream has nothing else.

### Upstream mapping, and what moved

| kennel (tape_knl.cc) | coop |
|---|---|
| knobs blue saturate, green wobble, white age (mirrored), orange mix | the same four turns |
| blue press `OP1_PRESS_PAGE`, page 2 blue = word 5 gain | `fn_turn` blue, word 5 (same word, same default 16384, same formula) |
| shift + blue on page 2: a tenth of a dB a click (`knl::Fine`, 273 words, written back from the render through the shift byte at `0x01dd7f78`) | shift + fn + blue, the core's fine: 20.5 Q15 a detent. Coarse is 328 a detent (about 0.12 dB); fine is about 0.0075 dB |
| green / white / orange presses | none (`OP1_PRESS_NONE`) |
| `"stereo": true` (`op1_fx_stereo_t`: mono render at synth and drum, stereo at master) | no flag. A `coop_fx_t` is stereo by ABI, and coop's loader would read a kennel stereo flag as the `op1_fx_stereo_t` layout and refuse the file. The stereo render runs at every position |
| `page2_defaults [16384, 0, 0]` | taken from the controls by the packer (never written next to `controls`) |
| `knl::Instance` (the position's scratch buffer) | one `TapeSat` in `.bss`: each of the five relocations has its own |
| `op1_fx_param_q15` (the stock range table at `0x01DE5C50`) | the SDK's constant ranges, once a block |
| the screen (`SCREEN_tape`, `tape_drive.svg`) | not ported yet (see "Screen") |

The page became a held layer (guidelines §4). The mix was already on orange (§4.3), so nothing moved.

## Stereo and cost

- **Stereo path.** The render calls upstream's `TapeSat::Process(l, r)` at every position:
  - one shared wobble delay (a ring of L | R pairs);
  - per channel, a lane: 2x halfband up, quintic soft clip, halfband down, head bump, HF loss, mix.
  - The channels are never summed. On kennel the synth and drum positions ran this mono, so in coop they now hear
    the effect in stereo.
- **The bus.**
  - coop hands int32 (the int16 bus widened by 16 bits) and the DSP is int16. Each block's high halves go through the
    DSP, and the output keeps the input's low halves: `out = wet16 << 16 | (in & 0xffff)`.
  - So the high half is bit-identical to upstream, and at mix 0 the whole int32 sample is the input (host test).
- **Upstream cost** (te-op1-cfw `docs/tape-port.md`, bfin-elf-run, per 64-sample call):
  - stereo 7,026 cycles, mono 4,128;
  - the stereo delay kernel 744, each lane 2,836, control about 200-400.
- **coop, simulator** (`tests/host/tape_drive`, bfin-elf-run, no cache or SDRAM model): **avg 9,061, peak 9,286 a
  block**, the glue included.
  - The glue is the int32 / int16 conversion (2 x 64 samples each way) and four divisions for the knob mapping
    (`__divsi3`).
  - So stereo at the synth and drum positions costs about 2.2x what the mono kennel render did there, and about 1.3x
    at the master.
  - Five instances (every position) would be about 46 k together.
- **Against the budget** (under 30 k a block, over 60 k does not ship):
  - Expected on the device: about 10-13 k (op1emu-style figures plus 10-35 %) [I].
  - Worst case with every SDRAM line it touches cold: about 60 lines x ~107 cycles, so up to about 16 k [I].
  - No optimisation pass is needed to ship. If the device figure comes in high, in this order:
    1. Fold the int16 conversion into the kernels: the delay kernel reads the high halves in place with a 4-byte
       stride, and the mix writes back the same way. That saves about 1 k.
    2. Replace the four divisions with exact reciprocal multiplies (keeping kennel's rounding). That saves about
       0.4 k.
    3. Run the mono half once when L == R. Not taken now: when the lanes' states differ, the output would differ from
       upstream for the length of a tail.
- **Memory.**
  - `.text` 9,416, `.rodata` 1,124, `.data` 0, `.bss` 3,372.
  - **Load size 13,912 bytes, x 5 relocations = 69,560 bytes** of heap 0.
  - The packed file is 11,064 bytes.
- **L1:** none. L1 for effects is still [proposed]; everything sits in `.bss` (SDRAM).
- **init:** `TapeSat::Init()`, which clears the 2 KB ring and sets about 40 fields: about 1.5 k cycles, cheap
  enough for the audio thread [I].
- **Device cost:** pending (the `cost` line `fx1_plugin=` / `s2_instr=` / `s2_master=`).

## Changes to the upstream DSP

- `src/tape.h`: `TapeParams::gain_oct` lost its `= 0` default member initialiser. It gave `TapeParams`, and so
  `TapeSat`, a non-trivial constructor, so a static instance could have needed `.ctors`, which coop refuses
  (guidelines §2.1). Every caller sets it.
- The `#include` paths: `src/tape/...` became `src/...`.
- Nothing else. The C reference kernels and the mono path stay: the host test uses them, and they are cold code on
  the device.

Forbidden constructs found in the upstream plugin (`tape_knl.cc`, `common/knl_fx.h`), and how each was replaced:

| Construct | Replaced by |
|---|---|
| `knl::ShiftHeld()` reading `0x01dd7f78`, and `knl::Fine` writing word 5 back through `op1_fx_knobs(pos)` | the core's `"fine"` |
| `op1_fx_param_q15` (`0x01DE5C50`) | constant ranges |
| `op1_fx_scratch` | `.bss` |
| `op1_fx_page`, `op1_fx_screen_pos`, `op1_fx_knobs_live`, `op1_screen_draw` | screen only, not ported |

The DSP itself had no float, no libc call, no address and no per-sample division. The only `float`, `sin` and `exp`
are in comments and in the offline generator script.

## Screen

The stock filter page shows until coop has an effect-screen hook (`docs/plugins.md` "Effect controls", "Display").
The core already shows `GAIN` and its value top left in blue for about a second on fn + blue.

Brief for the coop screen, against the manifesto (guidelines §5). It follows te-op1-cfw's design A, "Reels,
aligned" (2026-10-27, `gen_tape_drive_knl_svg.py`), which is not carried across here. Mock it in the playground
first.

- **Blue:** the saturation, as a wave on the tape between the reels (8 poses, switched by visibility).
- **Green:** the wobble, as the hubs jittering at the wobble's rate (0.5 Hz wow up to 8 Hz flutter), plus a marker on a
  wow to flutter scale.
- **White:** the age, as a low-pass glyph whose corner walks left and dims, plus hiss specks counting up.
- **Orange:** the mix, as a bar scaled from its foot.
- **The gain** is a bar about the centre, in blue.
  - Its bracket and readout (sign, dB, tenths) show only while fn is held (`coop_scr_mods`), not on kennel's page-2
    toggle.
  - No words at rest.
- Upstream's polyline reels had a turn-back that R246 draws as a spike (te-op1-cfw Test 52 notes). Check the new art
  with `pack.py`'s turn-back check.

## Gate plan (op1emu, step 9: not run here; one run at a time)

1. Build: `make -C plugins` gives `build/plugins/tape_drive.coop`. Build the NAND with `tools/make_nand.py`, with
   the `.coop` alone in `/yaffs2/plugins`.
2. `tests/sim/det_fx_select.txt`-style runs:
   - Select `tape drive` in the FX list (alphabetical) at FX 1 of the synth, the drum and the master, and at FX 2 of
     the instrument and of the master.
   - Play a chord through each.
   - Expect: no fault; the stats `plugins` line shows `seen 1 loaded 1 failed 0` and tape_drive rendering at each
     position; the `cost` line shows `fx1_plugin=` / `s2_instr=` / `s2_master=` near 9 k (emulator).
3. `tests/sim/det_fx_controls.txt` (the e6 walk), unchanged, with tape_drive alone on the NAND. It picks the
   stock list's last row and, for FX 2, the same list turns as for test_fx: with one plugin on the NAND it is the
   same row [I]. Expect:
   - `fxctl fx 1`;
   - word 5 exactly as for test_fx: `p0 ...:15092/0/0/0`, `p3 ...:17040/0/0/0`, `p2 ...:16056/0/0/0`,
     `p4 ...:16384/0/0/0`, `p1 ...:16712/0/0/0`. tape_drive declares nothing on words 6 and 7 or the latch word, so
     they read 0, and test_fx's green and orange clicks and fn + white land in `ignored` instead;
   - `fine 1`, `master_saves 1`, `plugins seen 1 loaded 1 failed 0`, no fault.
4. `tests/sim/det_fx_reload.txt`: power-cycle the NAND and expect word 5 kept at every position.
5. A pristine-NAND run (`det_chord.txt`) must be unchanged against the same core.
6. Read the emulator cost figures as emulator figures: the device is 10-35 % higher.

No new scenario file is needed: the e6 scenarios pick the lone plugin by its list position, not by name. If FX 2's
list puts `tape drive` on a different row from test_fx [I], adjust the list turns in a copy named
`tests/sim/det_fx_tape_drive.txt`.
