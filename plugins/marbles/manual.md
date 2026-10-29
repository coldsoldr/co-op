# marbles

Mutable Instruments Marbles as a sequencer: random rhythms and melodies with
memory.

![marbles screen](screenshot.png)

## What it is

**Mutable Instruments Marbles**, the random sampler, as a synth sequencer. A
random rhythm generator (**T**) fires three gates in time with the tempo,
and random voltages (**X**) quantized to a scale give each gate its pitch:
gate 1 plays voltage 1, gate 2 voltage 2, gate 3 voltage 3, so they overlap
into chords. A fourth, smooth voltage sets the velocity. **Deja vu** is the
memory: off, every step is fresh; on, the last loop repeats with the amount
of variation the knob allows; locked, it repeats exactly.

**Playing it.** Switch the sequencer on with the sequencer key, then hold a
key: Marbles runs from the moment you press, at the project tempo. Press
Shift while holding to latch it, and the next key you press takes over (the
latch clears). Letting go of the last key stops it. A key pressed while
Shift is already down plays through as a normal note.

Where the notes come from is the **pitch source**, Shift + white:

- **scale** (default): the key you hold is the root and the voltages spread
  over two octaves above it, quantized to the chosen scale.
- **keys**: the chord you hold is the pool and X picks notes from it. Latch
  a chord and let Marbles wander through it.

## Before you start

- **An original OP-1** (the 2011 model). It does not run on the OP-1 field
  or the OP-Z.
- **hundekraft 3.0.3** on it, from the
  [hundekraft workshop](https://hundekraft.github.io/op1-workshop/).
- Install it from co-op's Plugins tab over USB, or drop `marbles.knl` onto
  **Your plugins** in the hundekraft workshop. Either way the OP-1 asks you
  to press T1 to install, then restarts.
- A plugin runs as firmware code: back up your tape and sounds first. If the
  OP-1 ever fails to start after you add a plugin, switch it off and on
  again: it starts once without plugin files, and you can remove the file
  from the workshop.

## Controls

Marbles has two pages, **T** (rhythm) and **X** (pitches); press white to
switch.

| Encoder | T page | X page | Press |
|---|---|---|---|
| blue | **rate**: 1 bar to 1/32, with triplets | **spread**: how wide the voltages range | T page: the **T model**; X page: the **scale** (Shift: backwards) |
| green | **deja vu** amount | **length**: the loop, 1 to 16 steps | the page's deja vu **off / on / locked** (Shift: backwards) |
| white | **jitter**: timing randomness | **steps**: from every note of the scale to the root only | the **page** T / X; Shift + white: pitch source scale / keys |
| orange | **T bias**: which gates fire more | **X bias**: low or high notes more | |

## Second pages and Shift

- Pick it as a sequencer: hold Shift and press the sequencer key and pick
  **marbles**.
- Shift + turn moves any knob in eighths of a click.
- The seven T models are Marbles' own: complementary Bernoulli (a coin toss
  sends each clock to gate 1 or gate 3 while gate 2 keeps the clock),
  clusters, drums, independent Bernoulli, a clock divider, three states and
  a Markov chain.
- The six scales are Marbles' presets (major, minor, pentatonic, a gamelan
  scale and two ragas).

## On screen

Three lanes, one per gate, show the last sixteen steps as dots, raised by
their pitch. The bracket above spans the loop while deja vu is on (bright
when locked). At the top left, the two squares are the pages, the row of
dots is the model (T page) or the scale (X page), and the staircase and
keyboard icons are the pitch source. Along the bottom the page's three knobs
sit next to their icons with bars for their values; the bias bar is at the
top right.

## Tips

- Marbles is meant for the synth, but it is in the drum sequencer list too:
  on a kit the pitches pick drum sounds, so the rhythm models land on your
  drums.
- Marbles shares the stock sequencer switch. It only gets keys and ticks
  while the sequencer is on, exactly like tombola or finger. Lift, drop and
  scissors do nothing on its screen.

## What is saved

Its settings are saved in the OP-1's database, so they survive a restart.

## Credits and licence

- Ported from Mutable Instruments Marbles by Émilie Gillet (MIT), bit-exact
  where the hardware allowed and credited in each source file.
- The OP-1 glue and the screen are coldsoldr's own work, built on
  hundekraft's kennel plugin SDK.
- Licence: GPL-3.0-or-later, with the OP-1 firmware permission of the SDK.
  Source: https://github.com/coldsoldr/te-op1-cfw
