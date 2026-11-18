# v7dvs

Turn a **Numark V7** into a DVS turntable, on Linux and macOS, without the
vendor driver.

The V7 is a motorised DJ deck that speaks its own USB protocol: no class
compliance, no MIDI port. v7dvs reads its platter and synthesizes a **Serato
control-vinyl timecode signal** from the real rotation, then hands your DJ
software two ordinary things:

1. **audio carrying timecode**, so the software sees a turntable;
2. **a plain MIDI port** with the V7's buttons and faders, to map as you like.

Any software that decodes Serato timecode works: Mixxx, VirtualDJ, xwax, and
Serato itself if you own an interface it accepts.

## Three ways to use it

| Mode | What you need | Works on |
|---|---|---|
| **External timecode**: the V7's RCA outputs carry timecode into your mixer or DVS interface | a mixer or interface with line inputs | Linux, macOS |
| **Internal timecode**: timecode reaches the software through a loopback device, music leaves by the V7's RCAs | nothing but the V7 and the computer | Linux |
| **VirtualDJ native**: no timecode; VirtualDJ recognises the V7 and drives it itself | VirtualDJ | Linux (under wine), macOS |

Guide: [External timecode on a Mac](docs/external-timecode.md).

## Install

Downloads are on the [releases page](https://github.com/Hyperwindmill/v7dvs/releases).

**macOS** (Big Sur or later): unzip `v7dvs-<version>-macos.zip`, drag
`v7dvs.app` to Applications, then right-click → Open the first time. The
build is not notarized, so macOS asks once. Built for Intel; Apple Silicon
runs it under Rosetta. Details in
[packaging/macos/README-macos.md](packaging/macos/README-macos.md).

**Linux**: install the `.deb` (Debian, Ubuntu), `.rpm` (Fedora) or
`.pkg.tar.zst` (Arch) with your package manager. It brings the udev rule and
the loopback setup, and puts **v7dvs** in the application menu.

## Build from source

On Debian or Ubuntu:

    sudo apt install libjack-jackd2-dev libasound2-dev libusb-1.0-0-dev python3-usb1 python3-numpy python3-tk
    make
    ./launcher.py --configure

Without the package, libusb needs a udev rule to claim the device; it is in
`usb/99-numark-v7.rules`.

    make pkg            # the three Linux packages, in dist/
    make app            # the Mac app, on a Mac only
    make tcdec check    # self-tests, no hardware needed

The self-tests decode the timecode with xwax's own decoder, so they want an
[xwax](https://xwax.org/) checkout at `../xwax`. Nothing else does.

## Status

A test release. The Linux side and the Mac's external timecode mode are
measured on real hardware; Apple Silicon and several DJ applications are
not yet. If you try it, a report is welcome either way: open an
[issue](https://github.com/Hyperwindmill/v7dvs/issues) with your system, your
DJ software and what you saw.

This repository is the distribution copy: source, packaging and releases.

## License

GPLv3. The timecode tables derive from [xwax](https://xwax.org/).

## Not affiliated

v7dvs is an independent project. It is not affiliated with, endorsed by or
supported by Numark, Serato, VirtualDJ, Mixxx, Algoriddim (djay), Apple or any
other company or product named here. Those names are trademarks of their
owners and are used only to say what this software works with. "Serato
timecode" means a signal in the format of Serato's control vinyl, synthesized
by this project from the format parameters in xwax; no Serato software or
recording is included.
