# Crossword

*A step sequencer laid out as a crossword.*

Crossword by displayname808 is a step sequencer laid out as a crossword: a grid of 256 squares where white squares play notes and black blocks rest. It plays sections of up to 32 steps along one of 13 paths, with step probability, trig conditions and 20 modifiers such as ratchets, octave jumps and neighbour notes. It has 100 patterns, 15 scales and random patterns with undo.

## Controls

| Encoder | Turn | Shift + turn | Push |
|---|---|---|---|
| Blue | How many steps play (1 to 32) | The start point (0 to 255) | The path menu: 13 paths, and forward, backward, ping pong or random |
| Green | Moves the cursor along the grid | Moves the cursor a row up or down | Makes the cursor's square a note or a block |
| White | Step probability (10 to 100 %), or a trig condition further right | Modifier probability | The modifier menu |
| Orange | The note of the cursor's square | Hold on or off | The scale and pattern menu |

- Shift + the arrow keys moves the cursor one square left or right.
- The sequencer key turns the sequencer on and off; tap fn for an overview of the controls.
- In the scale and pattern menu, turn orange to "new random" and push to make one; it then offers "undo random", then "redo random". Undo belongs to the current pattern and is cleared when you switch or delete a pattern or restart.

## Using it

- **Step probability**: each note sounds with its own chance, 10 to 100 % in steps of 5. A note that misses is a rest; the timing stays the same.
- **Trig conditions**: a step plays only on some passes through the section (1:2 plays on the first of every two, 3:4 on the third of every four). A step has a probability or a condition, not both.
- **Modifiers**: one per square, each with its own chance (10 to 100 %) or trig condition. Ratchet 2, 3 or 4; hold and hold 6 (3 or 6 repeats); octave up or down one or two; divide 2 or 4 (half or quarter speed); neighbour left, right, up or down (plays the note of the square beside it); random note; reset (back to the first step); jump (to a random place on the path for the rest of the section); flip direction; and pre (plays only if the last earlier conditional step fired).
- Each square holds one of 49 notes, −24 to +24 scale degrees, in 15 scales and 12 roots.
- OPUS puts audio before graphics, so the screen can drop frames with the stock synth engines.

## Who made it

Crossword is by displayname808, made for OPUS by LizardMan. It's a free download, with an optional donation. Its page names no licence and links no source.

## Get it

Crossword 1.0.0 runs on the original OP-1 with OPUS core 2.31 or newer. Install OPUS first, then download `crossword.opmod` from [Crossword's page](https://displayname808hub.github.io/web/crossword/), copy it into the OP-1's `mods` folder in disk mode, eject and restart. Hold shift and press the sequencer key, choose crossword, and play a key. co-op lists it for reference and hosts no copy.

## Links

- [Crossword's page](https://displayname808hub.github.io/web/crossword/): the download, the cheat sheet, the trailer and install steps
- [The cheat sheet](https://displayname808hub.github.io/web/assets/crossword-cheat-sheet.png)
- [OPUS](https://opus-op1.pages.dev/)
