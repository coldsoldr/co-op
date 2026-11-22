# Per-tape channel strip (design, CS1 DSP reference: 2026-11-22)

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

## CS2: the Blackfin kernels (next)

Target: **everything on <= 12 k cycles a track** in the simulator (4 tracks <= 48 k, ~8 % of the block), the bypassed
paths 0. As the other DSP in this tree (docs/performance.md "Fractional-MAC rewrite", `modal_bfin.S`, `wrap_fast.S`):

- The strip's I/O and the section boundaries in **int16** (the tape lanes are int16); the EQ in 16-bit data with
  32-bit feedback coefficients and error feedback on the 40-bit accumulators (mixed-mode MACs), the feed-forward
  taps 16-bit; comp gain and level gains as 16 x 16 MACs; the sat table lerp in 16 bits.
- A C model of exactly that arithmetic (`coop_strip_bf.h`), bit-exact against the assembly in the simulator
  (`tests/sim`, as `run_wrap_check.sh`), and within the CS1 tolerances against the reference (strip_test runs both).
- The kernels in the L1 pool (`l1_pool.cc`) if they fit; asm_lint clean.

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
