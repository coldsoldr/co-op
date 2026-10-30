# audition

Plays patches from co-op on your OP-1, without disk mode.

![audition screen](screenshot.png)

## What it is

A tool with no screen of its own. It listens for co-op over USB, loads the patch you click onto the
current synth or drum slot, and keeps a copy of your own sound so you can put it back. The patch is only in
memory: the slot's saved sound does not change unless you change the sound yourself.

## Before you start

- **An original OP-1** (the 2011 model).
- **hundekraft 3.0.3** on it, from the
  [hundekraft workshop](https://hundekraft.github.io/op1-workshop/).
- Install it from co-op's Plugins tab over USB, or with the hundekraft workshop. The OP-1 asks you
  to press T1, then restarts.
- A plugin runs as firmware code: back up your tape and sounds first. If the OP-1 ever fails to
  start after you add a plugin, switch it off and on again: it starts once without plugin files.

## Controls

| Encoder | Main page | Press, then turn |
|---|---|---|
| blue | | |
| green | | |
| white | | |
| orange | | |

It uses no encoders or keys. Everything is done from co-op.

## On screen

Nothing of its own. The OP-1 shows the patch as usual, and a sampler patch shows its waveform.

## Tips

- Plug the OP-1 in normally, not in disk mode, and press the synth key, or the drum key for a kit.
- In co-op, press Connect for audition, then click a synth patch. The row fills as the patch loads.
- A settings-only patch loads at once. A sampler patch sends its audio, about 21 seconds for the
  full 6 seconds of sound. Clicking another patch cancels the transfer.
- A sampled drum kit sends its audio too, about 40 seconds for the full 12 seconds of sound. A dbox
  kit is settings only, so it loads at once. A Gin kit is not auditioned on the OP-1.
- A sampler or kit slot sounds scrambled while its row fills, so wait until it finishes. It needs
  no plugin memory: your slot's audio is kept in a file of up to about 1 MB on the OP-1 until you
  put it back.

## What is saved

Nothing of your sound, by the plugin. Turning a knob during an audition lets the OP-1 save that sound over the
slot. A sampler's audio is not saved even then, so after a restart such a slot holds the new
settings with its old audio. Use Put my sound back before you edit.

## Credits and licence

- By coldsoldr, built on hundekraft's kennel plugin SDK.
- Licence: GPL-3.0-or-later, with the OP-1 firmware permission of the SDK.
  Source: https://github.com/coldsoldr/te-op1-cfw/tree/dev/ports/knl/audition
