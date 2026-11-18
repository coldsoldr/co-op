# FLUX

*Two LFOs for every HITS pad.*

Two LFOs for every HITS pad, on the drum LFO page. Each has its own wave, rate, depth and target, and keeps running between hits, so the next hit picks up where it is. It needs HITS.

## Controls

With HITS as the drum engine, choose FLUX as the LFO with shift + T4, and turn it on. Hold shift and press a key to pick the pad. Push blue to switch between LFO A and B; the red waveform shows which one you edit.

| Encoder | Sets |
|---|---|
| Blue | Wave: sine, triangle, ramp up, ramp down, square |
| Green | Period, 20 ms to 10 s |
| White | Depth, up or down, 0–100% |
| Orange | Target: length, level, pitch, timbre, color, low cut, high cut |

A starts on length and B on level, both at zero depth. Both can modulate the same target, and pitch moves up to an octave either way. There is no retrigger or tempo sync. With FLUKE as the drum sequencer, orange push toggles its hold. Turning an encoder faster moves its value further, so long sweeps take fewer turns.

## Who made it

FLUX is by ESC_CODE (Volker Junginger) ([VolkerJunginger/OP-1](https://github.com/VolkerJunginger/OP-1), GPL-3.0), built with LizardMan's Opus plugin kit.

## Get it

FLUKE, HITS, FLUX and GRIP come as a set from the [device page](https://volkerjunginger.github.io/OP-1/), one `.opmod` each or all four in a zip, for Opus core 2.57 or later on the original OP-1 (firmware 246). Copy the files to the `mods` folder on the OP-1's disk, eject and restart; replace older copies by file name and remove an old `hitsfluke.opmod`. co-op lists it for reference and hosts no copy. FLUX needs HITS 1.0.0 or later, and shows “needs HITS” without it.

## Links

- [Device page](https://volkerjunginger.github.io/OP-1/#flux): downloads and every device
- [Install, credits and source](https://github.com/VolkerJunginger/OP-1/tree/main/Fluke)
- [Illustrated manual (PDF)](https://github.com/VolkerJunginger/OP-1/blob/main/docs/manuals/FLUKE%20v.2.0.0%20Manual.pdf)
