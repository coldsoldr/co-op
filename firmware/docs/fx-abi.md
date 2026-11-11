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

1. **Present stereo in / stereo out** to the coop core. `render(slot, l, r, params, n)` (in place, ABI v1)
   processes a stereo block.
2. **A mono DSP core is wrapped dual-mono by the port:** two independent instances of the effect's
   state, one driven by L, one by R. The port MUST allocate **separate per-channel state** (delay
   lines, filter history, envelopes) — never share one state across both channels, or the channels
   bleed and the image smears.
3. **An effect that needs true stereo** (cross-feed, mid/side, stereo widen, ping-pong) MUST be ported
   as a genuine stereo effect, NOT dual-mono wrapped. (A `COOP_FX_STEREO` flag was planned here; as of p6k
   the ABI v1 host accepts `flags == 0` only, and a native-stereo `.coop` is simply one whose render reads
   both buffers: there is nothing to flag until the ABI v2 store exists.) Mark it in the manifest so the core does not
   duplicate it.
4. **No mono collapse:** a port MUST NOT sum L+R to mono internally and re-spread, as that destroys
   upstream per-voice placement.
5. Fixed-point only, no float, no libc, hot loop under the I-cache budget (docs/performance.md); state
   in the position buffer the core gives (e8, "Memory" below), not static arrays.

6. **Controls beyond the four turns** (e6, `docs/plugins.md` "Effect controls"): declare them in `plugin.json`
   "controls" (saved words 5..7, latches as bits of `params[8]`); read `params[5..8]` once per block, clamp them, and
   never keep a latch across `init`. One render serves both channels, so a dual-mono port applies the same control
   values to both instances in that block.

(These will be enforced/annotated per-plugin in the Phase 4 porting guide. The coop FX we ship now —
`core/include/coop_fx.h`'s stereo delay — already satisfies rules 1–5 as a stereo-native effect.)

## Related

Per-voice panning is not only synths: **each drum-kit voice is independently pannable too** (the drum
voices get their own per-voice pan + spread, same model as synth voices; `coop_voice.h` already sizes
`COOP_MAX_VOICES` for the 12 drum voices). See docs/stereo-bus.md.

## Memory (e8: the position buffers; e7: the budget)

**The scratch model (e8, the default for every effect port).** The core owns one working buffer per position
(0 synth, 1 drum, 2 master, 3 instrument FX 2, 4 master FX 2), kennel's model (`op1_fx_scratch`). The effect active at
a position works in that position's buffer; the effect file is relocated **once** and its `.bss` holds only five
pointers (or tiny per-slot values indexed by `slot`).
- Declare: plugin.json `"scratch": N`, the bytes of one position's state (1 .. `COOP_FX_SCRATCH_MAX` = 180,224, 176 KiB,
  which every kennel effect core fits). pack.py writes the file's scratch flag (`KN_FLAG_SCRATCH` 0x20) and the bytes
  (extension 7); a core before e8 refuses the file ("unknown plugin flags"); this core refuses a larger declaration
  ("effect buffer larger than this core gives") and a file that calls `coop_fx_scratch` without the flag.
- Get: `void *coop_fx_scratch(int slot, uint32_t *bytes)` (a core export, `sdk/coop_plugin.h`), from `init` or
  `render`: the slot's buffer, 32-byte aligned, `*bytes` its size (at least the declaration). Fixed for the session.
- Contract: the contents are not preserved across selections. `init(slot)`, called every time the effect is selected
  (or restarted) at the position, resets all of the state as if the buffer held garbage; it stays cheap (a large tank
  is cleared over the next renders, the dry signal passing). The same effect at two positions works in two buffers.
- Sizing: the five buffers are one heap-0 block taken at the end of the boot scan (before the stock tape's cache), only
  when a scratch file loaded, each as large as the largest declaration among the loaded scratch files (32-byte
  rounded). The nine ports: tape_echo's 134,784 bytes, so 5 x 134,784 = 673,920 bytes.

**The budget (e7).** 1,536 KiB of heap 0 for all plugin files together (`docs/plugins.md` "e7"). A scratch effect counts
its image once plus what it grows the five buffers by (the buffers are counted once in all); an old-model effect (no
`"scratch"`: state in `.bss`, five relocations, as before e8) counts five times its load size; an engine once plus
its per-voice state. A file that would take the total over the budget is refused at boot, named in the stats
(`plugfile`) and the inventory. The nine ports with the buffers: 758,248 bytes (2,014,450 x5 before e8; tape_echo alone
was 718,260).
