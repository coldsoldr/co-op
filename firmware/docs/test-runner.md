# The test runner (p5b)

One scenario, played with the same inputs on the device and in op1emu: key presses and encoder turns at block-exact
times, sections with their own cost lines, and a click at both ends to line a recording up. The owner flashes, plugs
in USB and the audio cable, picks a script and presses Run; the page records the OP-1 from the interface and fetches
the results. [C op1emu, I device until the p5b test]

## Pieces

| Part | File | What it does |
|---|---|---|
| Scenario | `tests/sim/run_*.txt` | An op1emu headless script (guest-timed `at`), plus `#@mark` / `#@tail` / `#@lead` / `#@name` directives in comments |
| Compiler | `tools/run_script.py`, and the same code in `tools/coop_sysex.html` | Script to a CRUN file; `tests/sim/run_script_check.mjs` proves the two agree byte for byte |
| Runner | `core/sdram/coop_runner.cc` | Loads and checks the CRUN file (main loop), plays it from the audio interrupt, closes sections, writes `/yaffs2/user/coop_run.txt` |
| SysEx | `core/sdram/coop_sysex.cc`, command `0x27 RUN` | op 0 status, op 1 start (path), op 2 abort |
| Page | `tools/coop_sysex.html`, "Run a test" | Upload, start, poll, record (Web Audio), fetch `coop_run.txt` + `coop_stats.txt`, three downloads |
| Emulator | `tests/sim/run_emu.sh` | The same scenario twice: op1emu's own input (A) and the runner from a NAND file (B) |
| Compare | `tools/run_compare.py` | Device recording vs the B dump: clicks, clock drift, per-section level and brightness |

## How the runner injects input [C op1emu]

The stock key and encoder producers run in the audio interrupt (te-op1-cfw `docs/screen-interface.md`, "Input"):
- a key is `KeyHandler::PushKey(code | pressed << 8)` (`0x019C2F24`) and the wake `0x019C2F04`;
- an encoder step is `KeyHandler::AddKnob(k, +-1)` (`0x019C3044`), one per quadrature transition, two per detent; the
  KeyHandler thread takes the even part (`<< 9`: one detent = 1024).

op1emu write traces of `s_key_queue` (`0x01DD5044`) and `s_knob_acc` (`0x01DD5250`, `tests/sim/probe_knobs.txt`)
gave the mapping: enc1..enc4 are knobs 0..3, cw is +1, every Gray step of op1emu's `turn` is one `AddKnob(+-1)` 20 ms
apart, and the encoder pushes are keys `0x3B..0x3E`. The key codes of the other names come from op1emu's `gui/ui.json`
(`(bank - 3) * 16 + bit`). The runner calls the same functions from the same interrupt (`coop_stereo_block_start`,
before the stock callback). It accepts only real key codes (`0x00..0x37` without `0x2C`, `0x3B..0x3E`) and releases
every key it holds at the end and on Stop.

Each section's figures come from the cost table, which the interrupt copies and zeroes at every mark (where the costs
are added, so nothing races). After a run, the stats file's `cost` and `engines` lines therefore cover the last section
only; the run file has them all.

## CRUN format (version 1, little-endian)

`"CRUN"`, u16 version, u16 events, u16 marks, u16 0, char name[32]; marks x char[16]; events x
`{ u32 block, u8 type, u8 a, i8 b, u8 0 }`. Types: 1 key (a code, b pressed), 2 knob (a 0..3, b -4..4), 3 click, 4 mark
(a its index), 5 end. Block 0 is the start click; the first event comes `#@lead` ms later (1000), the end click and END
`#@tail` ms after the last event (8000). At most 2048 events and 32 marks. A block is 64 samples at 44.1 kHz.

## The run file (`/yaffs2/user/coop_run.txt`)

```
coop_run 1 build p5b script p5a
run events 1566 blocks 129130 state 3 lost_sections 0
switches dsynth 1 stage 1 cwo 1 ...
section lead-in blocks 1723 cb_avg 45306 cb_max 46380
cost avg/max/n stage=6697/6726/1723 master=76/721/1723
(p6a, 2026-11-08: the header is now `cost avg/max/n/p0/p1 pf_mode M pf_live L` and each entry carries `/p0/p1`, the performance counters' averages per call, when they are non-zero; docs/performance.md "p6a")
engines avg/max/n
section voltage6 blocks ... cb_avg ... cb_max ...
cost avg/max/n ...
engines avg/max/n synth=.../.../... voltage=8391/8420/6525
...
```
`cb_avg` / `cb_max` are the whole audio callback per block over the section (cycles, 580,499 per block).
State 3 is done, 4 is stopped.

## op1emu check (2026-11-04) [C]

`tests/sim/run_emu.sh tests/sim/run_check.txt tests/sim/idle_60s.txt`: Voltage (s2) with a knob turn, then Phase
(s7). A uses op1emu's input, B the runner from `user/coop_autorun.crun` (`g_coop_run_autostart` set, the device's own
loader). Aligned on the first note, the per-second RMS of A and B agrees to 0.1 through both chords and the turn. The
first note comes 4003.5 ms after B's start click; the script has it at 4000 ms. B's run file showed the three sections
with `voltage=8391` and `phase=5834` cycles per render, the p5a figures. The samples differ in phase (the stock
engines' free-running modulation, docs/emulator.md), not in level. No faults.

The images must be built with `--tape-stereo`: the main-loop hook that runs the stats, SysEx and the runner
(`coop_tape_io_step`) is only wired then, as in every package.

## Writing a scenario

- Start from the state you need: `tap synth`, a sound slot (`tap s5`), the engine page (`tap t1`) before the FX page
  (`tap t3`), because a tap of T3 on the FX page turns the effect off.
- The stock FX list clamps at its top (`tests/sim/probe_fx_clamp.txt`): `turn enc1 ccw 56`, then 8 steps per row
  (cwo, delay, grid, nitro, phone, punch, spring), selects any stock effect whatever the sound had.
- Factory sound slots are an assumption; the `engines` line names what really played.
- `wait` lines are for op1emu (wall time); the runner skips them. Keep the scenario's last `wait` long enough for
  op1emu to reach the END.

## p5a as a runner scenario (2026-11-04) [C op1emu]

`tests/sim/run_p5a.txt` (written by `tests/sim/gen_run_p5a.py`: six-voice Voltage and Phase with a knob sweep, DSynth dry, DSynth
through punch / phone / nitro / grid / spring / delay with all four knobs turned, five more engines, eight drum hits;
3 min 7 s, 1,566 events, 15 sections). In op1emu (`run_emu.sh ... idle_200s.txt p5a`) every effect section ran its
effect (the section's fx1 counter is the large one), the engine costs equal p5a's, A and B have the same level
(RMS 453.5 vs 455.4), no fault. `tools/run_compare.py` checked on a simulated recording of B: clicks found, the
+100 ppm clock offset measured, 0.0 dB in every section.

## The page cannot see the audio interface

**List inputs** reports in the status line how many inputs the browser found and why, and prints every device the
browser reports in the box below; send that text if the interface is missing. The first entry, "the browser's
default input", always records from whatever Windows has set as the default recording device, so setting the
interface as the default in Windows (Sound settings, Input) is a workaround. If the browser gives a file:// page no
media devices at all, serve the page from localhost (a secure context) instead:

```
wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware/tools && python3 -m http.server 8765'
```
then open `http://localhost:8765/coop_sysex.html` in Chrome or Edge.

## m1 additions (2026-11-10)

- `#@pfmode N` (0..4, 255 off): event type 6, `a` = the mode; the runner reprograms the performance counters
  (`coop_pf_set_mode`) when it plays; put it before the `#@mark` of the section it is for. Both compilers know it
  (`run_script_check.mjs`: 5 scripts, 0 different).
- `tests/sim/run_emu.sh`: `PLUGINS="a.coop b.coop"` puts plugin files on the NAND (both runs), `ONLY_B=1` runs only the
  runner side; `tests/sim/idle_340s.txt` for scenarios up to ~5 min.
- The run file holds 64 KB of text (the `stages` line and the gaps' stall columns per section).
