# client — DiizerU Wii U homebrew

devkitPro + wut, C++17, SDL2, packaged as a `.wuhb` for Aroma. Talks to the relay
over HTTP (libcurl), plays the relay's ADPCM audio stream, renders the UI.

## Build

```sh
sudo dkp-pacman -S --needed wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-sdl2_mixer
make            # -> DiizerU.wuhb  (needs DEVKITPRO + DEVKITPPC in the env)
```

Copy `DiizerU.wuhb` to `sd:/wiiu/apps/`. Point it at your relay via
`sd:/diizeru/relay.cfg` (one line, e.g. `https://your-domain.example/v1`) or by
editing `kDefaultRelayUrl` in `src/main.cpp` before building.

## Layout

```
src/
  core/      relay API client (libcurl + cJSON), models, session store
  audio/     IAudioBackend (SDL2), streaming player, IMA ADPCM decoder
  ui/        pairing screen, browser/player, on-screen keyboard, text, credits
  platform/  wut init, SD, network (nn::ac), input, SDL video
third_party/ cJSON
data/        bundled into the binary: font (Roboto), CA bundle
```

No business logic in `ui/`; network/track fetches run on worker threads so the
60fps loop never blocks. Assets are embedded in the RPX (the `.wuhb` content
mount isn't reachable via fopen on the Wii U).

## Controls

Sticks/D-pad move, A open/play, B back. Y play/pause, L/R skip, D-pad ←/→ seek,
minus repeat, plus credits, X re-link. Touchscreen works too (keys, rows, scrub
bar, play/pause).
