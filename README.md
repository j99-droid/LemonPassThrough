# Lemon Pass Through

A fork of [WiVRn](https://github.com/WiVRn/WiVRn) (wireless PC VR streaming to Quest headsets, GPL-3.0) that adds a passthrough "wheel window": a rectangle in the view where the real world shows through, so you can see a sim-racing wheel while playing.

## What is different from WiVRn

- **Renamed** to Lemon Pass Through. The app label and server startup banner use the new name. Package IDs and protocol names are unchanged, so this build installs over an existing WiVRn local build and talks to the same clients.
- **New logo** (`logo.svg`, `logo.png`), used as the launcher icon.
- **Wheel mask:** the server makes part of each eye's view transparent, and the headset shows passthrough there. The game is unchanged; the server requests alpha blend while the mask is on.
- **Room-anchored wheel box:** the mask comes from a box placed in the room, about where your wheel is. Each frame the box is projected into each eye, so it stays over the wheel as you move your head.
- **In-headset editing** with the Quest controllers:
  - Right B: place a fresh, upright 45 cm box below and in front of you, facing where you face. Press it while looking at your wheel.
  - Left thumbstick: nudge the box left, right, up or down.
  - Right thumbstick: make the box wider or narrower (left/right) and taller or shorter (up/down).
  - Right A: save to `~/.config/wivrn/wheel-mask.txt`, loaded on the next start.
- **Head-fixed fallback:** without a placed box, the mask is a fixed rectangle in each eye's view. The sticks move and resize it instead.
- **Environment variable:** `WIVRN_WHEEL_MASK="min_x,min_y,max_x,max_y"` (eye UV, 0 to 1, top-left origin) sets the head-fixed rectangle. It takes priority over the saved file's first line.

### Save file

`~/.config/wivrn/wheel-mask.txt` has one or two lines:

1. The head-fixed rectangle: `min_x,min_y,max_x,max_y`.
2. Only when a box has been placed: `x,y,z,qx,qy,qz,qw,width,height`. That's the box centre and orientation in tracking space, then its size in metres.

## Status

Work in progress. Passthrough works in the headset with Assetto Corsa EVO running. Room anchoring was reworked to use the correct flat-image projection and to clip the box at the eye instead of hiding it. That rework is still being tested in the headset. Treat it as experimental.

If the headset says "waiting for an application", check that `/run/user/$UID/wivrn/comp_ipc` exists. If it doesn't, restart the server, reconnect the headset, then start the game.

## Install the client (Quest)

Sideload `LemonPassThrough.apk` with developer mode and USB debugging on:

```bash
adb install -r LemonPassThrough.apk
```

## Build the server (PC, Linux)

See `docs/building.md` in `source/`. The server build used here:

```bash
cmake -B build-server . -GNinja -DWIVRN_BUILD_CLIENT=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-server
```

Run it with the mask set:

```bash
WIVRN_WHEEL_MASK="0.25,0.55,0.75,1.0" build-server/server/wivrn-server
```

## Client signing

The APK here is signed with a local key. Create your own keystore and `gradle.properties` as described in `docs/building.md` before building the client yourself. Neither file is included in this folder.

## Credits and license

Based on WiVRn by Patrick Nicolas and contributors. Licensed under GPL-3.0, see `COPYING`. Changes in this fork are under the same license.
