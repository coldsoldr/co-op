# coop-fw FX ABI and stereo rules

This is the firmware-side contract for effects, and the rules a 3rd-party plugin port MUST adhere to.
It will become the public `sdk/coop_fx.h` + `guide/porting-guide.md` in Phase 4; recorded here now so
the stereo decisions are locked before the stereo instrument bus (milestone C) and the FX host land.

## Decision: the whole insert/master chain is stereo in / stereo out (2026-11-01)

Once voices are placed in stereo (per-voice pan, milestone C), any mono stage in the path collapses
that placement back to centre. Therefore **every FX slot and the master chain carry stereo and present
a stereo-in / stereo-out interface.** There is no mono point downstream of the per-voice pan.

- **Insert slot position:** insert FX sit **after** the per-voice pan, so they process the already-placed
  stereo image and per-voice spread survives the chain. (A pre-pan mono insert may be offered later as
  an explicit per-effect opt-in, for effects that genuinely want the pre-placement mono sum; not the
  default.)
- **Master FX + master suite:** stereo (the stock master FX is already stereo; the coop master suite is
  stereo).

## Decision: mono effects are hosted as DUAL-MONO (2026-11-01, confirmed by the owner)

A mono effect is made stereo by running it **twice, once per channel, with independent per-channel
state** ("dual-mono"). The left and right instances never interact.

- **coop-native FX** are written stereo and fill a slot directly.
- **A mono DSP core** (most stock OP-1 insert FX — synth FX position 0 and drum FX position 1 are mono;
  only the master FX position 2 is stereo — and many ported kennel/opus effects) is wrapped dual-mono by
  the core/adapter.
- **Cost:** dual-mono doubles that one effect's per-block cost. There is ample headroom (a stereo insert
  slot measured ~1.9% of the 580,499-cycle budget, docs/performance.md), so a few dual-mono effects stay
  well under the 0.75 ceiling.
- **Limitation:** dual-mono is two independent mono effects, NOT a true stereo effect. Cross-channel
  behaviour (a true stereo reverb, a ping-pong delay, stereo widening) only comes from an effect written
  stereo. Dual-mono a reverb and you get two uncorrelated mono reverbs, which is acceptable but not a
  stereo reverb.

## Rules a 3rd-party plugin port MUST adhere to

When porting a kennel (.knl) or opus plugin to a coop FX, the port MUST satisfy:

1. **Present stereo in / stereo out** to the coop core. `render(slot, inL, inR, outL, outR, params, n)`
   processes a stereo block.
2. **A mono DSP core is wrapped dual-mono by the port:** two independent instances of the effect's
   state, one driven by L, one by R. The port MUST allocate **separate per-channel state** (delay
   lines, filter history, envelopes) — never share one state across both channels, or the channels
   bleed and the image smears.
3. **An effect that needs true stereo** (cross-feed, mid/side, stereo widen, ping-pong) MUST be ported
   as a genuine stereo effect, NOT dual-mono wrapped. Flag it `COOP_FX_STEREO` so the core does not
   duplicate it.
4. **No mono collapse:** a port MUST NOT sum L+R to mono internally and re-spread, as that destroys
   upstream per-voice placement.
5. Fixed-point only, no float, no libc, hot loop under the I-cache budget (docs/performance.md); state
   and scratch from the declared buffers (region B), not static arrays.

(These will be enforced/annotated per-plugin in the Phase 4 porting guide. The coop FX we ship now —
`core/include/coop_fx.h`'s stereo delay — already satisfies rules 1–5 as a stereo-native effect.)

## Related

Per-voice panning is not only synths: **each drum-kit voice is independently pannable too** (the drum
voices get their own per-voice pan + spread, same model as synth voices; `coop_voice.h` already sizes
`COOP_MAX_VOICES` for the 12 drum voices). See docs/stereo-bus.md.
