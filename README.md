# Bluetooth Manager — Ultrahand overlay

An overlay for the Nintendo Switch (1st generation) that runs inside **Ultrahand Overlay** and lets you
pair and manage Bluetooth devices without leaving your game.

## Features

| Screen | What it does |
|---|---|
| **Main menu** | Bluetooth radio status and the number of paired / connected devices |
| **Pair → Controller** | Enables pairing mode (SYNC). Newly paired controllers show up in real time |
| **Pair → Headphones / speakers** | Searches for audio devices, lists them as they appear, and pairs/connects with **A** |
| **Manage → Paired devices** | Lists controllers and audio devices, with *Connected* status refreshed every second |
| **Device details** | Type, address, VID:PID, status · **Connect** (audio) · **Disconnect** · **Forget** (two-press confirmation) |
| **Forget all controllers** | Clears the controller pairing database (two-press confirmation) |

Leaving a screen always cancels pairing mode / device search.

## Requirements

- Atmosphère + **Ultrahand Overlay** (Ultrahand itself requires HOS 16.0.0+)
- Firmware **13.0.0+** (needed for the Bluetooth audio APIs and `GetDeviceInfo` used by this overlay)

## System limitations (not overlay limitations)

- The Switch natively supports only **controllers** (Joy-Con, Pro Controller, licensed controllers)
  and **audio** (headphones/speakers). Keyboards, mice and phones are not supported by the firmware.
- Third-party controllers (PS4/PS5/Xbox…) only pair with the
  [MissionControl](https://github.com/ndeadly/MissionControl) sysmodule installed; after that they show up here normally.
- While Bluetooth audio is connected, only 2 wireless controllers can be used and local wireless play is unavailable.
- Bluetooth microphones are not supported.

## Building

### Option A — GitHub Actions (nothing to install)
1. Create a GitHub repository and upload this folder (including the hidden `.github` folder).
2. The `.github/workflows/build.yml` workflow builds the overlay automatically using the official
   `devkitpro/devkita64` Docker image.
3. Open the **Actions** tab, pick the latest successful run and download the **ovl-bluetooth** artifact.

### Option B — Locally
1. Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `switch-dev`.
2. Install libultrahand's dependencies:
   ```sh
   (dkp-)pacman -S switch-curl switch-zlib switch-mbedtls
   ```
   (`switch-minizip` is optional; it is only linked if present.)
3. From the project root:
   ```sh
   make
   ```
The output is `ovl-bluetooth.ovl` (also copied to `out/switch/.overlays/`).

> `libultrahand` is bundled in `lib/libultrahand`. To update it, replace that folder with the latest
> version from https://github.com/ppkantorski/libultrahand.

## Installing

Copy `ovl-bluetooth.ovl` to `sd:/switch/.overlays/` and open Ultrahand (default **ZL + ZR + ↓**).
The overlay is listed as **Bluetooth Manager**.

## Project layout

```
source/
  main.cpp          Screens (MainGui, PairGamepadGui, PairAudioGui, DevicesGui, DeviceGui)
  bt_manager.hpp    Bluetooth layer API
  bt_manager.cpp    Implementation on top of libnx's btm / btm:sys
lib/libultrahand/   UI library (libtesla fork)
```

### Services used

| Action | libnx function |
|---|---|
| List paired devices | `btmGetDeviceInfo` + `btmGetDeviceCondition` + `btmsysGetConnectedAudioDevices` |
| Pair controller | `btmsysStartGamepadPairing` / `btmsysCancelGamepadPairing` |
| Search for audio | `btmsysStartAudioDeviceDiscovery` / `btmsysGetDiscoveredAudioDevice` |
| Connect audio | `btmsysConnectAudioDevice` |
| Disconnect | `btmsysDisconnectAudioDevice` (audio) · `btmHidDisconnect` (controller) |
| Forget | `btmsysRemoveAudioDevicePairing` (audio) · `btmRemoveDeviceInfo` (controller) |
| Forget all controllers | `btmsysClearGamepadPairingDatabase` |

These requests use fixed-size IPC pointer buffers, so the overlay never asks for more entries than the
service allows (10 device infos, 8 device conditions, 8 audio devices). Exceeding those limits makes
`btm` close the session (`2001-0123`); if that ever happens, the overlay reopens the session automatically.

Errors are shown in the system's `2XXX-YYYY` format.

## Customisation

- Custom theme / wallpaper: `sd:/config/ovl-bluetooth/theme.ini` and `wallpaper.rgba`
- Translations: `sd:/config/ovl-bluetooth/lang/<code>.json` (keys are the English UI strings)

## License

GPLv2 (because it uses libultrahand).
