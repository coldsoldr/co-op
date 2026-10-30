# A prompt for preparing a plugin

Every plugin in co-op's library looks the same: a screenshot, a short
description, the same details on its card, and a manual with the same
sections. If you wrote the plugin with a coding agent (Claude Code, Codex,
Cursor or similar), the agent has the source in front of it and can prepare
all of this for you. Copy the prompt below into the agent, from the folder
that holds the plugin's source, and read what it writes before you submit it:
you know how the plugin behaves, the agent only knows what the code says.

The Plugins tab shows this same prompt under **Submit a plugin**, with a Copy
button. The guide to submitting a plugin is in
[contributing-plugins.md](contributing-plugins.md).

## The prompt

````text
Prepare this OP-1 plugin for co-op's plugin library
(https://coldsoldr.github.io/co-op/). Every plugin there has to look the same,
so follow this exactly.

First read the plugin's source: the DSP, the parameter and encoder handling,
the screen drawing code, what it stores with a sound, and any README or
comments. Write only what the source shows. If you cannot tell how something
behaves, leave it out or write "not documented"; never guess.

Make one folder named after the plugin's id, holding exactly four files and
nothing else, spelled exactly as here:

  <id>/plugin.json      describes the plugin
  <id>/<id>.knl         the built plugin (<id>.op1p for op1r, <name>.opmod for opus)
  <id>/manual.md        the manual
  <id>/screenshot.png   a picture of its screen

The id: for hundekraft, the name the OP-1 stores in a sound with spaces as _
("tape echo" is tape_echo), 1 to 15 lower-case letters, digits, _ or -. For
opus, 1 to 40 lower-case letters, digits, _, . or -.

## plugin.json

Use these fields, in this order, as JSON with two-space indents:

- "format": 1
- "id": the id, the same as the folder name
- "name": as the OP-1 shows it
- "firmware": "op1/hundekraft" or "op1/opus"
- "kind": "synth", "fx" or "feature" (a sequencer or tool)
- "author": the name to show, as I want to be credited
- "version": the same version as the built file, if it carries one
- "description": what it does in one or two plain sentences, at most 500
  characters, without repeating its name. For example: "Sample-rate and
  bit-depth reduction with a jittery clock."
- "license": one of GPL-3.0-or-later, GPL-3.0-only, GPL-2.0-or-later, MIT,
  BSD-3-Clause, Apache-2.0, MPL-2.0, or LicenseRef-Freeware for a closed plugin
  that is free to use. A plugin built on an SDK follows that SDK's licence
  (hundekraft's is GPL-3.0-or-later).
- "file": the plugin file's name
- "screenshot": "screenshot.png"
- "manual": "manual.md"
- "source": an https:// link to the source. Ask me for it if you cannot find
  it: a plugin without one is unlikely to be accepted.
- "requires": the firmware and version it needs, such as "hundekraft 3.0.3"
- "credits": only if it ports someone else's work: whose code or algorithms,
  with their licence, at most 200 characters
- "tags": up to 8, each 1 to 20 lower-case letters, digits or hyphens. Start
  with what it is ("synth", "effect", "sequencer"), then what it sounds like
  or comes from ("lo-fi", "mutable-instruments").
- "savedName": for opus only, "opus.<id>"

## screenshot.png

The plugin's screen on its own, running with a sound playing, on black, as a
PNG of exactly 960 by 480 pixels (the OP-1's 320 by 160 screen at 3x, with no
smoothing), at most 1 MB. No device, no hands, no text added. If you cannot
produce this yourself, say so and I will capture it.

## manual.md

The manual is shown in co-op's plugin library next to the screenshot, to people
who have never seen the plugin.

Use exactly these sections, in this order. Leave out a section only where it
says so.

1. `# <plugin name>` as it appears on the OP-1, then one line saying what it
   is in plain words (for example "A bit crusher: lower sample rate and bit
   depth, with a jittery clock.").
2. The screenshot line, exactly: `![<plugin name> screen](screenshot.png)`.
   The picture is the screenshot.png beside manual.md; do not link images
   anywhere else.
3. `## What it is`: one paragraph. What it does to the sound or what it
   plays, how it works in plain words, and what it is good for.
4. `## Before you start`: a short list. The OP-1 model it runs on (the
   original OP-1 or which other), the firmware and version it needs, and how
   to install it (from co-op's Plugins tab, the firmware's own tools, or a
   folder on the OP-1's drive). Say that a plugin runs as firmware code, and
   how to get the OP-1 going again if it fails to start, if the firmware has
   a way.
5. `## Controls`: a table with these columns:

   | Encoder | Main page | Press, then turn |
   |---|---|---|
   | blue | ... | ... |
   | green | ... | ... |
   | white | ... | ... |
   | orange | ... | ... |

   One row per encoder, always in the order blue, green, white, orange.
   Bold the control's name and follow it with what it does: `**rate**: the
   sample rate, clockwise = higher`. Give units and ranges where the source
   has them (Hz, dB, ms, steps), and which way is which. Leave a cell empty
   when the encoder does nothing there. If the plugin has pages that do not
   fit these columns (a sequencer with two pages, say), use the same idea
   with its own column names and keep one row per encoder.
6. `## Second pages and Shift`: what pressing each encoder does (a second
   page while held, a toggle, a latch), what holding Shift changes (fine
   steps, other parameters, other actions), and any other key the plugin
   uses. Leave this section out if the table already says all of it.
7. `## On screen`: what each thing on the screen shows and which knob moves
   it. On the OP-1 the colour of a thing on screen is the colour of the
   encoder that moves it; say so where it holds.
8. `## Tips`: how to get good sounds out of it, what it pairs well with,
   and CPU notes: whether it is heavy, what to turn down when the OP-1
   crackles, whether it runs mono or stereo, and any known limits.
9. `## What is saved`: which settings are stored with the sound (or project,
   or the OP-1's own memory) and which are not (momentary latches, for
   example).
10. `## Credits and licence`: who made it, whose code or algorithms it
    ports (with their licence), the SDK it is built on, and the plugin's own
    licence.

Style, for the description as well as the manual:

- Short sentences. Plain words. Explain a technical term the first time it
  appears.
- British spelling (colour, behaviour, anticlockwise, licence as a noun).
- No marketing words ("stunning", "powerful", "amazing", "next-level") and
  no exclamation marks. Describe a sound the way a musician would.
- Name the OP-1's encoders by colour: blue, green, white, orange. Name keys
  as the OP-1 prints them (Shift, T1 to T4, the sequencer key).
- Give units and ranges where the source has them.
- Never describe behaviour that is not in the source.
- Plain Markdown only: headings, paragraphs, lists, tables, bold, italics,
  code, and links to https:// pages. No HTML, no images other than the
  screenshot line.
- Keep it under 64 KB; a good manual is one or two screens long.

Make sure the built plugin's file name and the names in plugin.json agree, and
that any screen SVG has no number with more than four decimals (it hangs the
OP-1 at boot).

When you are done, show me the folder and list anything you were unsure about
so I can check it. Do not open a pull request yourself.
````

## An example

This is the manual of the bitcrusher in co-op's library
([plugins/bitcrusher/manual.md](../plugins/bitcrusher/manual.md)), written
from its author's notes. Use it to see what the prompt asks for.

````markdown
# bitcrusher

A bit crusher: lower sample rate and bit depth, with a jittery clock.

![bitcrusher screen](screenshot.png)

## What it is

A **bit crusher**. It lowers the sample rate (each sample is held for
longer, which turns the sound gritty and adds metallic aliasing) and the bit
depth (fewer levels, so it gets noisy and steppy). **Jitter** makes the
sample clock unsteady, like a dying sampler. From a subtle 12-bit grit to
2-bit destruction.

## Before you start

- **An original OP-1** (the 2011 model). It does not run on the OP-1 field
  or the OP-Z.
- **hundekraft 3.0.3** on it, from the
  [hundekraft workshop](https://hundekraft.github.io/op1-workshop/).
- Install it from co-op's Plugins tab over USB, or drop `bitcrusher.knl`
  onto **Your plugins** in the hundekraft workshop. Either way the OP-1 asks
  you to press T1 to install, then restarts.
- A plugin runs as firmware code: back up your tape and sounds first. If the
  OP-1 ever fails to start after you add a plugin, switch it off and on
  again: it starts once without plugin files, and you can remove the file
  from the workshop.

## Controls

| Encoder | Main page | Press, then turn |
|---|---|---|
| blue | **rate**: the sample rate, clockwise = higher, cleaner; anticlockwise down to 500 Hz | **input gain** −6 to +6 dB (Shift: tenths) |
| green | **bits**: the bit depth, clockwise = more bits; anticlockwise down to 2 | |
| white | **jitter**: randomness of the sample clock | |
| orange | **mix** | |

## Second pages and Shift

- Pick it as any effect: hold Shift and press T3 on a synth or drum sound,
  or on the master in the mixer, and choose **bitcrusher**.
- Pressing **blue** turns the blue knob into the **input gain** (−6 dB to
  +6 dB, 0 dB at the centre) while it is pressed, and the screen shows a
  bracket and a dB readout. Release the knob and the page closes.
- On the second page, holding Shift makes each click a tenth of a dB. On the
  main page the OP-1's own Shift fine-adjust applies as it does to the stock
  engines.

## On screen

A smooth wave and its stepped copy. The steps get wider as the rate drops,
the green ladder on the left gains rungs as the bits go (the levels the
sound is rounded to), and the row of clock ticks along the top shakes with
jitter. The dot runs along the stairs at the sample clock's speed. The blue
bar at the left edge is the input gain; it warms towards white as the input
gets loud. The orange bar at the right edge is the mix.

## Tips

- It is stereo on the master and mono on a synth or drum sound.

## What is saved

The four knobs and the input gain are stored with the sound, as the stock
knobs are.

## Credits and licence

- The DSP, the OP-1 glue and the screen are coldsoldr's own work, built on
  hundekraft's kennel plugin SDK.
- Licence: GPL-3.0-or-later, with the OP-1 firmware permission of the SDK.
  Source: https://github.com/coldsoldr/te-op1-cfw
````
