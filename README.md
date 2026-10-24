# Lemon Pass Through

A fork of [WiVRn](https://github.com/WiVRn/WiVRn) (wireless PC VR streaming to Quest headsets, GPL-3.0) that adds a passthrough "wheel window": a rectangle in the view where the real world shows through, so you can see a sim-racing wheel while playing.

## What is different from WiVRn

- **Renamed** to Lemon Pass Through. The app label and server startup banner use the new name. Package IDs and protocol names are unchanged, so this build installs over an existing WiVRn local build and talks to the same clients.
- **New logo** (`logo.svg`, `logo.png`), used as the launcher icon.
- **Wheel mask:** the server can make a rectangle of the eye view transparent. The headset shows passthrough there. The game is unchanged; the server requests alpha blend while the mask is on.
- **In-headset editing** with the Quest controllers:
  - Left thumbstick: move the rectangle.
  - Right thumbstick: grow or shrink it (up grows, down shrinks).
  - Right A: save to `~/.config/wivrn/wheel-mask.txt`, loaded on the next start.
  - The rectangle is hidden until a stick is first moved.
- **Environment variable:** `WIVRN_WHEEL_MASK="min_x,min_y,max_x,max_y"` (eye UV, 0 to 1, top-left origin) sets the starting rectangle. It takes priority over the saved file.

## Status

Work in progress. The mask editing and alpha passthrough path were built and the controller input was confirmed reaching the server. Passthrough on the headset with a game running was still being verified when this was packaged. Treat it as experimental.

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
