# fickle

Two processors that feed each other, sharing one clock. The design is inspired by the signal flow of a two-channel
micro-looper pedal. The input splits three ways:

- the **dry** path, untouched, to the mix;
- the **routing** switch, into the **wet channel**: reverb, delay or slip, with a freeze;
- the **micro-looper**, which is always listening.

While its latch is off, the looper records the most recent audio (the input plus the wet channel's output) round a
circular buffer. Turning the latch on plays back the last loop's worth, and the dub latch overdubs onto it.

Both channels run at the **clock**: one sample rate, from 32 kHz down to 2 kHz. A lower clock means longer loops and
delays, darker and grittier sound, and, as on tape, recorded material changes pitch when the clock moves.

Licence: coldsoldr's GPL-3.0-or-later with the OP-1 firmware permission. The plugin is coop's own: no upstream code.

```
            +-------------------------------------- dry --------------------------------------+
            |                                                                                  |
 input -----+--> routing --> WET CHANNEL (reverb | delay | slip, freeze) --+--> wet ---------+ |
            |      ^  (in / both / loop, while the loop plays)             |                 | |
            |      |                                                       v                 v v
            +------|--> MICRO-LOOPER (listening: records input + wet;      +-> loop ------> mix --> soft limit --> out
                   |      loop on: plays; dub: adds the input, never the wet)    (equal power)
                   +---------------- loop ------------------------------+
            all of it inside the dashed box runs at the CLOCK (2 .. 32 kHz), sample-and-hold in, smoothed out
```

## Controls (guidelines §3.2)

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | time (wet) | modify (wet) | clock (2 .. 32 kHz) | mix |
| Click | — | dub (latch) | loop (latch: off listening, on playing) | freeze (latch) |
| Shift + turn | time fine | modify fine | clock fine | mix fine |
| Shift + click | — | — | — | — |
| fn + turn | wet mode (reverb, delay, slip; wraps) | routing (in, both, loop) | loop mode (13 steps, wraps) | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | — | — | — | — |
| Shift + fn + click | — | — | — | — |

- **Turns** (`params[0..3]`, normalised to 0..32767 from the filter type's range units once a block, as the other ports
  do). Defaults: time 14000, modify 12000, clock 29372 (24 kHz), mix 16384 (50 %).
  - **time** and **modify** belong to the wet mode (below).
  - **clock** is the sample rate of both channels: 2 kHz x 16^x, exponential, eased over about 4 blocks.
    - The input goes through a gentle one-pole pre-filter at 0.42 x the clock, then is sampled by a phase accumulator.
      The pre-filter is too gentle to stop aliasing at low clocks: that is deliberate, the grit.
    - On the way back to 44.1 kHz, each clock sample is held and smoothed by two one-poles at 0.45 x the clock: a
      gentle anti-image filter.
    - Buffers are written and read at the clock rate. Turning the clock bends the pitch of whatever is already
      recorded, and a lower clock gives longer loops and delays.
  - **mix** is dry against both channels (wet + loop), equal power. The output is soft-limited: unity up to -2.5 dBFS,
    then a parabola to full scale at +1.9 dBFS over it.
- **wet mode** (fn + blue, word 5, steps, wraps, default `reverb`). A change dips the wet channel's output over 32 clock
  samples, switches, and fades back in.
  - **reverb**
    - Signal path: the input and the crossed-over feedback (L's tail returns on R) go through four allpass diffusers
      (113, 167, 229, 311 clock samples) into the line. Six taps spread across the size are read back, each side
      with its own signs, then damped and fed back.
    - *time* sets the size (512 .. 16,300 clock samples) and, with it, the RT60: 0.5 s .. 10.6 s at 24 kHz, longer as
      the clock drops. The round trip's decay comes from the two.
    - *modify* is the diffusers' gain (0 .. 0.72): from a clean multi-tap echo cluster to a washed reverb.
    - *freeze*: the line circulates exactly (the write copies the sample one size back) and the size holds. The last
      size's worth of the tank, read by the taps, holds as a pad indefinitely.
  - **delay**
    - *time* is 128 .. 16,320 clock samples (5 ms .. 0.68 s at 24 kHz; 8 s at 2 kHz), exponential. A change
      crossfades to the new time over 256 clock samples, so existing echoes keep their pitch instead of bending.
    - *modify* is the feedback (0 .. 1). From about 97 % it is 1: the line holds like a looper, sound on sound under
      the soft knee. The damping opens up with it.
    - *freeze*: the input off, feedback 1, no damping. The echo loops indefinitely.
  - **slip**
    - The input is written round the line continuously. Every refresh, a head jumps back and replays the latest audio
      at a speed, while the old head fades out over 256 clock samples.
    - *time* is the refresh: 512 .. 16,384 clock samples, shortened where the speed needs the room.
    - *modify* is the speed and direction in 14 steps: reverse x2, x1.5, x1, x0.75, x0.5, then forward x0.5, x0.67,
      x0.75, x0.89, x1, x1.12, x1.33, x1.5, x2 (octaves, fifths, fourths, a tone).
    - Forward at x1, slip is a plain delay of one refresh. Elsewhere it is a stutter or a pitch-shifted grab.
    - *freeze*: nothing is written, and every refresh starts from the same place: a repeating slip tone.
- **routing** (fn + green, word 6, steps, no wrap, default `both`) sets what the wet channel hears while the loop
  plays: `in` the live input, `both` input + loop, `loop` the loop alone. While the loop is off (listening), the wet
  channel hears the input regardless. A change ramps over 32 clock samples.
- **loop mode** (fn + white, word 7, steps, wraps, default `tape 1`). A change while playing dips the loop over 64 clock
  samples.

  | Step | Name | What the loop does |
  |---|---|---|
  | 0 | `tape 1` | forwards at the recorded speed |
  | 1 | `tape 2` | forwards, double speed (an octave up) |
  | 2 | `tape .5` | forwards, half speed (an octave down) |
  | 3 | `tape 1.5` | forwards, x1.498 (a fifth up) |
  | 4 | `rev 1` | backwards |
  | 5 | `rev 2` | backwards, double speed |
  | 6 | `rev .5` | backwards, half speed |
  | 7 | `ping` | forwards, then backwards at each end |
  | 8 | `env soft` | playing over -29 dBFS restarts the loop from its start (a 64-sample crossfade); its level follows your playing and fades in ~0.35 s once you stop |
  | 9 | `env hard` | playing over -21 dBFS jumps to the next of 8 slices; full level |
  | 10 | `stretch` | two overlapping 2048-sample grains read the loop at x1 from a scan moving at x0.5: twice as long, same pitch |
  | 11 | `str slow` | the scan at x0.125: almost frozen |
  | 12 | `str rev` | grains read backwards, the scan at x0.5 |

- **loop** (white click, latch bit 1 of `params[8]`).
  - Off is *listening*: the looper records input + wet at -3 dB round its 24,576-sample buffer (1.02 s at 24 kHz,
    12.3 s at 2 kHz).
  - On is *capture and play*: the last loop's worth becomes the loop. That is at most 24,064 clock samples, the buffer
    less the seam's pre-roll.
    - The loop's end crossfades into the 256 samples before its start (baked over two blocks), so it wraps without a
      click in either direction.
    - Playback fades in over 128 clock samples.
  - Off again fades the loop out over 128 clock samples and returns to listening. The next capture replaces the loop.
  - The loop joins the wet channel at -3 dB in the output.
- **dub** (green click, latch bit 2) **acts only while the loop plays.**
  - The input, never the wet channel (so there is no runaway), is added at a dub head that runs through the loop at x1
    from where the read head was when the dub started.
  - The old material fades x0.92 a pass, with a soft knee.
  - At other speeds you hear the overdub transposed on the next pass, as on tape.
- **freeze** (orange click, latch bit 0) holds the wet channel, as in each mode above.

Latches are never saved. The core clears them whenever the effect is selected at a position or the sound changes, and
when the stock switches between synth and drum (accepted for every effect, `docs/ui-features.md` §14).

`plugin.json` "controls":

```json
{"id": "freeze", "name": "freeze", "action": "click", "encoder": "orange", "kind": "latch", "bit": 0},
{"id": "loop", "name": "loop", "action": "click", "encoder": "white", "kind": "latch", "bit": 1},
{"id": "dub", "name": "dub", "action": "click", "encoder": "green", "kind": "latch", "bit": 2},
{"id": "wet", "name": "wet", "action": "fn_turn", "encoder": "blue", "word": 5, "kind": "steps",
 "steps": ["reverb", "delay", "slip"], "default": 0, "wrap": true},
{"id": "routing", "name": "routing", "action": "fn_turn", "encoder": "green", "word": 6, "kind": "steps",
 "steps": ["in", "both", "loop"], "default": 1},
{"id": "loopmode", "name": "loopmode", "action": "fn_turn", "encoder": "white", "word": 7, "kind": "steps",
 "steps": ["tape 1", "tape 2", "tape .5", "tape 1.5", "rev 1", "rev 2", "rev .5", "ping", "env soft",
           "env hard", "stretch", "str slow", "str rev"], "default": 0, "wrap": true}
```

No `"stereo"` flag: a `coop_fx_t` is stereo by its ABI.

## Screen (e9)

The owner's design "Fickle" (the canvas mock-up's `renderVals()`), on the e9 effect-screen path: `fickle.svg` (from
`gen_fickle_svg.py`) and `fickle_screen.cc`. It shows a two-footswitch stompbox in 3/4 view, drawn as wireframe, that
wobbles and **leans towards whichever channel is louder** (loop level against wet level).

**The clock is the screen's frame rate.** The pose, which is the time every motion uses, is sample-and-held at
4 + 56 x clock² Hz. At a low clock the pedal moves in steps, like a low sample rate. Blinks run on real time.

| Part | Encoder | What moves it |
|---|---|---|
| the blue, green and orange knobs' pointers | blue (time), green (modify), orange (mix) | the turn, -135 .. +135 degrees |
| the big white knob's pointer; the wobble's speed; the pose's sample rate | white (clock) | the turn |
| the left footswitch (pressed), its LED (lit) | orange click (freeze) | the latch |
| the wet glyph (left half): pencil, orange and still while frozen | blue with fn (wet mode), green (modify), blue (time) | reverb: seven dots in a sunflower, spread with modify, turning. Delay: five bars drifting right a step per delay time (the display word), smaller and dimmer with modify. Slip: a zigzag with an arrowhead for the direction, its height with modify's distance from noon |
| the routing lever between the halves | green with fn (routing) | leaning to the wet side (`in`), up (`both`), to the loop (`loop`) |
| the right footswitch (pressed), its LED (lit; blinking at 2 Hz while dubbing) | white click (loop) | the loop state (display word) |
| the loop ring: white's dark tone while listening, white while playing | white click (loop) | the loop state |
| the ring's 24 ticks | white | playing: the loop's envelope as heard (24 bins, a display word). Listening: a shimmer in the body tone that changes at the pose rate |
| the head dot | white | the loop position (display word): the write head (pencil) while listening, the read head (white) while playing. It runs backwards in reverse and jumps between slices for env |
| the green ring inside the loop ring | green click (dub) | only while dubbing (dub acts only while playing) |
| the glyph in the ring | white with fn (loop mode) | tape and ping: two reels whose spokes turn at the speed (backwards in reverse). Env: a spike, taller for hard, green for 120 ms on each retrigger. Stretch: three strokes, longer for `str slow` |

How it is drawn:
- **Every flat part is a group in its own plane's coordinates**, moved by one screen-space matrix a frame
  (`SetMatrix6`). The parts are the faces, the top face's art, the knob and footswitch tops, and the knob sides (a
  rect across the cylinder's silhouette, in its vertical plane).
  - The matrix is M(O, U, V) = [P(O+U) - P(O), P(O+V) - P(O), P(O)] under the orthographic projection, with el 1 rad.
  - U and V are always unit vectors, so a group is never scaled up. A stroke stays at about 1.5 units whether or not
    R246 scales strokes with a node's matrix. **I could not check which R246 does: the gate should look.**
  - No nested transforms. The stretch glyph is a group inside the top face's group, but it carries no matrix of its
    own (only visibility).
- **Dots** (the loop head, the lever's tip, the reverb dots) are translated in screen space.
- **Raster lines** (`coop_scr_line`), drawn over the SVG every frame: the pointers and the lever (stroke 2), the ring's
  ticks, the reel spokes, the delay bars, the slip zigzag.
- Fixed point throughout: `SinQ15` / `CosQ15`, Q10 matrices, model points in Q6.
- The box sits at y 86 (the mock-up's 80, 6 lower). Its back edge clears the core's popup corner top left, and the
  front edge ends above y 140.
- Colours:
  - the four brights and their dark tones: blue 2B3A70, green 00573A, white 5C5870, orange 6E1A2B (the mock-up's;
    tape_echo uses the same green and white darks);
  - the pencil DFD9FF for the box;
  - the body tone 38354D for the vents, the seam and the footswitch nuts;
  - 010101 face fills.
- SVG 6,685 bytes, 44 ids. The generator checks ids, decimals, the element subset, no transforms, no turning
  polylines, and the 8 KiB limit.
- **Frames for the owner without op1emu**: `make -C tests/host/fickle screens` writes PNGs of four scenes to
  `build/fickle_screens/`: listening; loop + freeze in delay; dub at a low clock in slip with env hard; and str slow
  with routing `in`.
  - The test drives the real plugin into each scene, runs the screen with a recording stand-in for the core's screen
    calls (`tests/host/fickle/screen_rec.cc`), and `render_frames.py` draws `fickle.svg` with what it recorded.
  - It is a picture of the drawing (no anti-aliasing, strokes not scaled), not of R246's renderer.

**Display words** (`state.h` `Display`, at the end of the state). The render writes them once a block, after the DSP,
the clearing blocks included. The screen reads them through `Published<>` while `view->live`:

| Word | Meaning |
|---|---|
| `clock_hz` | the clock in force, Hz |
| `loop_pos` | 0..32767 of the loop: the read head while playing, the write head's place in the buffer while listening |
| `loop_len` | the loop's length in clock samples; while listening, what a capture would take now (0 = not enough yet) |
| `loop_state` | 0 listening, 1 playing, 2 dubbing, 3 fading out |
| `wet_level`, `loop_level`, `in_level` | the block's peaks, 0..32767 |
| `head` | delay: the time over the line; slip: the head's travel through its refresh; reverb: the size over the line (0..32767) |
| `freeze` | 1 while the wet channel is frozen |
| `trig` | counts the env retriggers (a change is a hit) |
| `wet_mode` | the wet mode in force (after the switch's fade) |
| `env[6]` | the loop's envelope as heard: 24 bytes, a 24th of the loop each |

## Stereo, memory, cost

- **Stereo**:
  - Every buffer holds L | R int16 pairs, so one 32-bit load reads both channels.
  - The reverb's feedback crosses over, and its taps have different signs on L and R, so a mono input comes out wide.
  - It runs in stereo at every position.
- **Memory (e8)**: `"scratch": 171936` (the state is 171,920 bytes; static_assert):

  | Part | Bytes |
  |---|---|
  | the loop buffer (24,576 pairs) | 98,304 |
  | the wet line (16,384 pairs) | 65,536 |
  | the four diffusers | 4,608 |
  | this block's clock-sample buffers | 3,148 |
  | the rest (fields, display words) | ~324 |

  - The **loop buffer is never cleared**: the looper reads only what it recorded (a capture needs 1,280 clock samples
    recorded first).
  - The wet line and the diffusers are cleared over the first five blocks (4,096 words a block, 7 ms) with the dry
    passing.
  - `init` only resets the small fields and binds the buffer. The `.bss` holds the five pointers; the screen's node
    tables take the rest of its 276 bytes.
  - File 35,616 bytes: `.text` 22,036 (render 14.4 KB with everything inlined, screen 5 KB), `.rodata` 1,686,
    `.data` 4, `.bss` 276.
  - **The position buffers grow** to this declaration if fickle is the largest installed scratch effect: 5 x 171,936
    against tape_echo's 5 x 134,784, i.e. **+185,760 bytes of heap 0**. That is under `COOP_FX_SCRATCH_MAX`, but it
    counts against the e7 budget (see "For the owner").
- **L1**: none (effects have no lease yet).
- **Cost** (GNU simulator, compute only, `make -C tests/host/fickle sim`, cycles a block of 64):

  | Scene | Clock 24 kHz (default) avg / max | Clock 32 kHz (top) avg / max |
  |---|---|---|
  | reverb, listening | 20,333 / 20,626 | 23,942 / 24,369 |
  | reverb, frozen | 15,129 / 15,239 | 17,131 / 17,315 |
  | delay, listening | 13,226 / 15,070 | 14,728 / 16,981 |
  | slip, listening | 14,253 / 16,767 | 16,295 / 19,626 |
  | reverb + loop tape / rev / ping + dub, routing both | 24,785 .. 25,071 / 38,310 | 29,674 .. 30,647 / 44,058 |
  | reverb + loop env soft / hard + dub | 28,172 / 45,133 | 34,802 / 53,001 |
  | reverb + loop stretch / str slow / str rev + dub | 27,589 .. 28,004 / 41,073 | 33,373 .. 33,523 / 47,668 |
  | slip + str rev + dub | 21,576 / 34,844 | 26,358 / 39,783 |

  - Every max is the capture block. That block bakes half the seam, and the loop's fade-in, the routing's ramp and the
    dub's ramp all start in it. It happens once a capture.
  - Below 24 kHz the cost falls with the clock, since the clock-domain work is proportional to it (about 47 clock
    samples a block at 32 kHz, 35 at 24 kHz, 3 at 2 kHz).
  - **Over the 30 k SHOULD:** the loop playing with the dub on and the clock above about 26 kHz (to ~35 k at 32 kHz
    with env or stretch). The reason: two full processors at once, at up to 32 kHz each. Listening, freezing, or the
    default clock stay under 30 k. Every scene stays under the 60 k line.
  - The device is typically 10-35 % above the simulator, more with SDRAM misses. The heads walk the 160 KB of
    buffers sequentially, so a cache line serves 8 clock samples.
  - **Device figures: pending.**
- **How it got there** (`make -C tests/host/fickle prof`: each pass's cycles):
  - Every product in a per-sample loop is a single 16 x 16 multiply. GCC drops `int16` casts once it can prove the
    range (after a saturation) and then emits the multi-cycle 32 x 32 multiply, so on the Blackfin the multiply is the
    instruction itself in inline asm (`M16`, `MulSub`: the saturating 16-bit subtract and the multiply).
  - The reverb and the delay work in **block passes**. Every read they make reaches further back than a block's clock
    samples (taps from 57, diffusers from 113, delay from 128; a block is at most 47), so each ring is read and written
    in at most two runs a block, and the arithmetic runs on plain arrays with no per-sample masks.
  - The 44.1 kHz passes run one channel at a time, so the state stays in registers.
  - The clock's ticks are found without a branch.
  - The soft knee's common case falls through.
  - Steady states skip their ramps: the loop at full level, the dub at full level, the routing settled.
  - From the first version (reverb + loop + dub 55-63 k at 32 kHz) to this: about -45 %.
- No assembly files (nothing for `tools/asm_lint.py`). The two inline instructions are single packets with no parallel
  slots.

## Tests

`tests/host/fickle` (`fickle_test.cc`) tests the plugin as it ships, on the host and in the Blackfin GNU simulator.

- `make -C tests/host/fickle` (`BLOCKS=60000`, `SEED=1`) runs stress, holds and the e8 check:
  - **Dirty buffers:** after `init` on a position buffer full of random bytes, silence in gives exactly silence out
    (8 runs x 3,000 blocks, with every latch toggling, mode changes and clock jumps). Nothing the effect reads was left
    there by another effect.
  - **Stress:** random blocks drive the effect.
    - Sources: noise, saws with L != R and L == R, impulses, notes, noise bursts, silence.
    - Every wet mode, routing and loop mode.
    - Knobs walking and jumping, clock sweeps included.
    - Latches toggling in every order, and re-inits on dirty buffers.
    - Checks: the output's low bits, and the display words' ranges.
    - Every few segments, a **decay check**: silence in with freeze and loop off must reach silence (peak under 2 LSB,
      held longer than the longest delay) and then show no DC.
    - Seeds 1-4 at 100,000 blocks pass. The longest decay was 77 s: a long delay with heavy feedback at a low clock.
  - **Holds:** for every wet mode x loop mode, a captured loop keeps playing over silence. There is no sustained full
    scale and no garbage-sized jumps, and it returns to silence when released. A freeze keeps each wet mode sounding
    over silence and decays when released.
  - **e8:** `coop_fx_scratch_check` (slot 0 beside slot 3, and a restart on a dirty buffer: identical) in two
    settings.
- `make -C tests/host/fickle wav` writes 16-bit stereo WAVs to `build/fickle_wav/`:
  - `wet_<mode>.wav` (10 s): notes, a clock sweep 24 -> 2 -> 32 kHz, then freeze over silence.
  - `loop_<mode>.wav` (11 s): notes while listening, then the loop on over silence (noise hits for the env modes), a
    clock sweep while it plays, a dub with new notes, then the loop off.
- `make -C tests/host/fickle sim` gives the cycles above in `bfin-elf-run`. `prof` gives the same per pass.
- `make -C tests/host/fickle screens` writes the screen PNGs (above).
- `python3 gen_fickle_tables.py fickle_tables.h` regenerates the tables. `python3 gen_fickle_svg.py fickle.svg`
  regenerates the SVG.

**op1emu gate (not run: gate runs go one at a time):**
1. fickle at each position, with a chord through it.
2. The latches: loop on and off, dub, freeze.
3. The fn layers.
4. The stats `cost` line (`fx1_plugin=`, `s2_instr=`, `s2_master=`) and `plugmem` (the buffers' growth).
5. The screen against the PNGs: stroke widths under the knob and face matrices, and the raster lines' placement.

## For the owner

- **The heap budget:** fickle's 171,936-byte position buffers would be the largest declaration and grow all five by
  185,760 bytes.
  - A smaller loop would bring it to tape_echo's 134,784 or under: 16,384 pairs is 0.68 s at 24 kHz and saves 32 KiB.
    One constant, `kLoopPairs`.
- **The 30 k line:** see Cost. It is crossed only with the loop playing and dubbing above ~26 kHz.
  - A lower top clock (for example 28 kHz) would bring every scene under it.
  - So would dropping the reverb to three diffusers.
- **Pausing:** when the effect is bypassed (T3), render isn't called. When it comes back, everything resumes where it
  was: the loop, the wet tail and the smoothing. Kennel's ports restart after a 5 ms gap; this one keeps the loop on
  purpose, like a pedal's trails.
- **The loop footswitch, LED and ring follow the loop state, not the latch.** They come up the moment the latch goes
  off (the 128-sample fade-out counts as not playing). For a few milliseconds after the latch goes on, before a capture
  (or longer right after a start, until 1,280 clock samples are recorded), the latch is on while the screen still shows
  listening.
