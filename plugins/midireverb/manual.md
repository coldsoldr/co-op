# Midi-Reverb

*The Alesis MidiVerb and MidiFex programs, from your own ROM dumps.*

A mono reverb and effects unit that runs the programs of three late-80s Alesis ROMs: MidiVerb I, MidiVerb II and MidiFex. You supply your own ROM dumps; none come with it. Feedback, Cutoff, Mix and Program on the four encoders, with an Original engine and an approximate Low CPU one.

## Controls

| Encoder | Turn | Press |
|---|---|---|
| Blue | Feedback | |
| Green | Cutoff, 1–11 kHz | |
| White | Mix | |
| Orange | Program | Next ROM |

The ROMs go MidiVerb I, MidiVerb II, MidiFex, and each remembers its own program. Changing ROM, program or engine clears the tail. Two instances keep their own settings and delay memory. In the plugin's Opus settings, Output-Filter (on by default) is the wet-only reconstruction filter, and Engine picks Original or the approximate Low CPU tank. It is mono, for synth and drum effects only; on the master it passes audio through.

## ROMs

No ROMs come with it. Put your own dumps in `mods/mv1rom/` on the OP-1, named `midiverb.bin` (MidiVerb I), `midiverb2.bin` (MidiVerb II) and `midifex.bin` (MidiFex); the [README](https://github.com/VolkerJunginger/OP-1/tree/main/Midi-Reverb) lists the size and checksum each must match. A missing or wrong one is skipped. Midi-Reverb isn't affiliated with or endorsed by Alesis.

## Who made it

Midi-Reverb is by ESC_CODE (Volker Junginger) ([VolkerJunginger/OP-1](https://github.com/VolkerJunginger/OP-1), GPL-3.0). It builds on Eric Brombaugh's [MIDIVerb_RE](https://github.com/emeb/MIDIVerb_RE), with Paul Schreiber's reverse engineering, and thement's [midiverb_emulator](https://github.com/thement/midiverb_emulator), with LizardMan's Opus plugin kit.

## Get it

Midi-Reverb 1.0.0 needs Opus core 2.31 or later on the original OP-1 (firmware 246). Download `midireverb.opmod` from the [Midi-Reverb folder](https://github.com/VolkerJunginger/OP-1/tree/main/Midi-Reverb), copy it to the `mods` folder on the OP-1's disk and restart. co-op lists it for reference and hosts no copy.

## Links

- [Midi-Reverb: download, ROMs, install and controls](https://github.com/VolkerJunginger/OP-1/tree/main/Midi-Reverb)
- [Source](https://github.com/VolkerJunginger/OP-1/tree/main/Midi-Reverb/Source)
