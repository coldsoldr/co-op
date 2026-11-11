# Request: the project tempo as a core export (from the tape_echo port, 2026-11-11)

**Status:** open; for the firmware work, not built. The port ships its sync control at 120 BPM without it
(`plugins/tape_echo/README.md`, fn + green).

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

The tape echo would read it once a block, in place of its `TempoIncrement()` (`plugins/tape_echo/tape_echo.cc`, now
a constant 120 BPM). Other ports that sync (any delay or LFO-style effect) would share it.

## The control (decided, the owner, 2026-11-11)

The tempo lock is a saved `steps` control, fn + green "sync" (word 6): `off`, then kennel's 11 divisions 1/16T .. 1/2.
When set, the time follows the division at the tempo; `off` gives the blue turn the time back. Built in tape_echo 0.1
at 120 BPM.
