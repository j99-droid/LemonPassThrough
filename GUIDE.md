# Lemon Pass Through guide

This guide takes you from nothing to racing with your real wheel visible in the headset.

## What you need

- A Linux PC with Ubuntu 24.04, Pop!_OS 24.04 or newer (other distributions work if you install the build dependencies yourself).
- A recent NVIDIA, AMD or Intel GPU with Vulkan video encoding, NVENC or VA-API.
- A Meta Quest headset with passthrough. Tested on the Quest 3; other Quest headsets should work but haven't been tested.
- Your PC and headset on the same network. A 5 GHz or 6 GHz Wi-Fi router close to you works best; the PC itself is best wired.
- Developer mode on the headset, so you can install apps from your PC. See [Meta's instructions](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/).

## 1. Install

Download this project, open a terminal in its folder, and run:

```bash
./install.sh
```

The installer:

1. Installs the build dependencies with `apt` (asks for your password).
2. Builds the PC server and installs it to `~/.local/share/lemon-pass-through`. The first build takes 10 to 20 minutes.
3. Adds **Lemon Pass Through** to your apps menu and a `lemon-pass-through` command.
4. Installs the headset app, if the Quest is plugged in with USB and you've allowed USB debugging in the headset.

If the headset wasn't plugged in, install the app later with:

```bash
./install.sh --headset-only
```

Run `./install.sh --help` for the other options.

> **Already have WiVRn?** The Lemon Pass Through headset app is a separate app, so it installs alongside WiVRn without touching it. Use the Lemon Pass Through app with the Lemon Pass Through server; WiVRn's app may not match its version.

## 2. Set up your game in Steam

In Steam, right-click the game, open **Properties**, and set **Launch options** to:

```
PRESSURE_VESSEL_FILESYSTEMS_RW=$XDG_RUNTIME_DIR/wivrn/comp_ipc %command%
```

This lets games running in Steam's container reach the server.

Games that only speak OpenVR (SteamVR) also need a translation layer such as [xrizer](https://github.com/Supreeeme/xrizer/). Games with native OpenXR support, such as Assetto Corsa EVO, don't.

## 3. Start a session

Order matters: **server, then headset, then game.** A game started before the headset connects won't show up in the headset.

1. On the PC, open **Lemon Pass Through** from your apps menu. A terminal window opens and shows the server's log. Leave it open; closing it stops the server.
2. In the headset, open **Lemon Pass Through** from the app library (under Unknown Sources). Choose your PC in the list and press **Connect**.
3. Start the game on the PC.

Keep the game's window visible on the PC (small is fine). See [Troubleshooting](#troubleshooting) for why.

## 4. Place the wheel window

The wheel window is a circle in the room where the game is cut away and you see the real world through the headset cameras. Set it up once and it's saved.

| Control | Action |
| --- | --- |
| **Right B** | Place a new circle below and in front of you, facing the way you face. Look at your wheel when you press it. |
| **Left thumbstick** | Move the circle left, right, up or down. |
| **Left trigger + left thumbstick up/down** | Make the circle bigger or smaller. |
| **Right thumbstick up/down** | Also makes the circle bigger or smaller. |
| **Right A** | Save. The circle comes back in the same place next time. |

The circle is anchored in the room: it stays over your wheel when you move your head. If you move your chair or rig, press **Right B** again and save.

The saved position is in `~/.config/wivrn/wheel-mask.txt`. Delete that file to start from scratch.

## Troubleshooting

**The headset says "Waiting for an application".**
The headset is connected but the game isn't sending it anything. Either the game was started before the headset connected (close the game and start it again), or the Steam launch options from step 2 are missing.

**About 1 fps in the headset.**
The game's window on the PC is minimized or hidden. Some games, Assetto Corsa EVO included, slow down to a crawl when their window is hidden, and that applies to the headset too. Keep the window visible; shrink it in the game's settings if you want it out of the way.

**The headset keeps disconnecting while plugged in with USB.**
With the cable in, the headset can connect over the cable's network link, which some PCs keep resetting. Unplug the cable and use Wi-Fi. To keep using adb without the cable, run this once with the cable plugged in, then unplug it:

```bash
adb tcpip 5555
```

Then connect with `adb connect <headset-ip>:5555`. This resets when the headset restarts.

**The PC doesn't show up in the headset's list.**
Check that both are on the same network and that the server window is open. The PC announces itself with Avahi, which must be running (`systemctl enable --now avahi-daemon`). If you use a firewall, open UDP port 5353 for Avahi and TCP and UDP port 9757 for the server, for example `sudo ufw allow 5353/udp` and `sudo ufw allow 9757`.

**The circle is in the wrong place, or not visible.**
Look at your wheel and press **Right B** to place a fresh circle, adjust it, then press **Right A** to save.

**Watching the headset view on the PC.**
Install [scrcpy](https://github.com/Genymobile/scrcpy) and run this to show the left eye on a Quest 3 (the crop may need adjusting for other headsets):

```bash
scrcpy --crop 1832:1920:0:0 --max-fps 30
```

## Uninstall

```bash
./uninstall.sh            # PC server, launcher and menu entry
./uninstall.sh --headset  # also the app on a connected headset
```

Your saved wheel position is kept.
