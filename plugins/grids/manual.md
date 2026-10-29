# grids

Mutable Instruments Grids as a drum sequencer: a map of drum grooves, with
Euclidean mode.

![grids screen](screenshot.png)

## What it is

**Mutable Instruments Grids**, the topographic drum sequencer. It holds a
5 × 5 map of 25 drum grooves, each a kick, snare and hat pattern of 32 steps
where every hit has a level saying how essential it is. The map position
morphs between the four nearest grooves, each part's **fill** decides how
many of its hits play (low keeps only the strongest, high adds ghost notes),
and **chaos** perturbs the levels for each bar. Hits above a threshold are
accents. **Euclidean** mode replaces the map with three Euclidean rhythms.

**Playing it.** Switch the sequencer on, select grids and hold a key: the
bar starts on your press at the project tempo. Shift while holding latches
it, the next key takes over, the last key up stops it. The keys do not
sound themselves while Grids is the sequencer; they are its transport.

**Which drums it plays.** By default the kick is key 0 (the low F), the
snare key 2 (G) and the hat key 8 (C#), which fits a stock kit. To change
them press white: the arrow points at the part being assigned, and the next
key you press becomes that part's sound (it plays once so you can hear it).
White steps through kick, snare, hat, then the three **accent keys** (an
accented hit plays this key instead of louder: a second kick, a rim, an open
hat), then off. The keys are the transport again once the arrow is gone.
Shift + green forgets the accent keys, back to louder hits. Shift + white
turns the **random voice** on: a fourth part of random hits, played on key
12 (the upper F).

## Before you start

- **An original OP-1** (the 2011 model). It does not run on the OP-1 field
  or the OP-Z.
- **hundekraft 3.0.3** on it, from the
  [hundekraft workshop](https://hundekraft.github.io/op1-workshop/).
- Install it from co-op's Plugins tab over USB, or drop `grids.knl` onto
  **Your plugins** in the hundekraft workshop. Either way the OP-1 asks you
  to press T1 to install, then restarts.
- A plugin runs as firmware code: back up your tape and sounds first. If the
  OP-1 ever fails to start after you add a plugin, switch it off and on
  again: it starts once without plugin files, and you can remove the file
  from the workshop.

## Controls

| Encoder | Drums | Drums, Shift held | Euclidean | Euclidean, Shift held |
|---|---|---|---|---|
| blue | **map X** | kick fill | kick length | kick hits |
| green | **map Y** | snare fill | snare length | snare hits |
| white | **chaos** | hat fill | hat length | hat hits |
| orange | **swing** | **accent** | swing | accent |

## Second pages and Shift

Grids has no second pages; pressing an encoder is an action.

| Press | Action | With Shift |
|---|---|---|
| blue | drums / Euclidean | 16ths / 32nds |
| green | a new chaos draw at the next bar | forget the accent keys |
| white | assign the next key, see above | random voice on / off |
| orange | | **print**: write the last bar into the OP-1's own pattern sequencer as editable steps |

- Pick it as a sequencer: hold Shift and press the sequencer key and pick
  **grids**.
- The print is the way to keep a groove: find a bar you like, Shift +
  orange, then switch the sequencer to pattern and edit it there.

## On screen

The map at the left with the crosshair at X, Y (in Euclidean mode the
crosshair goes away). The kit in the middle lights on each hit and jumps
bigger on an accent; the bars at the right are the knobs of the page you are
on, and switch to the fills while Shift is held. The playhead runs along the
step ruler at the bottom. The icons at the top show the mode, the
resolution, the accent mode, the random voice and the assign arrow.

## Tips

- Grids is a drum sequencer, but it is in the synth list too: on a synth the
  three parts play the notes of keys 0, 2 and 8.
- Grids shares the stock sequencer switch. It only gets keys and ticks while
  the sequencer is on, exactly like tombola or finger. Lift, drop and
  scissors do nothing on its screen.
- The assign stays armed after it has captured a key until you cycle white
  round to off. The arrow on screen tells you.

## What is saved

Its settings are saved in the OP-1's database, so they survive a restart.

## Credits and licence

- Ported from Mutable Instruments Grids by Émilie Gillet (GPL v3), bit-exact
  where the hardware allowed and credited in each source file.
- The OP-1 glue and the screen are coldsoldr's own work, built on
  hundekraft's kennel plugin SDK.
- Licence: GPL-3.0-or-later, with the OP-1 firmware permission of the SDK.
  Source: https://github.com/coldsoldr/te-op1-cfw
