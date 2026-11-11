# Request: the project tempo as a core export (from the tape_echo port, 2026-11-11)

**Status:** built, 2026-11-11 (branch tempo-export, commit bd49705; packaged with e10): the export
`uint32_t coop_tempo_bpm_q16(void)` (`sdk/coop_exports.cc` entry 15, `core/sdram/plugin_host.cc`,
`core/include/coop_tempo.h`; `docs/fx-abi.md` "Tempo") and tape_echo 0.3, which reads it once a block. Host tests
bit-identical with the kennel reference at 120, 90, 133.5, 174 BPM and a seven-tempo walk; op1emu gate and device test
in `docs/device-tests.md` ("tape_echo 0.3"). Device: pending.

What was built, against the request below:
- **The source is the stock's float BPM at `0x018C1878`, not the LFO increment at `0x01AADAD4`** [C bytes,
  `symbols/sdram.sym`]: `SetTempo(float)` `0x01A5E9A4` stores the float first and derives the increment from it
  (`(uint)(bpm x 1662149.125f)`, through a single-precision product); the tape and the sequencer read the float. It is
  exact, it holds 120.0f in the image (valid from boot; the increment word is .bss, 0 until the first SetTempo), and it
  is the word the tempo screen sets.
- Q16 by integer shifts of the single (no float op); 120 << 16 outside 20..400 BPM.
- tape_echo: the 96th note is floor(882000 x 2^16 / bpm_q16) (a 16-step 32-bit division, only when the tempo moved);
  at 120 BPM 7350, as before, so the 120 audio is unchanged. The screen's ticks follow (the render publishes the 96th
  every block).
- A tape_echo 0.3 file on a core before e10: refused at load, "an import is not exported by the core".
- Not done: the beat / bar phase (not needed by tape_echo).

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
