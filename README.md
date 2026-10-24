<p align="center"><img src="logo.png" width="160" alt="Lemon Pass Through logo"></p>

# Lemon Pass Through

Wireless PC VR for Meta Quest on Linux, with a window to the real world: a circle in the room where the game is cut away and the headset's passthrough cameras show through. Put it over your sim-racing wheel and you can see your hands, wheel and buttons while you race.

It's a fork of [WiVRn](https://github.com/WiVRn/WiVRn), so it streams any OpenXR game the same way WiVRn does.

## Features

- **Room-anchored passthrough circle.** It stays over your wheel as you move your head.
- **Set up in the headset** with the controllers. No config files or numbers to type.
- **Saved between sessions.**
- Everything else from WiVRn: wireless streaming, hardware video encoding, foveation, and controller and hand tracking.

## Quick start

```bash
git clone https://github.com/j99-droid/LemonPassThrough.git
cd LemonPassThrough
./install.sh
```

Then:

1. In Steam, set the game's launch options to `PRESSURE_VESSEL_FILESYSTEMS_RW=$XDG_RUNTIME_DIR/wivrn/comp_ipc %command%`.
2. Open **Lemon Pass Through** on the PC, then in the headset, and connect. Then start the game.
3. Look at your wheel and press **Right B**. Adjust with the **left thumbstick** (hold the **left trigger** to resize), and press **Right A** to save.

The full walkthrough, controls and troubleshooting are in **[GUIDE.md](GUIDE.md)**.

## Controls

| Control | Action |
| --- | --- |
| Right B | Place the circle in front of you |
| Left thumbstick | Move it |
| Left trigger + left thumbstick up/down | Resize it |
| Right thumbstick up/down | Resize it |
| Right A | Save |

## Requirements

- Linux PC: Ubuntu 24.04, Pop!_OS 24.04 or newer for the installer; other distributions can build from `source/` by hand.
- A Meta Quest with passthrough and developer mode on. Tested on the Quest 3.
- PC and headset on the same network.

Tested with Assetto Corsa EVO on Pop!_OS 24.04 with an NVIDIA RTX 4080 SUPER and a Quest 3.

## How it works

The server makes part of each eye's image transparent and asks the headset to blend the stream over passthrough. The circle is a real position in the room: every frame, the server projects it into each eye using that frame's head pose and field of view. The game itself is unchanged.

The changes from WiVRn are in `source/server/compositor/` (`compositor.cpp`, `foveation.h` and `shaders/foveation.comp`). The headset app is WiVRn's, renamed, with a new icon.

## Building by hand

```bash
cmake -S source -B build -G Ninja -DWIVRN_BUILD_CLIENT=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

On Ubuntu 24.04, add `-DVulkan_INCLUDE_DIR=<path to Vulkan-Headers 1.4>/include`. `install.sh` handles this for you. See `source/docs/building.md` for the headset app.

## Credits and license

Based on [WiVRn](https://github.com/WiVRn/WiVRn) by Patrick Nicolas and contributors, which builds on [Monado](https://monado.freedesktop.org/). Licensed under GPL-3.0, see [COPYING](COPYING). Changes in this fork are under the same license.

Not affiliated with WiVRn, Meta or Kunos Simulazioni.
