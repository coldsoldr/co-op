# Father redesign: plan and candidates (2026-10-29)

Status: **candidate A picked by the owner (2026-10-29) and built** on this branch (`plugins/father/README.md` "Redesign",
ARCHITECTURE.md "Father (redesign 2026-10-29)"). Native A/B and unit readings: see the end of this file. Evidence, prototypes and the
listening page: root `run/father-redesign-2026-10-29/` (`index.html`, `meas/cost.json`, `meas/differences.json`,
`proto/f2.c`).

## Why

Father costs about 19 % of a unit audio block and crackles with heavy synths. bcore counts 13.2k packets per mono call,
but the call's data traffic dominates on the unit: per cold call 205 line fills and 1,238 single SDRAM writes (root
`run/father-toggle-hang-2026-10-28/NOTES.md`). The data cache does not allocate on a write miss, and Father writes eight
8 KB tank lines, two 8 KB predelay rings, four allpass rings and a 3 KB per-call workspace mostly before reading them.
Trimming that design saves 3-4 % (branch `father-lite`). Target: at most about 8 % of a unit block in the bcore + cache
model, with no single-block spikes.

## Design (shared by every candidate)

- The same network: eight unequal lines, two interleaved four-way Hadamard groups (each line feeds four, all eight after
  two passes), per-line slow triangle modulation, decay gains from Father's RT60 curve, two input allpasses per channel,
  the unequal left/right early-reflection cluster with its colour filter, Father's gate and mix code unchanged.
- **Lower internal rate.** The tank runs at 11.025 kHz (2:1 inside the effect): a half-band decimator (-1 0 9 16 9 0 -1)/32
  on the predelayed input, a 4-tap (-1 9 9 -1)/16 interpolator on the wet output. Same delay times in seconds, same decay
  per second; the damping pole is squared so Colour keeps its cutoff.
- **Exact-length rings, read before written.** Every line and allpass is a ring of exactly its maximum delay. The block's
  reads cover the lines its writes then land in, so the stores hit the cache and leave as whole-line write-backs. Writes
  without a preceding read (predelay ring, per-call rows) touch one word per 32-byte line first.
- **Small and contiguous state.** About 20 KB per instance (A) instead of 93 KB; per call it touches about 4 KB.
- Integers only, no division, no libc. Knob meanings, the FX name, plugin.json defaults and preset markers do not change.

## Candidates (mono call, cold cache, no write-allocate)

| | What changes | Data per call (fills / write misses / write-backs) | Unit estimate, native | What you lose |
|---|---|---|---|---|
| current | - | 205 / 1,238 / 32 | 14.8 % in the model (unit ~19 %) | - |
| **A** | tank at 11 kHz; predelay and early taps stay at 22 kHz | 123 / 58 / 81 | **~7.2 %** | nothing above 5.5 kHz in the late tail |
| A0 | A, with predelay and early taps at 11 kHz too | 106 / 80 / 76 | ~6.6 % | also a duller reverb attack (-7 dB at 5 kHz, -18 dB at 8 kHz wet drums) |
| B | four lines at 11 kHz | 96 / 50 / 60 | ~5.5 % | slower build-up (dense at 300-400 ms instead of 220-260 ms), plucks ring +3.5 dB |
| C | eight lines at 5.5 kHz | 102 / 86 / 61 | ~6.0 % | nothing above 2.75 kHz in the late tail |

Write misses in the prototypes are mostly their own stack spills and temporaries (40-71 per call; the native estimate leaves them out). Source-only sound
measures against current Father (wet, worst material): A keeps RT60 within 0.1 s (1.49 / 3.76 s vs 1.45 / 3.88 s), the
same echo-density build-up (dense at 229 ms vs 220-259 ms) and the 1/3-octave spectrum within 1.2-3.4 dB RMS.

Cost model: packets + fills x 115 + write misses x 25 + dirty write-backs x 100 + hot code lines x 115 cycles, against the
580,499-cycle unit block. Native packets are the plain-C prototype's divided by Father's own
plain-C / asm ratio (father_dsp.c without father_kern.S on the same guest: 41.5k vs 13.2k packets, 3.15x), so A is about
9.2k per call; native hot code is assumed at 2.4-3 KB. The margin under 8 % for A is about 0.8 % of a block. The stereo master call adds a second predelay ring and the right channel (not
measured; roughly +1.5-2 %).

Recommendation: **A**. It is the closest to Father by measurement and the extra cost over A0/B/C is 1-1.5 % of a block.

## Native build plan (after the pick)

1. `father_dsp.{c,h}` and `gen_tables.py` for the chosen candidate; `father.c`, `father_screen.c`, plugin.json name and
   defaults unchanged (the `cost` field updated from measurement). Prototype bug not to inherit: with full-rate
   predelay (A, B, C) the gate hold adds `pre_samples * F2_DIV`, but `pre_samples` is already in full-rate samples
   there (up to 30 ms too long a hold at maximum distance; the renders had the gate off).
2. Fuse the per-call rows where possible (input rows into the write-back, mono right channel skipped) and bound the ring
   wrap paths to two contiguous pieces, so a wrap call costs about the same as any other.
3. Hand-scheduled Blackfin kernels for the tank read, write-back and early taps (`father_kern.S`, checked bit-exact
   against the C on bcore as today), hot code at most ~3 KB.
4. Init: the state is cleared in slices across the first callbacks (dry meanwhile, `fx_spread.h`), so selecting Father
   is no longer a single heavy block (today's init clears 93 KB inside the audio callback).
5. Review: `preset_audio.py` native dry/full takes from separate silent forks, bcore cost tools and
   `tools/firmware/perf_mem.py` warm/cold (op1emu's memory model on a built plugin, so it only applies to the native
   build; its numbers replace this plan's private model, cachesim2.py with 115 / 25 / 100-cycle costs, in the
   before/after table), `make -C synth_sdk/harness check`; a before/after table and A/B pages.
6. On the unit: page 5/7 readings with JUNI or MBIRA + Father, six fast notes, against the same scene with today's Father.

## Result (2026-10-29)

Built as planned with two changes found on the way: each line's modulated delay is held for a callback (it moves less
than 1/100 sample per callback; the read becomes one fixed-fraction loop), and the tank read and late sum stay partly in
C. Cost per call (root `run/father-redesign-2026-10-29/meas/final.md`):

| call | packets | cold: fills / write misses / write-backs | model, cold | model, warm |
|---|---|---|---|---|
| current, mono | 13,204 | 205 / 1,238 / 32 | 14.9 % | 7.2 % |
| redesign, mono | 10,729 | 122 / 67 / 82 | 8.4 % | 2.3 % |
| current, stereo | 13,204 | 213 / 1,241 / 37 | 15.2 % | 7.3 % |
| redesign, stereo | 13,174 | 159 / 61 / 95 | 9.8 % | 3.0 % |

The mono call lands at 8.4 % in the cold model, against the ~7.2 % plan estimate. The cold model counts the kernels'
register saves on a cold stack (62 write misses); the ISR's stack is probably warm on the unit. Left for later, about
0.3-0.6 % of a block: the eight tank reads as one kernel (two lines per dual MAC, the per-line setup out of C) and a
smaller `mw_process`. The select block clears in 6 callbacks of 1,024 words instead of 93 KB at once.

Native (op1emu, rc-3.1j core and fixture, `preset_audio.py` full and dry takes from separate silent forks; root
`run/father-redesign-2026-10-29/native/`): 103/103 checks for both the current `father.knl` and the redesign's (pushed
over USB), loaded code exact. Levels match at the defaults, bright and gated settings. On a held note at the long
setting the two differ by up to ~10 dB in sustained wet level depending on the note (a sine sweep 120-500 Hz: the
current Father louder at 120, 131, 135, 262 and 392 Hz by 1-11 dB, the redesign louder at 140 and 500 Hz): the delay
set changed (primes near the halves), so the resonances sit at other pitches. Decay time is the same once the note
stops (T60 7-8 s in the bass, ~3 s at 1.5 kHz). The owner keeps this delay set (2026-10-29): exact-half
delays, which would restore the old resonances, were offered and declined. The built C is 53-59 dB SNR from the
prototype the owner approved. Listening: `run/father-redesign-2026-10-29/native/index.html`.
