# Scarlett MixControl for modern macOS

Makes Focusrite's **Scarlett MixControl** work again with **1st-gen Scarlett** interfaces on current macOS, including Apple Silicon.

On current macOS, MixControl shows "NO HARDWARE CONNECTED" even when the interface is plugged in and working. This project patches a copy of the app so it finds the device. The original app isn't modified.

| Before | After |
|---|---|
| ![Before: NO HARDWARE CONNECTED](docs/images/before.png) | ![After: mixer connected](docs/images/after.png) |

## Devices

Scarlett **18i20** (tested), **18i8**, **18i6**, **8i6**, **6i6** (1st gen, untested).

## Requirements

- Scarlett MixControl 1.10.6 installed in `/Applications`
- Rosetta 2 on Apple Silicon: `softwareupdate --install-rosetta --agree-to-license`
- Command Line Tools: `xcode-select --install`

## Install

1. Download **Scarlett MixControl 1.10.6** for Mac from [Focusrite](https://downloads.focusrite.com/focusrite/scarlett-1st-gen/scarlett-18i20-1st-gen).
2. Open the downloaded `.dmg` and drag **Scarlett MixControl** into **Applications**.
3. Run:
   ```sh
   git clone https://github.com/<your-account>/scarlett-mix-control.git
   cd scarlett-mix-control
   ./build.sh --install
   ```

## Launch

Open **Scarlett MixControl (Patched)** from Applications. The original app won't connect.

## Warnings

- Don't use MixControl's firmware update with the patched app.
- MixControl may overwrite the mixer settings stored on the device. Save a snapshot first if they matter.

## Troubleshooting

- Quit MixControl, replug the interface, and open the app again.
- Log: `~/Library/Logs/ScarlettMixControlShim.log`. For a detailed log, run: `open --env SCARLETT_SHIM_DEBUG=1 "/Applications/Scarlett MixControl (Patched).app"`

## How it works

MixControl looks for a USB audio object that current macOS no longer creates. `shim.c` redirects that lookup to the device's USB interface and supplies the matching CoreAudio ID. It affects only Focusrite devices. Details are in [`shim.c`](shim.c).

## License

[MIT](LICENSE). Provided as-is, without warranty. Not affiliated with Focusrite. Focusrite, Scarlett and MixControl are trademarks of Focusrite Audio Engineering Ltd.
