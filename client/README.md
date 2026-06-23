# /client — DiizerU Wii U homebrew (planned, M3+)

devkitPro + wut, C/C++17, packaged as `.wuhb` (wuhbtool), for Aroma.

> Not built yet. Per the build order, the GUI is implemented only after pairing
> and the audio path work end-to-end. This README pins the intended structure so
> it matches ARCHITECTURE.md.

## Stack

- GUI: SDL2 + SDL2_ttf + SDL2_image (wut ports)
- Net: libcurl (bundled `cacert.pem` on SD)
- JSON: nlohmann/json or cJSON
- Audio: `IAudioBackend` over AX/sndcore2 or SDL2_mixer — consumes the relay's
  raw PCM (no Spotify crypto/decoding on the console)

## Layout (planned)

```
src/
  core/      relay API client, models, player state machine, queue, cache
  audio/     IAudioBackend, ring buffer, PCM pull from /v1/stream
  ui/        SDL2 screens, mini-player, focus/nav, on-screen keyboard (swkbd)
  platform/  wut init, SD mount (sd:/diizeru), network, input (VPAD/KPAD)
```

Rule: no business logic in `ui/`. Network fetches run async so the 60fps loop
never blocks. Relay URL is read from `sd:/diizeru/relay.cfg` (never compiled
in) — this is what makes the central↔self-hosted seam free on the client side.

## Status

- ✅ **M3 hello build** written: `Makefile` (wut → `.wuhb`), `src/platform/`
  (SDL2 video/input init), `src/main.cpp` (dark-theme mock UI, 60fps loop,
  controller input, HOME/B to exit). Toolchain + Makefile validated — compiles
  with the wut PPC compiler; only needs the SDL2 portlibs to link.

## Build

```sh
# one-time: install the wiiu SDL2 portlibs (needs sudo)
sudo dkp-pacman -S --needed wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-sdl2_mixer

# requires DEVKITPRO + DEVKITPPC in the environment
make            # -> DiizerU.wuhb
# copy DiizerU.wuhb to sd:/wiiu/apps/ and launch via Aroma
```

## First-run pairing

The app shows a device code on the TV. Enter it at
`your-domain.example/v1/pair` on your phone and sign in with Spotify. The
session token is persisted to `sd:/diizeru/session.json`.
