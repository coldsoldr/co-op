# Request: the project tempo as a core export (from the tape_echo port, 2026-11-11)

**Status:** open; for the firmware work, not built. The port ships without it (`plugins/tape_echo/README.md`).

## What the plugin needs

Kennel's tape echo locked its time to the project tempo's divisions (1/16T .. 1/2). It read the tempo from the stock
LFO's tempo-synced phase increment at `0x01AADAD4` (bpm x 1662149.125; te-op1-cfw `ports/knl/common/knl_fx.h`
`TempoIncrement`, from te-op1-cfw `docs/lfo-interface.md` §4 and `docs/sequencer-interface.md` §4), taking anything
outside 20..400 BPM as 120. A coop plugin may not read firmware addresses (guidelines §2.1); guidelines §2.3 already
names the export as [proposed] ("an effect that syncs reads the core's beat clock ... and falls back to 120 BPM").

Asked for:
- an export in `sdk/coop_exports.cc` (append-only), e.g. `uint32_t coop_tempo_bpm_q16(void)`: the project tempo in
  BPM, Q16, safe to call from render (a word read, no lock), 120 << 16 when unknown;
- later, a beat phase for effects that sync to the bar (not needed by tape_echo).

The tape echo would read it once a block. Other ports that sync (any delay or LFO-style effect) would share it.

## Also open (a control decision, the owner's)

Where the tempo lock sits now that shift + turn is the core's fine step: see `plugins/tape_echo/README.md` "Open
question for the owner". The export is useful whatever is decided there; the lock can't be built without one or the
other being settled, and runs at 120 BPM until the export lands.
