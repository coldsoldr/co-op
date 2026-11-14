# Pulse Sync

*Clock the OP-1 from a pulse on its line input.*

Syncs the OP-1 to an analog pulse clock on its line input, so a drum machine, sequencer or modular clock can set the tempo. It reads 24, 48 or 96 PPQ, or quarter to thirty-second note pulses, turns them into the OP-1's 24 PPQ MIDI clock, and can mute the clicks from monitoring.

## Using it

Choose line in, keep the input on, and pick Sync on the tempo screen. Set Input Rate to match the clock. Its other settings are in the Opus mods list: select Pulse Sync and press blue.

- **Pulse Threshold**, 1–10 (default 5): higher needs a stronger pulse.
- **AutoStart** (default off): sends MIDI start on the first pulse and stop once the pulses stop.
- **Clock Priority** (default USB): which clock wins when USB MIDI and pulses both run; it falls back to the other when one stops.
- **Mute Clock Input**: keeps the clicks out of monitoring on line in, while still reading them.

Use a line-level signal and turn modular clocks down before plugging them in.

## Who made it

Pulse Sync is by ESC_CODE (Volker Junginger) ([VolkerJunginger/OP-1](https://github.com/VolkerJunginger/OP-1), GPL-3.0), built with LizardMan's Opus 2 plugin kit.

## Get it

Pulse Sync 1.0.7 needs Opus 2 on the original OP-1 (firmware 246). Download `pulsesync.opmod` from the [Pulse Sync folder](https://github.com/VolkerJunginger/OP-1/tree/main/Pulse-Sync), copy it to the `mods` folder on the OP-1's disk and restart. co-op lists it for reference and hosts no copy.

## Links

- [Pulse Sync: download and setup](https://github.com/VolkerJunginger/OP-1/tree/main/Pulse-Sync)
- [Manual](https://github.com/VolkerJunginger/OP-1/blob/main/Pulse-Sync/MANUAL.txt) and [changelog](https://github.com/VolkerJunginger/OP-1/blob/main/Pulse-Sync/CHANGELOG.md)
- [Source](https://github.com/VolkerJunginger/OP-1/tree/main/Pulse-Sync/Source)
