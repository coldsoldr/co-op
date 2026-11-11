# tape_echo

A Space Echo style tape delay as a coop effect: one tape loop per channel, a record head and three playback heads at
1/3, 2/3 and 1 of TIME, each repeat going round the record amp, a low cut, the tape loss and the tape saturation
again; wow and flutter after Mutable Instruments Beads; TIME glides like a tape motor (the echoes bend, never click).
The DSP is te-op1-cfw's fixed-point TAPE ECHO (`ports/fx/src/tape_echo`, design and measurements in te-op1-cfw
`docs/tape-echo-port.md`), as the hundekraft kennel plugin ran it (`ports/knl/tape_echo` 0.2, commit 5aa314b): 2:1
down to 22.05 kHz through Clouds' halfband filters, the echo, back up, an equal-power mix with the dry signal and a
soft limit. Stereo at every position.

Licence: coldsoldr's GPL-3.0-or-later (with the OP-1 firmware permission) for the plugin and the echo; the wow and
flutter (Beads) and the half-rate output stage (Clouds) follow Mutable Instruments' MIT designs (`LICENSE-MIT`).

## Controls (guidelines §3.2)

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | time (30 ms .. 1.5 s) | feedback (0 .. 1.1) | age | mix |
| Click | — | hold (latch) | heads (3, 2+3, 1+2+3, 1+3) | — |
| Shift + turn | time fine | feedback fine | age fine | mix fine |
| Shift + click | — | — | — | — |
| fn + turn | gain (-6 .. +6 dB, 0.5 dB a detent) | sync (off, 1/16T .. 1/2) | — | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | gain fine (0.1 dB a detent) | — | — | — |
| Shift + fn + click | — | — | — | — |

- **Turns** (`params[0..3]`, kennel's four knobs unchanged): time is head 3's delay, 30 ms x 50^x (exponential);
  feedback is the loop gain, 1.0 at 91 % (above it the tape self-oscillates, the saturation bounds it); age wears the
  tape (wow and flutter depth, the loss from 9 kHz to 1.8 kHz, the low cut from 40 to 110 Hz, the drive 1..2); mix is
  dry to wet. The core hands them in the filter type's range units (`sdk/coop_plugin.h`); the plugin normalises them
  to 0..32767 once a block as kennel's `op1_fx_param_q15` did. Shift + turn is the stock fine step on each.
- **gain** (fn + blue, word 5, continuous, default 16384 = 0 dB, saved, not randomised): what reaches the record head;
  the dry side of the mix doesn't see it. **Blue's shift + fn layer is gain fine** (the owner's decision,
  `docs/ui-features.md` §14): `"fine": true`. Steps (the owner, 2026-11-11, dev 50e7dbd): `"step": 1365` (0.5 dB a
  detent, 2730.7 Q15 a dB over the +-6 dB range; kennel's page click was 1536, 0.56 dB) and `"fine_step": 273`
  (0.1 dB, kennel's `knl::Fine` tenth of a dB).
- **sync** (fn + green, word 6, steps, saved, no wrap, randomised, default `off`): kennel's tempo lock. `off`: the
  blue turn sets the time. `1/16t`, `1/16`, `1/8t`, `1/16.`, `1/8`, `1/4t`, `1/8.`, `1/4`, `1/2t`, `1/4.`, `1/2`
  (shortest first): the time follows that division at the tempo, and the blue turn does nothing to the time until sync
  is back on `off` (its value is kept). The time glides to a new division like any time change. **The tempo is 120
  BPM** (a 16th 125 ms, a half note 1 s) until the core exports the project tempo
  (`docs/request-tempo-export-tape-echo.md`): kennel's own fallback, guidelines §2.3. At a tempo where a division is
  over 1.5 s (1/2 below 80 BPM, 1/4. below 60, 1/2t below 54) the time falls back to the turn, as kennel skipped such
  divisions. Arithmetic as kennel's (`DivisionWord`, `TapeEchoTimeWord`): the division's delay as a time word.
- **hold** (green click, latch bit 0 of `params[8]`, not saved, never randomised): the input off, the loop gain exactly
  1, a quarter of the tape loss and a 10 Hz low cut: the tape loops what it holds, slowly fading. **Latches clear
  when the stock switches between synth and drum** (the stock starts the instrument effect again; accepted by the
  owner, `docs/ui-features.md` §14), and whenever the effect is selected or the sound changes.
- **heads** (white click, word 7, steps, saved, wraps, randomised, default `3`): the head pattern, one click a
  pattern: head 3 alone; heads 2 and 3; all three; heads 1 and 3. A change ramps the head weights over 8 calls
  (11.6 ms).

`plugin.json` "controls":

```json
{"id": "gain", "name": "gain", "action": "fn_turn", "encoder": "blue", "word": 5, "kind": "continuous",
 "default": 16384, "fine": true, "step": 1365, "fine_step": 273, "random": false},
{"id": "hold", "name": "hold", "action": "click", "encoder": "green", "kind": "latch", "bit": 0},
{"id": "sync", "name": "sync", "action": "fn_turn", "encoder": "green", "word": 6, "kind": "steps",
 "steps": ["off", "1/16t", "1/16", "1/8t", "1/16.", "1/8", "1/4t", "1/8.", "1/4", "1/2t", "1/4.", "1/2"],
 "default": 0},
{"id": "heads", "name": "heads", "action": "click", "encoder": "white", "word": 7, "kind": "steps",
 "steps": ["3", "2+3", "1+2+3", "1+3"], "default": 0, "wrap": true}
```

No `"stereo"` flag: a `coop_fx_t` is stereo by its ABI, and coop's loader reads kennel's flag as the
`op1_fx_stereo_t` layout and would refuse the file.

**Upstream mapping** (kennel `tape_echo_knl.cc`):

| Kennel | coop | Why |
|---|---|---|
| knobs blue time, green feedback, white age, orange mix | the same four turns | the owner kept them |
| blue press: page 2, blue turns the input gain (word 5); shift + blue a tenth of a dB (`knl::Fine`, reading the shift byte at 0x01DD7F78 and rewriting the stored word) | fn + blue (0.5 dB), shift + fn + blue its fine (0.1 dB) | every layer is held (guidelines §4); the core does the fine step |
| green press: hold, `OP1_PRESS_LATCH` (`op1_fx_page[pos][1]`) | green click, latch bit 0 | guidelines §7.1 |
| white press: page 2, white clicks the head pattern (word 7, `ClickMode(word, 4)`: 1536 a click) | white click, steps, word 7 holds the step index | enumerations are steps (§3.1); a sound saved by kennel decodes clamped (word 7 >= 3 plays as `1+3`) |
| shift + blue on page 1: a shifted click walked the time through the tempo divisions (1/16T .. 1/2; the tempo read at 0x01AADAD4); a free turn unlocked it; not saved | fn + green sync, steps (`off` + the 11 divisions), word 6, saved; 120 BPM until a tempo export | the owner, 2026-11-11: shift + turn is the core's fine step and the plugin never sees shift; a sync setting belongs to the sound |
| `op1_fx_param_q15` (the stock range table at 0x01DE5C50) | the SDK's constant filter ranges, once a block | no firmware addresses in a plugin (§2.1) |
| the instance in the position's scratch buffer (`op1_fx_scratch`) | `.bss` (one instance per relocation, i.e. per position) | §2.3 |
| `TapeEcho::Init` clears the 133 KB tape on every (re)start, in the audio interrupt | the clear is spread over the first 17 blocks (2,048 pairs a block, 25 ms), the dry passing through | a slow init doesn't ship (§2.3) |
| the screen (`tape_echo.svg`, design B "Schematic") | the stock filter page; coop shows the control's name and value top left | effect screens are not built in coop (`docs/plugins.md` "Effect controls"); brief below |

Time already has a fine step (shift + blue), so there is no separate delay-time fine control.

## Screen

The stock filter page shows (effect screens are not built). Brief for later, against the manifesto (§5), from
kennel's design B: the tape row with the record head and three play heads, their distance from the record head
following time (blue), lit per head pattern (white, steps); a dot travelling record head to head 3 at the tape speed;
the tape drawn as a wave whose amplitude follows age (white) and breathes with the wow; feedback as four chevrons
(green), all four and the return path in green while hold is on (the latch shows it is engaged, §5.4); mix as a bar
(orange). No words at rest; the gain readout (dB, tenths) only while fn is held. Kennel's art is not carried across.

## Stereo, memory, cost

- **Stereo:** the upstream's own stereo path (`render_stereo`, kennel ran it only at the master): one tape of L | R
  pairs, every head read serving both channels, so stereo costs about 1.56x the mono path rather than 2x (te-op1-cfw
  simulator: 9.2 MHz stereo, 5.9 MHz mono, whole effect). coop runs it at every position (synth, drum, master and both
  FX 2 slots): the instrument positions now hear the echo in stereo, and each position is a full stereo instance.
- **Memory:** file 10,664 bytes; `.text` 5,836, `.rodata` 3,052, `.data` 0, `.bss` 134,764 (the tape 132,868, the
  core's state, the half-rate filters, the dry buffers). Load size 143,652 bytes; five relocations: **718,260 bytes of
  heap 0** (heap_largest was ~9 MB after the plugin scan in e4). No imports.
- **L1:** none (effects have no L1 lease yet, [proposed]). The hot data per sample is the kernel block (88 bytes), the
  six head reads and one write across the 133 KB tape, the soft-limit table (1 KB) and the saturation table.
- **Cost:**

  | | per block (64 samples) |
  |---|---|
  | kennel stereo, te-op1-cfw simulator (9.2 MHz / 689 blocks a second) | 13,350 |
  | coop, GNU simulator, `make -C tests/host/tape_echo sim` (compute only, the scenario's 3,000 blocks) | 15,880 avg / 16,637 max |
  | coop, op1emu (`fx1_plugin=`, `s2_instr=`, `s2_master=`) | gate pending |
  | **coop, device** | **pending** |

  The ~2.4 k over kennel is the Q31 <-> int16 conversion of the bus, the dry copy and the clearing blocks averaged in.
  The device was 1.5-4x the simulator for kennel's effects (cache misses: the head reads span 133 KB of SDRAM), so
  expect 20-35 k a position on the device [I]: near the 30 k line, under 60 k. Five positions all running tape_echo
  would be ~80-160 k of the 580,499 budget [I].
- **Optimisation plan** (after the device figure, `docs/plugin-performance.md`): prefetch the three heads' tape lines
  a few lines ahead in the kernel (they advance about one pair a sample, so one line serves 8 samples); keep the
  kernel block in registers across the loop instead of loading and storing it every sample (the kernel touches
  `[P5 + ...]` some 20 times a sample); fuse the int16 -> Q31 output into the mix kernel; an L1 home for the kernel
  block and the soft-limit table once effects get a lease. The mono kernel for L == R is not an option: it records
  only the left half of the tape, and the right half would be stale when width returns.

## Tests

- `make -C tests/host/tape_echo`: the plugin against te-op1-cfw's own DSP and the kennel glue, 20,000 random
  blocks (noise, saws, impulses, silence, L != R; knobs walking and jumping; feedback over 1; gain, sync, heads and
  hold changing; restarts by pause and by re-init), on the host: **bit-identical** (11,521 blocks changed by the
  effect, 9,194 with sync on). The reference takes sync from kennel's own lock (`TimeLock::Word`, `DivisionWord`,
  `kDivision96ths`) at its 120 BPM fallback, and both builds print the 11 division time words at 120 BPM
  (8543 11940 14362 15350 17746 20156 21142 23565 25961 26948 29358), which must agree.
- `make -C tests/host/tape_echo sim`: the same in the Blackfin simulator, the plugin's assembly kernels against the
  original's C: **bit-identical** over 3,000 blocks, plus the cycle figures above.
- `make -C tests/host/tape_echo tables`: regenerates `src/tape_echo_tables.h` from the original's constexpr tables.
- `tools/asm_lint.py plugins/tape_echo/src/*.S`: 0 packets flagged (neither kernel has a parallel packet).

**op1emu gate (not run yet: gate runs go one at a time):**

```sh
make -C plugins                                                     # build/plugins/tape_echo.coop
mkdir -p build/nand_te/plugins && cp build/plugins/tape_echo.coop build/nand_te/plugins/
python3 -I tools/make_nand.py --provision ~/op1emu-work/provision_nand.py --firmware ~/Downloads/op1_246.op1 \
    --mkyaffs2 ~/op1emu-work/yaffs2utils/mkyaffs2 --overlay build/nand_te --output ~/op1emu-work/plug_te.bin
```

1. `tests/sim/det_fx_select.txt` on that NAND (the e6 image): tape_echo at the synth position, a chord through it;
   `plugins seen 1 loaded 1 failed 0`, no fault, the `cost` line's `fx1_plugin=` figure; the WAV has echoes.
2. `tests/sim/det_fx_tape_echo.txt` (new: `det_fx_controls.txt`'s walk with tape_echo's controls): the plugin at p0
   synth FX 1, p3 instrument FX 2, p2 master FX 1, p4 master FX 2, p1 drum FX 1; expected `fxctl` per position in the
   file's header (p0 11197/0/1/0 at the end, p1 17749/0/1/0, p2 15019/0/0/1, p3 19114/0/1/0, p4 16384/3/2/0 with sync
   on `1/8t`; turns 11, fine 1, clicks 5, latches 4, ignored 2, cancelled 1), no fault, `s2_instr=` / `s2_master=`.
3. `tests/sim/det_fx_reload.txt` on the same NAND: gain, sync and heads kept at every position, hold off everywhere.
4. A pristine-NAND `det_chord.txt` unchanged against e6.

Read the stats cost line as an emulator figure (the device is 10-35 % higher, more with SDRAM traffic).
