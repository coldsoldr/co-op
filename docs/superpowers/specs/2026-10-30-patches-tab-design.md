# Patches tab and local library

Agreed with the owner on 2026-10-30. Built on `dev`, and merged to `main` only when the owner
says so. Nothing here has run on a device yet, except what `docs/research/2026-10-29-hundekraft-usb-and-plugins.md`
records as tested.

## Goal

A library far larger than the OP-1 can hold, easy to audition and to copy from. People browse
patches on their computer or from the community, hear them, and send the ones they like to the
OP-1.

## Two tabs, two jobs

| | Patches | Library |
| --- | --- | --- |
| Purpose | Browse and audition | Organise and send to the OP-1 |
| Needs a device | No | Yes, in disk mode |
| Shows | Community and the local folder | synth, drum and local in the left column |
| OP-1 state | Normal mode, for auditioning on the device | Disk mode |

This split replaces the copy queue and the remembered drive of the mockups: a person never needs
both OP-1 modes on one screen, so there is nothing to remember and nothing to queue.

## Patches tab

Sits next to Packs. It always shows both sources, with or without an OP-1 attached.

- **Sources:** Community, and the local folder once one is chosen.
- **Browse:** search, kind (synth or drum), engine, folder.
- **Every patch can be heard somehow:** the row says where it will play.

| Patch | OP-1 connected with the plugin | Otherwise |
| --- | --- | --- |
| Synth engine, not sampler | Plays on the OP-1 | Plays its stored audio if that is a real render, else shows no preview |
| Sampler, drum | Plays in the browser, with an on-screen keyboard | The same |

Audition uses the patch's settings only, so it works from either source, and from a browser that
never saw the drive.

- **Connection:** the tab looks for the OP-1 and the plugin when it opens and on request. It never
  blocks browsing. With no plugin on a hundekraft OP-1 it offers to install it.
- **Put my sound back:** always visible while an audition is running.
- **Firmware:** hundekraft first. opus depends on what its plugin format allows, which is unknown.
  Stock firmware cannot audition: it gets browser previews only.

### Turning a knob during an audition

Open. The recommendation is that it ends the audition and keeps the sound, with a warning up
front. The OP-1's own code would save it into the slot, replacing the original.

## Local folder

- Chosen with the same browser folder access as Connect OP-1, so it needs Chrome, Edge or Vivaldi.
  Other browsers can use the Community source only.
- Remembered between visits. The browser asks to allow it again after a restart, so the page shows
  a Reconnect button, as the Library does for the drive.
- Any folder layout is accepted. co-op scans for `.aif` and `.aiff` patches and keeps the folders
  as they are.
- Read first. Writing to the folder (rename, move, delete, saving from the OP-1) comes after the
  rest works.

## Library

- The left column gains a third group, **local**, under synth and drum, with the local folder's
  folders. It is hidden until a local folder is chosen.
- Selecting a local folder shows its patches in the same list, without the actions that change the
  OP-1.
- **Send to OP-1:** tick one or many patches, press Send, and choose the target folder under synth
  or drum. Patches go to the group that matches their kind.
- Sending uses the guards installs already have: a free name when one is taken, the warning for
  patches the firmware cannot play, and the plugin prompt. Files are copied unchanged.
- It stops with a clear message when the free space estimate says the OP-1 is full.
- A dropped read-only drive stages the sends in the changes zip, like every other write.

## Community patches

The first public set needs patches with clear permission. The 81 patches in `incoming/` are for
testing only and are never committed, because their rights are unclear.

The Community source lists every patch in `packs/`, using the existing pack format, checks and
pull request route, so there is one way to submit. A pack stays the unit of submission, and the
Patches tab shows its patches as one flat list.

While the public set is small, a developer can point the local folder at `incoming/`.

## Audition protocol

The plugin grows from the proof of concept:

- It receives a whole patch and checks it arrived intact before applying it.
- Plugins never see SysEx, so data travels as channel messages. Polyphonic pressure on channel 15
  carries 14 bits a message.
- The browser reads the patch's JSON and sends its fields in a compact binary form, so the plugin
  needs no JSON text parser.
- A patch's settings are about 350 bytes, which is under 0.1 s at the measured rate.

Speed test on the owner's OP-1, Vivaldi, 2026-10-30, timed by the plugin's reply so the figures
are delivered rates: 7,909 and 15,857 messages a second as asked, and a ceiling of 18,479 a second
(31.6 KB a second) when asked for 32,000. Nothing was lost at any rate, so the USB path sets the
limit and the plugin keeps up with it. The rate depends on the host, so the transfer must still
check and resend. Its working rate is 15,000 messages a second, under the ceiling, for about
25 KB a second.

At the ceiling a typical 460 KB sample patch takes at least 15 s and a 1 MB patch 33 s, before
checking and before the OP-1 stores it. That is too slow for clicking through patches, so sampler
and drum patches stay in the browser. A deliberate "load this one on the OP-1" could be added
later, but it also needs a way to store the audio, which is untested.

## Trying a plugin

Plugins load only when the OP-1 starts, so trying one always takes a restart.

- Each plugin gets a demo video with its real sound.
- **Try on my OP-1** installs it, waits for the restart, offers demo patches through audition, then
  asks Keep or Remove.
- To be tested on the device: removing a plugin that the current slot uses.

## Order of work

Each step ends on the dev site for the owner to try.

1. **Local folder:** choose, remember, scan, and show it on a basic Patches tab.
2. **Browse and hear:** Community source, filters, browser preview, voice clip shown as no preview,
   on-screen keyboard.
3. **Library:** the local group, multi-select and Send to OP-1.
4. **Audition:** the patch transfer in the plugin, connection handling, audition in the Patches
   tab, plugin in the library.
5. **Trying plugins:** demo videos and Try on my OP-1.

## Where it stands

Steps 1 to 5 are built on `dev` and tested against stand-ins, not yet on a device. Differences from
the plan above:

- **Turning a knob** follows the recommendation, and the Patches tab warns about it.
- **New folders** when sending are made in the Library's left column, not in the send dialog.
- **The audition plugin** is not in the plugin library yet, because its source is not published.
  Until then it is installed by hand through the hundekraft workshop.
- **Demo videos** are an optional `video` link in `plugin.json`. No plugin has one yet.
- **Try on my OP-1** offers Keep or Remove and points to the Patches tab. It does not bring its own
  demo patches.

## Parked

Rendering real notes into patch files, or recording previews through a line input. The OP-1
replaced a slot's render with its voice clip when co-op copied the file into `snapshot`, so
renders do not survive in preset files.
