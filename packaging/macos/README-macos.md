# v7dvs on a Mac — for testers

v7dvs makes a Numark V7 look like a turntable (Serato timecode out of its
RCA outputs) plus a plain MIDI controller, to any DJ software. This is a
test build: it works, it is rough around the edges, and your report is what
makes the next one better.

## Install

1. Unzip `v7dvs-<version>-macos.zip` and drag `v7dvs.app` to Applications.
2. First launch: **right-click the app → Open → Open**. macOS says the
   developer is unidentified because this build is not notarized; this is
   expected and only asked once. On macOS 15 (Sequoia) the first launch is
   refused instead: open **System Settings → Privacy & Security**, scroll to
   the message about v7dvs and click **Open Anyway**, then launch again.
3. On an Apple Silicon Mac, macOS offers to install Rosetta the first time.
   Accept; the app is built for Intel and runs under it.

Nothing else to install. Python, drivers, the lot: inside the app.

## Use

Plug in the V7 over USB (no other software driving it), open v7dvs, and the
settings window appears. Pick what each RCA pair carries, then **Save and
start**:

- **External timecode** (`timecode:A` / `timecode:B` on the pairs): the V7's
  RCA outputs carry Serato timecode. Connect them to the line inputs of your
  own DVS interface, select those inputs as vinyl inputs in your DJ software
  and calibrate as with a turntable. Works with every DJ app that does DVS,
  Serato included if the interface is one it accepts. The V7's pitch fader is
  already inside the timecode: do not map it.
- **VirtualDJ native** (`output` or `none` on both pairs, MIDI name
  `Numark V7 MIDI`): no timecode; VirtualDJ recognises the V7 as itself and
  drives the platter, motor and LEDs. The RCAs carry nothing on macOS for
  now, so send VirtualDJ's audio to another output.

A window with a **Stop** button stays open while it runs. Close it, or press
Stop, to release the V7.

## If something is wrong

The log is `~/Library/Logs/v7dvs.log` (Finder → Go → Go to Folder, paste the
path). Send it together with:

- the macOS version and whether the Mac is Intel or Apple Silicon,
- the DJ software and its version,
- what you did and what you saw (a 10-second phone video of the deck's
  waveform or scope is worth a paragraph).

Known limits of this build: no menu-bar icon, no notarization, timecode
through a virtual audio device (no interface needed) is Linux-only so far.

v7dvs is GPLv3; source and issues: https://github.com/Hyperwindmill/v7dvs

