# coop plugin guidelines

The rules every coop plugin follows, whether it is written for coop or ported from hundekraft (kennel 3.1) or Opus.
They make every plugin behave the same way under the hands, sound right in the stereo chain, and stay inside the
block budget. They draw on these sources:

- the owner's screen manifesto, "The stock look: art direction for OP-1 screens" (claude.ai artifact
  `WF7BsFuruMS2TXX5Ff1s5w`), the source for sections 4 and 5;
- the hundekraft 3.1 manual and source (`incoming/op1-workshop/manual.html`, `incoming/hk/hundekraft-3.1.4-source/ext/kennel`);
- the Opus 2 manual and plugin kit (https://opus-op1.pages.dev/, `incoming/opus/plugin-kit/docs/api.md`, `rules.md`);
- coop's own records: `docs/plugins.md`, `docs/fx-abi.md`, `docs/plugin-performance.md`, `docs/screens.md`.

What the words mean:
- **MUST**: a plugin that breaks this rule does not ship.
- **SHOULD**: a plugin may break this rule only with a reason written in its contract (section 3).
- **[now]**: ABI v1 already enforces this rule, or a plugin can already follow it.
- **[proposed]**: the rule needs core work that isn't built yet (the parameter store, the six-action dispatch, ABI v2).
  It is written down now so ports are designed for it from the start.
- **[C]** and **[I]**: confirmed and inferred, as in the rest of `docs/`.

---

## 1. What a plugin is

| Kind | Status | Interface | Where it appears |
|---|---|---|---|
| Effect | [now] ABI v1 | `coop_fx_t`, `sdk/coop_plugin.h` | The FX list (shift + T3): blue column FX 1, green column FX 2 (`docs/ui-features.md` §7) |
| Engine | [now] ABI v1 (E1, `docs/engines.md`) | `coop_engine_t`, `sdk/coop_plugin.h` | Stock engine list (shift + T1) |
| LFO, sequencer, tool | [proposed] later | Not yet designed | Their stock lists |

- A plugin is one `.coop` file in `/yaffs2/plugins`. It is packed from `plugins/<id>/plugin.json` by `plugins/Makefile`,
  installed over SysEx, and loaded at the next power cycle. [now]
- **The id** is 1-15 characters from `[a-z0-9_]`, unique, and never changed after release. Saved sounds and the
  `coopplugins` number map name it. The list shows the id with `_` as a space. [now]
- **A port keeps the upstream name** unless it clashes with a stock type or another plugin. Its `description` says
  where it came from: firmware, version and author. The licence MUST allow the port (GPL-3.0-or-later with the OP-1
  firmware permission for coop's own plugins).

## 2. The audio contract

### 2.1 Numbers and code (MUST, [now])
- Use fixed point only: no `float` or `double` anywhere in the file, not even outside the audio path.
  - Kennel's float paths are compiled out of its shipped builds (clouds).
  - Opus allows soft-float. coop doesn't, because a float that sneaks into the audio path costs thousands of cycles.
- No libc and no static constructors.
  - libgcc helpers such as division and 64-bit shifts are allowed outside the per-sample loop.
  - A per-sample divide is a bug.
- Keep the hot loop small and contiguous (the I-cache is 16 KB, 4 ways, and coop keeps its own hot path near 12 KB).
- Follow every per-sample and structure rule in `docs/plugin-performance.md`.
- Hand-written assembly goes through `tools/asm_lint.py`. Copy only packet combinations the stock code uses: the
  device refuses some packets that op1emu accepts (SEQSTAT 0x22).
- **No firmware addresses in a plugin.** A plugin reaches the core only through its callbacks and the append-only
  `sdk/coop_exports.cc` table. [now]
  - Kennel plugins read the shift byte, the voice velocity, page globals and screen nodes at hard-coded R246
    addresses. A port MUST replace every such read with the coop equivalent: a callback argument, a declared
    parameter or an export.

### 2.2 Stereo (MUST, [now]; `docs/fx-abi.md`)
- An effect is stereo in, stereo out: `render(slot, l, r, params, n)` works in place on two int32 planar buffers
  (Q31, the int16 bus widened by 16 bits), 64 samples a block.
  - This replaces the separate in/out signature drafted in `docs/fx-abi.md`.
- It runs at every position (synth 0, drum 1, master 2, instrument slot 2 is 3, master slot 2 is 4) with the same
  call. There is no mono position in coop.
- Kennel and Opus effects with only a mono render are hosted **dual-mono**: two independent states, one per channel.
  - A port MUST key its state by channel as well as by position. Kennel keys state by `pos` only.
  - Each of coop's five relocations has its own `.bss`, so per-position state comes free. Per-channel state does not.
- An effect with cross-channel behaviour (stereo reverb, ping-pong, widening, mid/side) MUST be written as true
  stereo. The kennel effects flagged stereo are crush, father, juni_chorus, plate, sister, steve and warble.
  - On kennel these ran stereo only at the master. On coop they run their stereo render at every position, so the
    instrument positions now hear them in stereo.
- Never sum L and R to mono and spread the result again. That destroys the per-voice placement upstream.
- An engine renders one voice: 128 samples at 88.2 kHz, the stock voice rate. [proposed]
  - The core places each voice in stereo (voice spread, `docs/stereo-bus.md`).
  - A stereo voice is opt-in, behind a flag.

### 2.3 Threads, memory, time (MUST)
- `init(slot)` runs from the audio thread before a position's first render. It only resets state and binds buffers.
  [now]
  - Work that costs more than a few thousand cycles (clearing a large tank, building tables) goes in a main-thread
    hook. ABI v2 will provide one ([proposed]). Until then, a plugin with a slow init doesn't ship.
- Nothing in render allocates memory, touches a file, waits or calls a long helper. [now]
- Memory sources:
  - Long lines and tanks go in the plugin's own `.bss`. The loader zeroes it, five times over.
  - Data the per-sample loop touches every block goes in L1 (a line the audio interrupt touches in SDRAM costs a bus
    wait when it isn't cached, ~107 cycles measured by Opus; L1 never does). [now, e4, engines; `sdk/coop_plugin.h`, `docs/engines.md`]
    - `coop_l1_lease(bytes)` from the engine's `init` only: a zeroed, 32-byte aligned block of L1 data bank B kept
      for the session, up to 4 KB for all engines together; 0 when refused, so keep an SDRAM fallback. Say in the
      contract how much the plugin takes.
    - `coop_l1_scratch(&bytes)`: 1312 bytes of the L1 scratchpad, per call, inside the engine's own `note_on` and
      `render` only (kennel rings' window). Nothing survives between calls.
    - Effects: [proposed] (their init runs in the audio interrupt at the stock positions).
  - The host's own effects follow `docs/fx-data-placement-plan.md`; a port does the same: prefetch its delay-line
    streams a few lines ahead (`PREFETCH [P]`), process a delay line in block passes rather than per sample across
    several lines, keep per-sample state in registers, keep the render inside the I-cache's reach. [proposed]
  - Each relocation costs the file's load size, so a 129 KiB plugin takes 645 KiB of heap 0. Keep `.bss` to what
    the sound needs. A plugin that doesn't fit fails to load with "plugin memory full".
- Tempo: an effect that syncs reads the core's beat clock (an export, [proposed]: tempo, MIDI clock or tape speed),
  and falls back to 120 BPM as kennel does.

### 2.4 Cost (MUST)
- An effect slot SHOULD stay under 30 k cycles a block. Over 60 k is a bug to fix before release (the budget is
  580,499 a block).
- An engine's voice is measured against the stock engines it sits beside, using the stats file's `engines` line.
- The contract gives the device-measured average and peak from the stats file's `cost` line (`fx1_plugin=`,
  `s2_instr=`, `s2_master=`). Emulator figures don't count, because the device is 10-35 % higher.
- A bypassed or unselected plugin costs nothing beyond a check.
- **An engine: keep the code one render runs under ~4-7 KB.** [C device, `docs/m1-results.md` "m1b"] A synth
  engine renders six times a block, and coop's per-voice code and the stock's run between the renders from the same
  4-way, 16 KB instruction cache. Against `probe_alu` (the same render cost, almost no code) the whole callback cost
  8 k a block more with 4 KB of engine code, 93 k with 8 KB and 121 k with 12 KB (m1b, before the v2 cut of coop's
  own path). Count the code a render actually runs (the control code included),
  not the file size. Measure it before porting: `tests/plugins/probe_code` (4 / 8 / 12 KB, `fn` + blue) at your
  render's cost (`fn` + green) gives the collateral for each size on the device; compare your engine's
  `collateral` in `tools/m1_table.py` with the probe's.
- Over-budget switch-off ([proposed], after Opus): the core switches off a plugin that runs more than 25 % over its
  declared peak in 8 of the last 64 blocks, and the stats file names it.

## 3. The contract: what every plugin declares

Every plugin ships a **contract**:
- a parameter table in `plugin.json` ([proposed] schema; the core reads it at load);
- a 4 x 6 action table in `plugins/<id>/README.md`, in encoder order.

A picture of an action doesn't implement it. An action that isn't built yet is marked *proposed*.

### 3.1 The parameter table [proposed, ABI v2]

The core owns every parameter's value (the core-owned store, P2 onward). The plugin declares its parameters and
reads values. It never edits the store or sees raw encoder deltas.

| Field | Meaning |
|---|---|
| `id` | Short stable key, `[a-z0-9_]`. Saved sounds use it. Never renamed |
| `name` | Screen name: at most 8 characters, from the stock glyph set (A-Z, 0-9, `! " # % * + , - . / : ; < = > ? _`; written lowercase, drawn as capitals) |
| `action` | Where it sits: `turn`, `click`, `shift_turn`, `shift_click`, `deep_turn` (shift + fn + turn) or `deep_click`, plus the encoder (`blue`, `green`, `white`, `orange`) |
| `kind` | `continuous`, `steps` (n positions, e.g. a filter type), `switch` (on/off), `latch` (momentary state, not saved) or `command` (one-shot, e.g. init) |
| `min`, `max`, `default` | In the parameter's own integer units. The plugin receives Q15 (0..32767) of the range, or the step index for `steps`, `switch` and `latch` |
| `steps` | For `steps`: the count and each step's name (at most 8 characters each) |
| `format` | How the value is shown (section 3.3) |
| `lfo` | Whether the stock LFO may move it. Default: yes for `continuous`, no for everything else |
| `saved` | Default: yes for everything except `latch` and `command` |
| `smooth` | Whether the core ramps changes per block (default: yes for `continuous`) |

Rules for the table:
- **The core translates ranges.**
  - A plugin never sees the stock FX range table. Kennel plugins had to call `op1_fx_param_q15`, and a stored word
    out of range locked up the OP-1 at boot.
  - The core keeps whatever the stock patch JSON stores inside the stock ranges, and keeps the plugin's own values
    in its `"coop"` member.
- **Enumerations are `steps`, never quantised in the DSP.**
  - Kennel's `word*n>>15` and Opus's plate size on a continuous knob both go.
  - The core counts the steps, and the screen and MIDI see the step index.
- **24 actions is a ceiling, not a target.** Leave slots empty.

### 3.2 The action table (README)

Use the manifesto's format: rows are the eight actions (section 4), columns are blue, green, white and orange. Empty
cells are `—`.
Below the table, give:
- the upstream mapping it replaces (kennel push pages, Opus shift banks) and why anything moved;
- the memory, L1 and measured cost figures (section 2.4).

### 3.3 Value formats
- With no real unit, show **00-99** as stock does.
- Use a real unit when it is what the player thinks in: `MS` / `S`, `HZ` / `KHZ`, `DB`, `ST` (semitones), `CT`
  (cents), `BPM`, divisions (`1/16`), `%` for a mix.
  - Plan the unit changes before placing anything (`999 MS` becomes `1.00 S`).
  - Offsets keep their sign.
- `steps` show the step name. `switch` shows `ON` or `OFF`.
- The core formats the value from `format`, so every plugin's `MS` looks the same. A plugin with an unusual readout
  (outatime's year) declares `format: custom` and draws it itself.

## 4. Controls: the eight actions

Every encoder has eight actions: the manifesto's six, plus fn + turn and fn + click (the owner, 2026-11-10, for the
rings port: `docs/ui-features.md` §13). **The colour follows the encoder through every layer**: blue is blue on all
eight.

On a **synth engine's** page the core dispatches every layer but turn and shift + turn to the engine's declared
controls (`plugin.json` "controls", `docs/engines.md` "Controls") [now, e4]. Effects get the same layers with the v2
parameter store [proposed].

| Action | What goes here | Rule |
|---|---|---|
| **Turn** | The four things you play | MUST make the plugin complete on its own: a player who never presses anything still has the instrument |
| **Click** | Steps and switches (type, model, on/off), or a latch (freeze, capture, hold) | One step per click, cycling. Acts on **release**, so shift and fn combos never fire it |
| **Shift + turn** | Second row of continuous controls, or fine adjustment | See 4.1 |
| **Shift + click** | Second-row steps and switches | As click |
| **fn + turn** | A mode the sound is built on (model, polyphony, algorithm): steps or continuous | Changed between notes, not played; an engine latches it at note on |
| **fn + click** | A switch of the same kind | As click |
| **Shift + fn + turn** | The deep layer: trims, ranges, setup | Set once per sound, not played |
| **Shift + fn + click** | Rare commands: init, reset, mode | Anything destructive asks first (section 5.4) |

The rules:
- **Depth follows how often a control is touched.** If a deep control gets reached for every few bars, it's in the
  wrong layer.
- **Every layer is held.**
  - The screen shows a layer while its modifier is down and returns when it's released.
  - Both layers keep their values.
  - No plugin leaves the encoders in a hidden mode.
  - This retires kennel's per-encoder push "page 2" toggle, which left one encoder silently on page 2.
- **Stock page order:** T1 engine, T2 envelope, T3 effect, T4 LFO.
  - An engine that replaces T2 or T4 (Opus `OWN_ENVELOPE` / `OWN_LFO`, kennel Amp's tuner) declares it in its
    contract.
- **T3 toggles the focused effect's bypass** as in stock, shown `FX1 OFF` / `FX2 OFF`. Lift focuses FX 1 and drop
  FX 2 on the effect screen. Bypassed means no cost (section 2.4).
- **The per-preset controls coop adds** (bend amount and rate) live on the shifted T2 screen, not on an engine's
  page, so an engine keeps all four shifted encoders on T1 (`docs/ui-features.md` §2).
- **fn is the Help key.** A tap still opens help; held with another control it is fn (the core then sends no help).
  Help + encoder is the fn layer, shift + Help + encoder the deep layer; shift + Help + T1-T4 is solo on the tape.
  fn + an encoder had no meaning anywhere before e4 (checked: the bend and drum pan use shift, LFO 2 and FX 2 use lift
  / drop and their page keys).
- **Clicks on stock firmware:** stock ignores encoder clicks on every synth page, and never uses white and orange
  clicks at all [C, `te-op1-cfw/docs/screen-interface.md` §7].
  - The core takes clicks (codes 0x3B-0x3E) on plugin pages only. On an engine's page an undeclared layer does
    nothing and is consumed.
  - A plugin never handles keys itself.
- **The core shows what changed:** a declared control's name and value appear top left in its encoder's colour for
  about a second (e4), for every engine, with or without a screen; a plugin screen may draw more.
- **Randomise** (synth key + right arrow) draws a declared control within its steps, or 0..32767 when continuous,
  unless the control says `"random": false` (e4).

### 4.1 Shift + turn: second row or fine

Shift + turn is fine adjustment by default, and becomes the second row when a parameter is declared there.
Kennel and Opus disagree about shift: on kennel it gives cents on semitone controls; on Opus it gives fine steps, or
a second bank (chorus, plate, outatime, amp). coop settles it per encoder:

- **No `shift_turn` parameter declared on an encoder:** shift + turn is fine adjustment of that encoder's turn
  parameter. That's 1/16 of a coarse step, the stock mode-0 behaviour [C].
  - The screen says nothing: it's the same parameter.
- **A `shift_turn` parameter declared on an encoder:** that encoder has no fine adjustment.
  - Its turn parameter's coarse resolution has to be enough on its own. The default is 100 detents across the range;
    a plugin may declare more.
  - While shift is held, the screen shows the second row's names and values.
- **Steps parameters** never get fine adjustment.
- **Semitone controls** (pitch, key): turn moves semitones and shift + turn moves cents. This is the kennel
  `PAGE_SEMI` convention, and it counts as fine.

### 4.2 Resolution
- One detent is a core delta of 1024 [C on kennel hardware; coop P3a].
- Continuous parameters default to 1 % of the range per detent on turn, and 1/16 of that on fine.
- `steps` parameters move one step per detent.
  - A selector with many steps may declare `detents` per step (`plugin.json` "controls", e4; Nostromo used 2).
  - The 8-step kennel `coarse` knobs become `steps: 8`.
- Every turn of every plugin goes through the core, so acceleration, MIDI and the LFO behave identically everywhere.

### 4.3 Common meanings (SHOULD)

These meanings are shared so a player's hands carry over from plugin to plugin. They are the most common layouts in
kennel and Opus.

| Meaning | Where |
|---|---|
| Dry/wet **mix** | Orange. On the turn when it's one of the four played controls, otherwise shift + orange |
| **Freeze / hold / capture** latch | Orange click |
| An effect's **time / rate / size** | Blue turn |
| An engine's **main timbre** (shape, model, waveform family) | Blue. Turn if continuous, click if `steps` |
| **Level / output trim** | Deep layer (shift + fn + white or orange), never on the turn |
| **Init** | Shift + fn + blue click, after a confirm |

A port that moves a control to follow this table says so in its contract (section 7 has an example).

### 4.4 MIDI and modulation
- The stock CCs reach the four **turn** parameters of each page through the core store. Kennel uses CC 53-56 for
  the engine and 72-75 for FX; coop follows kennel's layout.
  - Plugins get CC control for free and MUST NOT read MIDI themselves.
  - CCs for the second row and the deep layer: [proposed].
- The stock LFO moves only parameters with `lfo: yes`, and moves them within their range.
  - The plugin receives the modulated value.
  - The screen shows the saved value, unless the screen is deliberately drawing live modulation (section 5.4).

## 5. Screens (summary of the manifesto)

The manifesto is the full guide, with numbers from R246's 61 stock screen files.

### 5.1 Colour and ink
- **The colour is the knob.**
  - The four brights are blue `698EFF`, green `00ED95`, white `FFFFFF` and orange `FF3A5D` (the colour stock
    actually draws with, not the swatch file's `FF423F`).
  - A bright means only "this encoder moves this part". Never use one as decoration or as a warning.
- **One dim body tone per screen** for the frame and fixed structure. The pencil is `DFD9FF` for structure no
  encoder owns.
  - An encoder's dark tone marks its unused parts.
- **Line, not mass.**
  - Strokes are 1.5 units with square caps; emphasis is 2.
  - Dots have radius 2.5 (smaller ones can vanish on hardware).
  - Near-black `010101` occluders hide back edges.
  - No gradients, glow, blur, transparency, or strokes of 3 or more.
- **Colour is never the only cue.** Keep parts in encoder order, or give each encoder a different kind of motion.

### 5.2 Layout and text
- The canvas is 320 x 160.
  - Keep 8 units clear left, right and top, and 16 at the bottom. Nothing that must be read sits below y 140.
  - Kennel lifts effect screens 12 units by itself; don't lift them twice.
- **No words at rest.**
  - A turn brings up that control's name and value in a fixed spot. The text goes away when the hand lifts.
  - Held layers list their names and values.
  - The deep layer may lead with text, with the drawing dimmed to its body tone.
- Text is capitals from the stock glyph set (written lowercase).
  - Don't shrink text to fit: shorten the name or change the format.
  - Anchor the name and value as a group, so a value growing from 9 to 100 doesn't drag its label.
- Lay the drawing out at its largest pose, so no setting at 100 runs into the text.

### 5.3 Renderer limits (MUST)
- The stock SVG subset only:
  - groups, paths (`M L C z`, at most 4 decimals, one contour per path), lines, circles, polylines, rects;
  - explicit paint, transforms flattened;
  - no text elements, CSS, gradients, filters, fonts or images.
- About 8 KiB of artwork. Keep still parts still, and switch poses by visibility.
- The screen runs on the GUI thread, once a frame. It reads values from the core and never from audio state the
  render writes mid-block.
- Never ship stock artwork or the stock font (`opfont.svg`) in a plugin.

### 5.4 State tells the truth
- Selected, playing and edited each get their own mark. So do draft, applied and saved.
- A latch shows that it's engaged. A `steps` change that rebuilds something heavy shows pending until it's done (the
  FX 2 page's "push to set" is the model).
- Empty, loading, unavailable and failed each get their own message.
- Before anything destructive, show the whole target and the whole action, and wait for a deliberate press.

## 6. Sounds and persistence (MUST)
- **Per-sound state is saved in the sound.**
  - Every `saved` parameter is stored by the core: the stock words inside the stock ranges, the rest in the sound's
    `"coop"` member.
  - A plugin keeps no hidden state that changes the sound.
  - Opus outatime moved its drive type out of a device-wide setting for exactly this reason.
- **Latches and commands are never saved.**
- **Device-wide settings** are for the device, not the sound: an input calibration, a display preference.
  ([proposed], at most 12 a plugin, as Opus has.)
- **A missing plugin never breaks a sound.**
  - The sound keeps its name and values, the plugin's number stays reserved, and the position plays the filter
    until the file returns. [now, P2]
  - An engine sound with its plugin missing says "needs <id>" instead of loading as something else. [proposed]
- **Versioning:** a plugin that changes its parameter table keeps the old `id`s working.
  - It maps old values in a `migrate` step ([proposed]).
  - A removed parameter's `id` is never reused.
- Loading a sound resets the effect's tail, as kennel does.

## 7. Porting

### 7.1 From kennel (hundekraft 3.1)

| Kennel | coop |
|---|---|
| `op1_fx_t.render(pos, buf[64], params)`, mono | `render(slot, l, r, params, n)`, dual-mono state per channel (section 2.2) |
| `op1_fx_stereo_t.render_stereo` (master only) | The stereo render, run at every position |
| `params[0..3]` in the stock FX ranges, `op1_fx_param_q15` | Q15 of the declared range (section 3.1) |
| `params[4]` selector `0x5300+n` | Owned by the core. Never read or written |
| `params[5..7]` page-2 words (white and orange share word 7) | Declared `shift_turn` parameters, each with its own storage |
| `press[]` `PAGE` / `PAGE_STEPS(n)` | a declared control: `fn_turn` (steps, e4), `click` / `shift_click` (steps), or `shift_turn` once v2 has it |
| `press[]` `LATCH` (freeze, capture) | `latch` on a click, usually the orange click |
| `PAGE_SEMI` (shift moves a cent) | Semitone turn, cents on shift + turn (section 4.1) |
| `coarse` 8-step knobs | `steps: 8` |
| Shift byte read from render (Steve's unison) | A declared `shift_click` switch |
| `OP1_VOICE_VEL`, `OP1_PITCH_INC`, `op1_eng_page`, screen node addresses | Engine ABI arguments (`velocity`, `pitch[]`), `coop_scr_words` / `coop_scr_mods` and the SDK screen calls (e4) |
| `op1_l1_claim`, `op1_fx_big`, `OP1_BIGBUF` | `coop_l1_lease` (persistent, from `init`), `coop_l1_scratch` (per call) and `.bss` (section 2.3) |
| `cost` in `plugin.json` | Measured device figures in the contract (section 2.4) |

### 7.2 From Opus 2

| Opus | coop |
|---|---|
| `mono(fx, buf, par)` on synth and drum, `stereo` on master | One stereo render at every position. A mono-only effect becomes dual-mono |
| `limits[8][3] {min, max, step}` | The parameter table; `step` becomes the coarse resolution |
| `shift_knob` returning 0 (OS fine) | Default fine (no `shift_turn` declared) |
| `shift_knob` returning 1 (a second bank in params 4-7) | Declared `shift_turn` parameters |
| `need()` with `state` / `bulk` / `fast`, core-allocated per instance | `.bss` per relocation, an L1 lease for the fast part |
| `opus_on_key` / `opus_on_knob`, engine `turn` writing knobs itself | Not allowed. Every control is declared and the core dispatches it |
| Floats outside audio (zoom) | Not allowed (section 2.1) |
| `cpu.txt` worst block | The contract's device cost figures |
| `uses` lines with clash refusal | Kept as the model for tool plugins ([proposed]) |
| Role-named colours (`OPUS_COLOR_E1..E4`) | The four brights by encoder (section 5.1) |

### 7.3 Worked example: HAIL (a kennel FX)

Upstream mapping (kennel `hail.c:651`):
- turn: Time, Size, Shape, Density;
- push pages: blue Pitch (`PAGE_SEMI`), green Mix, white Space;
- orange push: a freeze latch.

The coop contract (proposed):

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | time | size | shape | density |
| Click | — | — | — | freeze (latch) |
| Shift + turn | pitch, ST (shift + fn + blue gives CT) | space | — | mix |
| Shift + click | — | — | — | — |
| Shift + fn + turn | pitch fine, CT | — | — | out trim |
| Shift + fn + click | init (confirm) | — | — | — |

What moved and why:
- **Mix** moved from green to orange (section 4.3).
- **Pitch** can't take fine adjustment on shift + turn, because it already sits there. Its cents moved to the deep
  layer.
- **The pages became held layers.**

### 7.4 Port checklist
1. Licence checked, and the upstream firmware, version and author recorded in `description`.
2. Every float, libc call, static constructor and firmware address removed (section 2.1).
3. Stereo: per-channel state, or true stereo with a flag. No mono collapse. Every position tested.
4. Parameter table and action table written (section 3). Upstream mapping changes explained.
5. Enumerations moved out of the DSP into `steps`.
6. Screen redrawn to the manifesto, or the stock FX screen kept until P3 (not upstream art carried across unchecked).
7. Host test against the upstream C reference where one exists (`tests/sim`, as for the stock rewrites).
8. op1emu gate: the plugin selected at every position, a chord through it, no fault, the stats line clean. Gate runs
   go one at a time.
9. Device run by the owner, with the `cost` figures from the stats file recorded in the contract and the result in
   `docs/device-tests.md`. **Not done before this.**

## 8. Release checklist (every plugin)
- `make -C plugins` packs it with no warnings, and `tools/asm_lint.py` is clean.
- The contract is complete: parameter table, action table, cost, memory, L1.
- Every state checked on screen:
  - minimum, maximum, the longest name, signs, unit changes;
  - every layer held and released;
  - bypass, empty and error.
- It works with the four turns alone.
- A saved sound reloads the same, both with the plugin and with the plugin removed.
- The device result is in `docs/device-tests.md`.

## Decided (2026-11-07, `docs/ui-features.md`)
- **fn is Help.** The FX 2 page no longer uses shift + Help: FX 2 is the green column of the FX list, focused by drop.
- **Shift policy (section 4.1)** stands: fine unless a second row is declared on that encoder.

## Open decisions for the owner
1. **Common meanings (section 4.3).** Mix on orange and freeze on the orange click follow most of kennel and Opus.
   Confirm this before ports start remapping.
2. **MIDI CCs** for the second row and the deep layer, which neither upstream has.
3. **Over-budget switch-off (section 2.4).** Adopt the Opus rule, or only report the plugin in the stats file.
