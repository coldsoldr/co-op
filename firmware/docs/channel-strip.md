# Per-tape channel strip (design, CS1 DSP reference, CS2 Blackfin kernels: 2026-11-22)

A channel strip on each of the four tape tracks: 4-band parametric EQ -> compressor -> saturator, then the track's
signal three ways: **dry** to the master, a **send to master FX 1** and a **send to master FX 2**, each 0-100 % on
its own (dry only, a dry + FX blend, or FX only). The owner's decisions are in docs/ui-features.md ("Channel strip");
the mockups are on the design canvas https://claude.ai/artifact/VrUfgR6XW9YpL8oT4xn5NU (layout A).

## The behaviour (owner, 2026-11-22)

- **Settings › Voices: "channel strip" on/off.** Off = the mixer and master as today, nothing of this runs.
- **On:** the mixer key shows the existing mixer. The **master EQ and master compressor are bypassed** (their settings
  are kept for when the switch goes off). Clicking blue / green / white / orange opens the strip of tape 1 / 2 / 3 / 4.
- **The strip is four pages** (layout A): EQ, COMP, SAT, SEND, tabs in the encoder colours. Clicking an encoder opens
  its page; clicking the encoder of the page already shown bypasses that section; shift + click resets the value under
  that encoder. The mixer key goes back to the levels; T1-T4 switch to another tape's strip on the same page.

  | page | blue | green | white | orange |
  |---|---|---|---|---|
  | EQ | band 1-4 | frequency | gain | Q |
  | COMP | threshold | ratio | attack (shift: release) | makeup (shift: knee) |
  | SAT | drive | type (tape / tube / diode / fold) | tone | mix |
  | SEND | dry | FX 1 | FX 2 | tap pre / post |

  EQ bands: 1 low shelf (shift + blue: low cut), 2 and 3 peaks, 4 high shelf (shift + blue: high cut).
- **The sends feed the master FX 1 and FX 2** (positions 2 and 4). With the strip on they are **send effects**: their
  input is the sum of the tape strips' sends only, and their returns go to the master bus. The **live instrument goes
  dry to the master** (it keeps its own sound FX 1 / FX 2); what is recorded to tape is unchanged.
- **Tap:** PRE = the track as it comes off the tape, POST = after EQ, comp and sat. Sends and dry both follow the
  track's fader and pan (riding the fader fades the track's FX too).
- **Saved per tape project** (with the tape, not with a sound).
- **A bypassed section costs nothing** (it is not entered). The same holds for an EQ band at 0 dB, a send or the dry
  path at 0 %, and a track whose strip is neutral (every section off, dry 100 %, no sends): such a track stays in the
  stock summed tape mix and gets no per-track work at all.

## The signal path

```
tape lanes (stock cache, 8 lanes = 4 stereo tracks)
  -> per track, unity gain: [neutral? -> the stock summed mix as today]
  -> strip: PRE tap -> EQ -> COMP -> SAT -> POST tap
  -> dry  x level x fader/pan -> the master pair d34/db4
  -> send 1 x level x fader/pan -> send bus 1 -> master FX 1 (stock master effect) -> += d34/db4
  -> send 2 x level x fader/pan -> send bus 2 -> master FX 2 (coop slot 2)        -> += d34/db4
live instrument -> its FX 1 / FX 2 -> d34/db4 (dry: no master FX while the strip is on)
d34/db4 -> master level stage, the master EQ and comp bypassed -> output
```

Where the stock does what (docs/tape-stereo.md, docs/audio-graph.md):

- The four tracks are mixed **inside the L1 tape tick** (`0xffa067a8`): the lane loop `LSETUP(0xffa0714c, 0xffa0716c)`
  MACs every lane x its fader/pan gain into one L/R play-buffer pair (`0x01b5ef94` / `0x01b5f444`) at the tape rate,
  and `l1_tape_play_resample` (`0xffa06194`) resamples that pair to the 64 samples of d34/db4 at the tape speed [C].
  So **a track exists on its own only inside the lane loop, before the resampler**. Two ways to give the strip a
  track: (a) mix each non-neutral track into its own pair and resample each (a 16-tap polyphase pass per track, a
  fixed 64-sample block at 44.1 kHz, the strip independent of the tape speed), or (b) run the strip at the tape rate
  before the resampler (no extra resampling, but EQ frequencies and time constants then move with the tape speed and
  the block length varies, ~130 samples at speed 1). **Plan: (a)**; (b) stays the fallback if the extra resampling
  does not fit. The lane loop and the gain hook are already coop's (`coop_tape_gains_thunk`, `tape_stereo.cc`).
- **Master FX 1** is the stock master effect, rendered in L1 on d34/db4 in place (`0xffa017bc/c6`); **master FX 2**
  is `coop_slot2_render(1, d34, db4)` in `coop_master_apply` (`master_suite.cc`). With the strip on, FX 1's call
  site is pointed at send bus 1 (an L1 call-site patch, like the existing ones) and its output is added to d34/db4;
  FX 2 renders send bus 2 and adds it the same way. With no send above 0 % on any track neither effect runs.
- **Master EQ and comp** live in the stock level stage `0xffa04280` (biquad + comp/drive, state at `0xff801880`)
  [C for the stage, I for which words are the EQ and the comp]. To find: the parameter words the mixer's EQ / master
  pages write, so the strip mode can force them neutral while it is on and restore them after.
- **UI:** the mixer screen's encoder clicks, the Settings › Voices row (`coop_settings.cc`), the four strip pages
  drawn by coop (as `coop_fx2view.cc` / `engine_screen.cc` draw theirs), the save file beside the tape.

## CS1: the DSP reference (done, host-tested)

- `core/include/coop_strip.h`: the saved form `coop_strip_params` (40 bytes of UI steps), the state, and
  `coop_strip_process(strip, L, R, gL, gR, buses)` on a 64-sample stereo Q31 block. Tables from
  `tools/gen_strip_tables.py` -> `core/include/coop_strip_tables.h` (no libm on the device).
  - EQ: RBJ cookbook shelves / peaks / 2-pole cuts, 120 frequency steps (1/12 octave, 20 Hz - 19.9 kHz), +-18 dB in
    0.5 dB, 24 Q steps 0.3 - 10. Direct form I with first-order error feedback: plain DF2T with 32-bit states left a
    -66 dBFS residue that never decayed for poles near 20 Hz (found by the host test). Coefficients are computed only
    when a parameter changed (five int64 divides per band).
  - Comp: stereo-linked peak detector, gain computed every 16 samples in the log2 domain (table log2/exp2), soft
    knee 0-12 dB, ratio 1:1 - 20:1 and limit, attack 0.1 - 100 ms, release 10 ms - 2 s, makeup 0 - +24 dB, the gain
    ramped across each 16 samples.
  - Sat: drive 0 - +24 dB into a shaping table (tape = tanh, tube = biased tanh with a DC blocker, diode = harder knee)
    or a triangle fold, output trimmed by 1/sqrt(drive), a one-pole tone tilt (+-), dry/wet mix.
  - Levels: 0-100 % as a squared curve (100 % = unity, 50 % = -12 dB), ramped across the block when they change.
- `tests/host/strip_test.cc` (in `make -C tests/host`): neutral strip bit-exact, 0 dB bands skipped, peak / shelf / cut
  responses within 0.3-1 dB of the design, decay to silence from every extreme setting, comp static curve (-20 dB,
  4:1 -> a -8 dB sine at -17 dB), makeup, limit, knee, sat mix 0 bit-exact, tone, sends (no-dry case, pre vs post
  tap, fader on sends), level ramps, 300 random full-on settings bounded.

### Cost of the reference (bfin simulator, `tests/sim/run_strip_bench.sh`, cycles per track and block)

| configuration | cycles |
|---|---|
| neutral (dry only, would stay in the stock mix) | 16,311 |
| EQ, 1 band | 48,469 |
| EQ, 4 bands | 144,576 |
| comp | 29,720 |
| sat (tape) | 49,158 |
| sat + tone + mix | 65,929 |
| dry + 2 sends | 37,958 |
| everything on | 228,895 |

The block is 580,499 cycles. The reference is C over int64 products and is **far too slow for the device** (four
tracks with everything on would take 1.5 blocks before the device's cache factor). It is the golden model, not the
shipping code.

## CS2: the Blackfin kernels (done in the simulator, 2026-11-22)

Target was **everything on <= 12 k cycles a track** in the simulator (4 tracks <= 48 k, ~8 % of the block), the
bypassed paths free. Met: **11,714 a block on average, 11,958 in the worst block** (a fader move ramping all six
buses) [C, simulator]. The heaviest setting (four +18 dB bands, tube, tone, mix, three ramping buses) is 12,759 /
13,003; see "Where the cycles go".

- `core/include/coop_strip_bf.h`: the C model, the same saved form `coop_strip_params` and API shape
  (`coop_strip_bf_set` / `_update` / `_process(strip, L, R, gL, gR, buses)`), its own state. The kernels in it
  (`strip_bfc_*`) are exactly what `core/sdram/strip_fast.S` (`strip_bfa_*`) computes; the per-block driver is
  shared C. Host-compilable (`-DOP1_HOST`) and bfin-compilable; on the Blackfin it calls the assembly.
- `core/sdram/strip_fast.S`: 11 kernels, 2,036 bytes of code (fits the L1 pool or the I-cache next to an engine);
  truncating MAC modes only (no RND_MOD dependence); circular buffers so no look-ahead load leaves its array; no
  store outside the named arrays; asm_lint clean.
- `tests/sim/run_strip_check.sh` (`strip_check.cc`): every kernel against its C model over thousands of random and
  edge blocks, states and settings, guard words around every array, then the whole strip, assembly against C, over
  600 random settings x 24 blocks (parameter and fader changes mid-run), outputs, buses and the whole state
  compared [C, simulator]:

  ```
  strip_check pack           3000 cases: bad 0, guard hits 0
  strip_check unpack         3000 cases: bad 0, guard hits 0
  strip_check peak           3000 cases: bad 0, guard hits 0
  strip_check comp          12000 cases: bad 0, guard hits 0
  strip_check shape/fold     3000 cases: bad 0, guard hits 0
  strip_check tube/tone/mix   3000 cases: bad 0, guard hits 0
  strip_check bus_n          3000 cases: bad 0, guard hits 0
  strip_check eq_band        9000 cases: bad 0, guard hits 0
  strip_check process       14400 cases: bad 0, guard hits 0
  PASS
  ```
- `tests/host/strip_test.cc` runs every property test on the reference and on the model with the CS1 tolerances
  unchanged (the model gets each input's int16 part, its int16 buses go back as Q31; "bit-exact" for the model
  means exact in int16: a neutral strip and mix 0 give the int16 input back exactly). The decay test passes on the
  model too; beyond it, all 11,520 settings of every frequency step, every Q step, +-18 dB, shelf and cut on four
  bands decay to exact digital zero (slowest 4,336 blocks) [C, host].

### The design

- **I/O int16**: L / R int16 in (processed in place, as the reference), the buses int16 L / R arrays added into with
  saturation: the format of the master pair d34 / db4 and of what the master effects render, so CS3 needs no
  conversion pass. Inside, a block is 64 stereo frames packed one per word (L low, R high): one dual MAC or one
  vector op does both channels (pack / unpack: 3 cycles a frame each, with the peak meters).
- **EQ: direct form I with a 31-bit feedback state.** The suggested design (16-bit data, 32-bit feedback
  coefficients, first-order error feedback) was built and measured first: it limit-cycles. Any error the feedback
  state cannot represent is amplified at DC by 1 / (1 + a1 + a2), about 1.2e5 at 20 Hz; with a 16-bit y the
  residues were up to 146 LSB (first-order error feedback) or a DC offset of up to 127 LSB (second-order error
  feedback with the then 2^-11 internal precision), for 2/3 of the decay test's settings. What decays:
  - the state is y = Y + E / 2^15 (Y the int16 output, rounded; E the remainder, kept as 2E), so the feedback
    carries 31 bits and the recursion is the exact filter's;
  - Q29 coefficients split c = H 2^15 + L (H signed 16-bit, L 0 .. 32767); a y-product is H Y 2^30 + (H E + L Y)
    2^15 + L E, three scales: G0 (L 2E) >> 15, G15 (H 2E, L Y, the b's L x) >> 15, G30 (H Y, the b's H x), all
    16 x 16 dual MACs (L and R at once), every product exact;
  - the one truncation left (the 15 bits the G15 -> G30 shift drops) is fed back to the next sample (first-order
    error feedback), so no constant error can hold the filter off zero, and the resonance ripple of that
    feedback is at 2^-15 LSB x ~2,200 (Q 6.3 at 20 Hz), well under the rounding;
  - b's above 2 (boosts) are scaled by 2^-k (k <= 3) and the output is round(y 2^k), a 5-cycle pass after the band;
  - 29 cycles a sample for both channels, ~110 a band of setup and state; a band at 0 dB is not in the job list.
- **Comp**: the reference's detector and gain computer on the int16 peak, in 32 bits (`coop_mul_fr` = the Blackfin
  3-MAC 32 x 32 sequence, exact); the gain per 16-sample sub-block as a Q15 mantissa and a left shift 0 .. 5,
  ramped and ending on the target. One kernel for the block, branch-light (a taken branch costs 9 cycles in the
  simulator, more than the arithmetic it skips).
- **Sat**: drive Q11, the +-4 clamp as a saturating shift (Q29), a 512-step table lerp with a 15-bit fraction (two
  table reads per channel), trim Q15; fold as a wrapped triangle (`(p << 4) + 2^30`, ABS); tube's DC blocker in 32
  bits; tone and mix on packed pairs in 16 bits.
- **Buses**: one job list for the dry path and both sends (L, R): nothing at 0 %, a plain add at exact unity (so a
  neutral strip is bit-exact), 2 cycles a sample pair at a constant gain, 3 while the gain ramps (per sample, the
  last sample on the new gain, as the reference).
- **Bypass**: a bypassed section, a 0 dB band, a 0 % bus are not entered; with no section on the strip only
  measures the peak and adds the buses straight from L / R (440 cycles with dry 0, 617 with dry 100 %).

### Cost (bfin simulator, `tests/sim/run_strip_bench.sh`, cycles per track and block) [C]

| configuration | reference | CS2 avg | CS2 worst |
|---|---|---|---|
| neutral (dry only) | 16,448 | 617 | 700 |
| all off, dry 0 | 5,321 | 440 | 442 |
| EQ, 1 band | 50,205 | 2,965 | 3,048 |
| EQ, 4 bands | 151,087 | 8,713 | 8,796 |
| comp | 29,323 | 1,795 | 1,878 |
| sat (tape) | 49,170 | 2,247 | 2,330 |
| sat + tone + mix | 65,813 | 2,851 | 2,934 |
| dry + 2 sends | 38,227 | 972 | 1,216 |
| **everything on** | 234,721 | **11,714** | **11,958** |
| everything on, 4 x +18 dB bands, tube | 237,814 | 12,759 | 13,003 |

The CS2 worst is the block where the fader moves (all six buses ramping); the bench moves it every 8th block. The
reference numbers are this build's (both implementations in one binary: ~2 % above the CS1 table's, code layout).
The neutral row would not run on the device (such a track stays in the stock mix).

Kernels alone: EQ band 1,975 (k = 0) / 2,330 (k > 0); comp 818; shape 1,210; fold 890; tube 632; tone 440; mix 161;
pack 252; unpack 255; peak 162; six bus jobs (mixed) 716.

### Where the cycles go, and what is open

- Everything on (4 bands at +6 dB, one of them k = 1): EQ ~8.3 k, sat + tone + mix 1.8 k, comp 0.8 k, buses 0.7 k,
  pack / unpack 0.5 k, the C driver ~0.2 k.
- The four +18 dB bands case is 1 k over the target: each boosted band needs the k > 0 output pass (+5 cycles a
  sample, ~355 a band) and tube adds 632. Folding the pass into the band's loop costs the same 5 operations a sample;
  a cheaper form needs a different coefficient scaling (open).
- **The update block**: a parameter change costs ~25 k cycles in the block it lands in (`coop_strip_bf_update`:
  the reference's int64 coefficient divides, five per band). CS3 should spread it (one band a block) or move it out
  of the audio callback.
- The state points into itself (the EQ jobs, the work blocks): a copied `coop_strip_bf` needs `coop_strip_bf_set`.
- In the reference, `coop_strip_update` tracks `eq_was` but never uses it: the EQ switched back on resumes its bands'
  old filter state (a band is reset only when the band itself turns on). The model resets every band when the EQ
  section switches on, as the doc says a section should.
- Device: nothing here has run on the OP-1. The simulator counts no cache or SDRAM stalls (docs/performance.md); the
  kernels' working set is the strip state (1,512 bytes a track, its two work blocks included) and the tables (log2 / exp2 2 KB, one sat curve 1 KB).

## CS3 onwards (plan)

1. **CS3, the audio path (op1emu):** a coop lane loop that leaves neutral tracks in the stock sum and mixes each
   other track into its own pair; one resampler pass per such track; the strips; the dry sum into d34/db4; send
   buses 1 / 2; master FX 1's call site moved to send bus 1 and added back; master FX 2 on send bus 2. Gate: strip
   mode on with four neutral strips is sample-identical to strip mode off; a chord recorded on track 1 with only
   FX 1 at 100 % and dry 0 comes out of FX 1 only.
2. **CS4, master EQ / comp bypass:** find their parameter words, force neutral while the strip is on, restore after.
3. **CS5, the UI:** Settings › Voices row; the mixer's encoder clicks; the four pages and their tabs, meters (in/out,
   gain reduction), the encoder-colour value popup; T1-T4 and the mixer key; shift + click reset.
4. **CS6, saving:** `/yaffs2/user/coop_strip.bin` beside the tape (4 x 40 bytes + version + the switch), written
   when a value settles (as kennel's held saves), cleared when the tape image is rebuilt.
5. **CS7, the device:** package, the owner's test, `docs/device-tests.md`. Core tag **e17** (e16 is the engine work
   running in parallel, 2026-11-22).

## Open points

- Whether four resampler passes fit next to a heavy engine (measure in CS3; fallback (b) above).
- The meters the stock lane loop writes (`[SP+0x60]`) must keep coming for the tape screen when tracks leave the sum.
- How the stock master effects behave on a send bus: their wet/dry is their own (an effect that leaves its dry in
  adds that dry again). Possibly a "100 % wet" override per type in strip mode; ask the owner after hearing it.

## CS3 / CS4 hook map (research, 2026-11-22; stock L1 addresses, unpatched by coop unless noted)

Prior art: kennel 3.3.1 (GPL) ships a tape mix takeover with per-track outs and a tape strip insert
(`synth_sdk/hook.S`, `screens/usbtrack.c`, `fxroute.c`), proven on devices. Two op1emu write traces on the stock image.

**Per-track split (tape tick `0xffa067a8`).** Play pair `[FP-0x64]` = `0x01b5ef94` (L), `[FP-0x60]` = `0x01b5f444`
(R), 600 samples each, resampler bases 8 lower (`0x01b5ef84` / `0x01b5f434`) [C]; the tick re-fetches ~9 samples
before the position each block, so no history to keep [I]. Lane gains: the pan law `0xffa06d3e..dc2` (level
`0xff801928` x pan `0xff801930` x mute fade `0xff8018b4`), pointer `[SP+0x5c]`, swapped by coop for `g_lane_gains`;
meters `[SP+0x60]` = `0xff802990`, swapped for `g_lane_meters` [C]. Main lane loop `LSETUP(0xffa0711a, 0xffa07176)`
(P1+2 source, M0 / I1 L / R destinations, P0 gains, P4 meters, R3 lane, R6 count, R7.H destination offset); its meter
MAX `0xffa0712c` (`06c4 0100`) then `IF CC JUMP 0xffa0716e` (CC = count <= 0) [C]. Seam loop
`LSETUP(0xffa0707a, 0xffa070e0)`, meter MAX `0xffa0709c` (`06c4 1000`), `IF CC JUMP 0xffa070da` [C].
**Hook:** each meter MAX becomes `CALL tramp`; the thunk does the MAX, copies a split lane's R6 samples at unity into
its track pair at offset R7.H (lane t -> L[t], 4+t -> R[t]) and returns CC = 1 so the stock MAC skips that lane
(neutral tracks stay bit-exact in the stock sum); every other register and CC preserved. Main site: P2 free
(12-byte `P2 = thunk; JUMP (P2)`); seam site: all P live (`[--SP] = P2; ...`, the thunk pops). `coop_tape_gains`
zeroes the split pairs each block and hands the split mask and per-track gL / gR to the strips. L1 room: the tape
build's free range `0xffa0bfb4..c000` is full; loop B's dead body `0xffa00fb4..0xffa00fdb` (40 B, unreached [C])
holds three 12-byte trampolines, the voice-sum skip leaves `0xffa00d2e..0xffa00d71` (68 B) in reserve.

**Resampler `0xffa06194`** (`CALL` at `0xffa07250`): R0 = position (1/64 sample), R1 = increment (~130 at speed 1),
R2 = gain-ramp step pointer; gain from `W[0xff8029dc]`; 16 taps x 64 phases at `0xff802154`; writes all 64 of
d34 / db4; returns pos + 64 x inc [C]. Sources and outputs are immediates (`0xffa061b4..d0`), 0xb4 bytes, otherwise
position independent: a coop copy with pointers passed in (as kennel's `tr_rs_pair`). **3,583 sim cycles a stereo
pass** [C bfin-sim]: four split tracks +14.3 k (2.5 %); kennel's packed kernel ~1.8 k a pass [I]. **Hook:** retarget
the CALL at `0xffa07250` (P2 free) to a thunk: the stock pass, then each split pair with the same pos / inc / ramp,
the strips, dry into d34 / db4, sends into buses 1 / 2; the stock tape level stage `0xffa072c4..314` then runs on the
sum as before.

**Master FX 1** (gate `0xffa01202..121e`: `[0x01aaad68]` instance, `B[0x01aaac40]` on -> `JUMP.S 0xffa017b8`):
`0xffa017b8..e6` calls render(this + adjust, d34, db4, params `0xff8016c4`), then `JUMP.S 0xffa01220` [C]. Order in
the master stage: master EQ `CALL 0xffa030f0` at `0xffa01118` -> FX 1 -> getter site `0xffa017a4`
(`coop_master_apply`: FX 2) -> `0xffa04280`. **Hook:** overwrite `0xffa017b8..` with `P2 = coop_master_fx1;
CALL (P2); JUMP.S 0xffa01220` (12 B): strip off = the stock call, strip on = render send bus 1 and add the return.
**FX 2:** `coop_slot2_render(1, l, r)` renders any 64-sample pair in place (stock types and plugins) [C]: pass send
bus 2 and add it back (mind the `g_coop_s2_live[1]` early-out).

**Master EQ (CS4)** [C trace]: three words `0xff8017cc / ce / d0` (low / mid / high; orange moves all three), default
0x3fff, 0x4000 exactly flat [I strong]; DSP `0xffa030f0`, called unconditionally from `0xffa01118`. **Bypass:** rewrite
`0xffa0110e..0xffa0111b` (`P1 = 0x010004cc; CALL (P1); CALL 0xffa030f0`, 14 B) as `P1 = coop_master_eq; CALL (P1)`
+ NOPs, skipping the stock EQ while the strip is on (words, UI and save untouched). **Comp:** four words
`0xff80189c / 9e / a0 / a2` read by `0xffa04280` (defaults 0x7fff, 0x7fff, 0x2000, 0x2800; word 3 >>> 10 indexes
`0x0106a8f4`) [C]; the stage cannot be skipped (it writes e34 / f34), so the bypass is by values: **neutral values
not settled** (an op1emu sine sweep per word). Mixer subscreen word `[0x01aad5f0]` (T1 0, T2 1 EQ, T4 3 comp), knob
handler `0x19da2bc`, key-down `0x19db7c4` [C].

**Mixer UI:** coop already owns keys (`coop_ui_key`), turns (`coop_ui_knob`) and drawing (`coop_ui_frame`). Mixer =
`CUR_MODE 0xFF907D58 == 3`, screen 0x1F; encoder pushes are key codes 0x3b..0x3e, which the stock MasterController
ignores [I strong]. Hook: in strip mode with the mixer's T1 page up, a push opens strip `code - 0x3b` and is swallowed;
while open, T1-T4 (0x06..0x09), pushes and turns are coop's, drawn over 0x1F; the mixer key (0x05) closes and passes.

**Risks:** thunks inside the tick's hardware loops (LC1 live: asm, or C with LC0/LC1/LT/LB and ASTAT saved); CC
untouched for non-split lanes; L0-L3 = 0 in the master-stage thunks; split pairs zeroed every block; device cost of
SDRAM resampler passes unknown (sim floor 14.3 k for four).

**Send returns (owner, 2026-11-22: wet only):** the return is FX(send) - send, as kennel's aux returns. The stock effects carry their own dry; kennel's aux returns add FX(send) - send
(a pure wet return for delay / reverb; filter / punch / phone types return only "the change"); adding FX(send) instead
doubles the dry when the track also has dry > 0.
