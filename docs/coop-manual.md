# coop user manual

This manual covers coop e10, the build whose boot picture reads `coop e10 - 246`.

## What coop is

coop is a custom firmware for the original OP-1, built on Teenage Engineering's firmware R246. The stock firmware stays the operating system: the pages, the engines, the sequencers and the menus you know are still there. coop replaces the audio core and adds features on top. Your instruments, effects and tape are stereo. Each sound gets a second effect and a second LFO. You can load extra synth engines and effects as plugin files. The co-op website builds coop from your own copy of the stock `op1_246.op1`, so no Teenage Engineering firmware is ever shared.

## Before you flash

### Back up your tape

The first boot of coop reformats the tape. coop replaces the stock tape image with its own: four stereo tracks and no album. Every tape recording and both album sides are lost. Back the tape up first, with co-op or in disk mode.

Your synth and drum presets are not touched.

### What changes

- The tape becomes four stereo tracks. The album is gone.
- Synths, drums, both effect slots and the master are stereo.
- Each synth, drum and master position gets a second effect slot (FX 2), and each sound a second LFO (LFO 2).
- The lander game is gone. Its key combination opens coop's settings menu.
- The Help key also works as a modifier, called fn in this manual. A tap still opens help.
- The boot picture shows four coloured dots, the co-op wordmark and the line `coop e10 - 246`.

### What doesn't change

- te-boot, the bootloader you reach by holding COM at power-on. A coop package never contains the bootloader, so flashing coop cannot rewrite it. The recovery path stays exactly as it was (see "Getting back to stock").
- Your presets and the rest of the OP-1's disk.
- The stock pages and controls, except where this manual says otherwise.
- The speaker. It still sums everything to mono. Use headphones or the line out to hear the stereo.

### Safety rules

- Install only through the te-boot menu (below).
- Don't rename the package so that its name contains `FRC`. That tells te-boot to force a bootloader rewrite.
- Never put a file with `.KEY` in its name on the OP-1's disk. It triggers a permanent change to the unit's security key.
- Don't use te-boot's "7. Factory reset" or "8. Format drive". They erase the display's calibration files along with everything else.
- Keep your stock `op1_246.op1`. You need it to go back to stock.

## Installing

1. On the co-op website, open the **Build firmware** tab. Give it your stock `op1_246.op1` and download the coop `.op1` it builds.
2. Switch the OP-1 off. Hold **COM** and switch it on. The te-boot menu appears, with TE's logo.
3. Choose **1. Upload firmware**. The OP-1 appears on your computer as a USB drive.
4. Copy the coop `.op1` file onto that drive, then eject it. te-boot checks the file, writes the firmware and reports when it's done. Follow the screen.
5. Switch the OP-1 on normally. The coop boot picture shows for about two seconds. The first boot pauses while the tape image is rebuilt.

If the OP-1 misbehaves after a flash, switch it off, hold COM at power-on and flash the previous file or stock. te-boot starts before any coop code runs, so a broken firmware can't block it.

## What's different from stock

### Second effect slot (FX 2)

Every effect position has two slots: synth, drum and master. FX 2 runs after FX 1.

| Action | Where | What it does |
|---|---|---|
| Drop | Effect page | Focuses FX 2 and shows its page |
| Lift | Effect page | Focuses FX 1 |
| T3 | Synth or drum effect page | Switches the focused slot off and on |
| Shift + T3 | Synth, drum or master | Opens the effect list with two columns: blue for FX 1, green for FX 2 |

- In the list, turn blue to choose FX 1 as stock does. Turn green to choose FX 2, then press T3 to close the list and load it. Closing with any other key discards the green choice.
- A sound you haven't touched has no FX 2, so it sounds as stock. The first drop fills FX 2 with FX 1's type and settings, switched off. Press T3 to switch it on.
- There is no "none" in the FX 2 column. To silence FX 2, switch it off.
- FX 2's page looks exactly like the stock page for its type. The big "FX OFF" shows the state of the slot in focus.
- coop remembers which slot you last focused at each position.
- On the mixer, the master effect page works the same way.
- On the effect page, lift and drop no longer copy and paste the sound. They still do on other pages.

### Second LFO (LFO 2)

LFO 2 works like FX 2, on the LFO page (T4).

- Drop focuses LFO 2 and lift focuses LFO 1. T4 switches the focused LFO off and on.
- Shift + T4 opens the LFO list. Its green column sets LFO 2's type: tremolo, value or random.
- LFO 2 reaches the same destinations as LFO 1. The two add together.
- LFO 2 is saved with the sound.

### Randomise

- Hold synth (or drum) and press right: the page you're on gets random values. On the synth that's T1 (engine), T2 (envelope), T3 (effect) or T4 (LFO). On the drum it's T2, T3 or T4. The drum's T1 page holds sample data, so randomise leaves it alone.
- Hold synth (or drum) and press left to step back through the last five randomisations. Manual edits aren't recorded.
- With FX 2 or LFO 2 in focus, randomise changes that slot. It does nothing while the slot is off.
- A plain tap of synth or drum still returns to T1, but now acts when you release the key.

### Pitch bend amount and time

Bend is still shift + left / right. Hold shift on the synth's T2 page to see two extra rows:

| Encoder | Setting | Range |
|---|---|---|
| White | Bend amount | 0 to 12 semitones, the same up and down |
| Orange | Bend time | OFF, 1-127, MAX (up to about 2 seconds) |

Both are saved with the sound.

### Drum per-key pan

On the drum's T2 page, hold shift to see coop's PAN page. Blue pans the last key you played, from L50 through C to R50. Each key keeps its own pan, saved with the kit.

### Overwrite save

Hold synth (or drum) and hold a sound key (S1-S8) through the save clock. The screen says `OVERWRITING` and the sound replaces its own saved preset instead of making a new one. A factory preset, or a sound that was never saved, falls back to a normal save.

### Rename a preset

In the preset browser (shift + a sound key), on one of your own synth presets:

1. Press orange to start naming.
2. Turn blue for letters, green for digits, white for symbols. Press the encoder to keep a character. A press with nothing turned adds a space.
3. Turn orange anticlockwise to delete.
4. Press orange to save. Any other key cancels.

Names are up to 13 characters, lower case. Factory presets and sampler presets can't be renamed.

### Tape markers

On the tape page, each encoder colour has a marker, drawn as a small flag above the tape.

- Shift + encoder press places that colour's marker at the playhead. Press again with the playhead on it to remove it. If the marker is elsewhere, it moves to the playhead.
- A plain encoder press jumps to the marker.
- Markers are saved with the tape and cleared when you erase it.

### Solo

- Shift + T1-T4 mutes a track, as stock.
- Shift + Help + T1-T4 solos a track. Hold several T keys to solo several. It works on the tape and mixer pages. Repeat the combo to restore the mutes as they were.
- You hear your live playing through the current track. If you solo a different track, your playing goes silent until the solo is off. Stock mute does the same.

### Help as fn

A tap of Help opens help as before. Held with another control, Help is the fn modifier for plugins. With shift held, Help never opens anything.

## Stereo everywhere

coop's rule is that nothing between the voices and the output collapses to mono.

### Synths and drums

- **Synth voices**: each note lands at a random place in the stereo field. The settings menu's **spread** (0-100) sets how wide. 100 is the default and 0 puts every note in the centre.
- **Drums**: each key has its own pan (see "Drum per-key pan"). Spread doesn't apply to drums.

### Effects

- Each instrument runs as a stereo pair from the voices on. Both effect slots get the full stereo signal, so a filter on the synth filters everything, including the panned part.
- At the synth and drum positions, a stock effect runs as the stereo version the stock master uses: a separate copy for the left and the right. One exception: the stock delay mixes its input to mono inside, so its echoes sit in the centre wherever it runs.
- A plugin effect is stereo in and stereo out at every position. There is no mono position in coop.
- Many ported plugins started as mono effects. coop runs them **dual-mono**: two independent copies, one per channel, so each side keeps its own placement. tape_echo, for example, has one tape loop per channel.
- Effects written for stereo (ensemble's cross-fed voices, the reverbs' wide tails) run their stereo processing at every position. On hundekraft they did this only on the master.
- The reverbs (cs_hall, cs_plate) feed their tank from the sum of left and right. That's the reverb's design: the dry signal stays fully stereo and the tail is wide.

### Master

The master has FX 1 and FX 2, both stereo, before the stock master level stage.

### Tape

- Four stereo tracks. Recording keeps each voice's placement.
- The tape page monitors in stereo.
- Lift, drop and erase keep both channels.
- In disk mode, `tape/track_1.aif` to `track_4.aif` are stereo files you can open in a DAW.

## The settings menu

Hold **synth + drum + tape + mixer** together to open the menu. These keys used to start the lander game.

| Control | Action |
|---|---|
| Blue turn | Move between items |
| Right arrow or blue press | Enter a submenu |
| Left arrow | Go back one level. At the top level, close the menu |
| Any mode key | Close the menu and act as that key |
| Green turn | Change the selected value (shown in green) |

The menu has three submenus. Most rows are placeholders for features that aren't built yet:

| Submenu | Row | What it does in e10 |
|---|---|---|
| voices | spread | Live. 0-100, the stereo width of synth notes |
| voices | drum adsr, master eq, master comp | Show "stock". Can't be changed yet |
| ui | tape position | Shows "bottom". Can't be changed yet |
| ui | date, time | Show "not yet" |
| plugins | one row per plugin file | ON or OFF |

- On a plugin row, turn green right for ON and left for OFF, or press the encoder to toggle. The change applies at the next power cycle. A plugin that is off disappears from the engine or effect list.
- If there are no plugin files, the plugins page says "no plugin files".
- Settings are saved when the menu closes and kept across power cycles.

## Using plugins

coop loads two kinds of plugin, each a `.coop` file:

- **Effects** appear in the effect list (shift + T3) at every position, FX 1 and FX 2.
- **Synth engines** appear in the stock engine list (shift + T1).

The list shows a plugin's name with `_` as a space, so `tape_echo` appears as "tape echo".

### The standard controls

Every coop plugin uses the same layout, so your hands carry over from one to the next. Each encoder has eight actions, and **the colour always follows the encoder**: blue is blue on every layer. fn means Help held.

| Action | What goes there |
|---|---|
| Turn | The four main controls. The plugin is complete with these alone |
| Shift + turn | Fine adjustment of the turn, 1/16 of a step, as stock |
| Click | Switches, steps and latches (for example freeze or hold) |
| Shift + click | Second-row switches |
| fn + turn | A second control on that encoder, such as a model or an input gain |
| fn + click | A switch of the same kind |
| Shift + fn + turn | Usually the fine adjustment of that encoder's fn + turn control. Otherwise a set-once control |
| Shift + fn + click | Rare commands |

The rules behind it:

- **Clicks act on release**, and only if shift and fn are still as they were when you pressed. A press that becomes part of a combination never fires a click.
- **A layer with nothing on it does nothing.** On a plugin effect with no extra controls, fn + turn is a plain turn.
- **coop shows the change.** When you move an extra control, its name and value appear top left in the encoder's colour for about a second.
- **Steps controls** move one step per detent, or every few detents for a long list. Some wrap round from the last step to the first.
- **Latches** (freeze, hold) are momentary and not saved. They switch off whenever the effect restarts at that position. That includes switching between synth and drum, because the stock restarts the instrument effect.
- **Saved controls** are stored with the sound.
- **Randomise** includes a plugin's saved controls, within their steps. Latches are never randomised. Controls marked as fixed, such as input gain, never jump.
- Common meanings: mix is on orange, a freeze or hold latch is usually a click, and time, rate or size is usually on blue.
- **Input gain**: every ported effect has an input gain on fn + blue, -6 to +6 dB in 0.5 dB steps, with shift + fn + blue moving it in 0.1 dB steps.

These layers work on a plugin's own page only. Stock engines and stock effects behave as they always have.

### Installing plugins

Install plugins from co-op's **Plugins** tab over USB.

1. Plug the OP-1 in by USB normally, not in disk mode. co-op talks to it over MIDI.
2. In the Plugins tab, use **Install on OP-1** on a listed coop plugin, or **Install a .coop file…** under "On your OP-1" for a file on your computer.
3. Power-cycle the OP-1. Every change applies at the next power cycle.

The "On your OP-1" panel lists each plugin file with its kind, its version, what the last boot did with it (loaded, failed to load, switched off) and whether it will be on at the next boot. From there you can switch a plugin on or off or remove it, as with the settings menu.

Limits:

- Up to 32 plugin files load.
- All plugins together may use 1,536 KiB of memory. A file over the limit is refused at boot. co-op shows it as "failed to load", with the reason.
- If a plugin is missing or switched off, sounds that use it keep their values. A synth sound plays as iter and an effect as the filter until the plugin is back.
- The plugin files on co-op are built for e10. tape_echo 0.3 needs e10 and is refused by older builds.

### The plugin list

| Plugin | Kind | Turns (blue / green / white / orange) | Extra controls |
|---|---|---|---|
| bitcrusher 0.2 | Effect | rate / bits / jitter / mix | Input gain |
| drive 0.2 | Effect | drive / tone / bias / mix | Input gain |
| wavefolder 0.2 | Effect | fold / symmetry / tone / mix | Input gain |
| tape_drive 0.5 | Effect | saturate / wobble / age / mix | Input gain |
| ensemble 0.4 | Effect | rate / depth / voices / mix | Input gain |
| freq_shift 0.2 | Effect | shift / feedback / spread / mix | Input gain. Green click: shift or ring mode |
| tape_echo 0.3 | Effect | time / feedback / age / mix | Input gain. fn + green: sync to the project tempo (off, 1/16T to 1/2). Green click: hold (latch). White click: which heads play |
| cs_hall 0.2 | Effect | size / decay / damp / mix | fn + blue / green / white: predelay, early, width (each with fine on shift + fn). Orange click: freeze (latch) |
| cs_plate 0.2 | Effect | decay / damp / pre-delay / mix | fn + blue: size (8 steps). fn + green / white: diffusion, motion (each with fine on shift + fn). Orange click: freeze (latch) |
| stereo_delay 0.1 | Effect | Four knobs | A stereo feedback delay |
| rings 0.5 | Synth engine | structure / brightness / damping / position | fn + blue: model (six). fn + green: polyphony (1, 2, 4, all). Both apply from the next note |
| saw 0.2 | Synth engine | brightness / detune / sub / drift | fn + blue: shape (saw, square, triangle). Shift + fn + orange click: sub octave (-1 or -2) |

The nine ported effects (bitcrusher to cs_plate) have their own animated screens. Each plugin's full control table is in its README in the firmware's `plugins` folder.

## Getting back to stock

te-boot is never touched by coop, so the way back always works:

1. Switch off. Hold **COM** and switch on.
2. Choose **1. Upload firmware**.
3. Copy your stock `op1_246.op1` onto the drive and eject it.

This is the normal stock update. On a unit with te-boot 2.30 and the stock file name, te-boot skips the bootloader write. The te-boot screen shows the version.

On its first boot, stock rebuilds its own tape image, which takes a few minutes. Your stereo tape recordings are lost, so back them up first. Nothing else is erased, and you don't need a factory reset.

## Status and known limits

coop is tested in an OP-1 emulator before every release, and then on the developer's own OP-1. Not everything in e10 has been through the second step yet.

### Confirmed on a real OP-1

- Stereo synths and drums with spread.
- Stereo tape: recording keeps each voice's placement, the tape page monitors in stereo, and lift, drop and erase keep both channels.
- Installing plugins over USB.
- FX 2 and LFO 2 pages.
- The two-column effect and LFO lists.
- The settings menu, including switching plugins off and on.
- Randomise, markers and solo, in an earlier build.
- Engine controls with the saw engine.
- Rings playing on the device.

### Built, but not yet confirmed on a real OP-1

The following work in the emulator. Report anything odd.

- The extra controls on effects: fn layers, fine steps, clicks and latches.
- The nine effect ports and their screens.
- The 32-file limit and the memory budget.
- The e10 boot picture.
- tape_echo following the project tempo.
- Rings 0.5 with polyphony "all".
- Corrections to bend, overwrite save, rename and drum pan made after the first device run. In particular, the semitone scale of bend amount still needs checking by ear: +12 should be an octave.

### Known limits

- The speaker plays mono. Stereo is on headphones and the line out.
- There is no album on the tape.
- Loops around a stereo take haven't been checked on the device yet.
- The stock delay's echoes are centred wherever it runs. For echoes that follow the stereo placement, use tape_echo or stereo_delay.
- Several settings rows are placeholders: drum ADSR, master EQ, master compression, tape position, date and time.
- Tape undo and redo are not built.
- Randomise doesn't work on the master.
- The extra plugin layers have no MIDI CC or velocity control yet.
- Rings is heavy. Its busiest models can go over the audio budget at their peaks.
- Overwrite save doesn't re-copy a sampler preset's audio.
