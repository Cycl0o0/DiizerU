# client — DiizerU Wii U homebrew

devkitPro + wut, C++17, SDL2, packaged as a `.wuhb` for Aroma. Logs into Deezer
directly, decrypts and MP3-decodes on-device, and plays the result through SDL2.

## Build

```sh
sudo dkp-pacman -S --needed wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-sdl2_mixer
make            # -> DiizerU.wuhb  (needs DEVKITPRO + DEVKITPPC in the env)
```

Copy `DiizerU.wuhb` to `sd:/wiiu/apps/` and put your Deezer ARL in
`sd:/diizeru/arl.txt` (generate it in the browser at https://diizeru.cyclooo.fr,
nothing is uploaded). No pairing, no server.

## Layout

```
src/
  core/      Deezer client (ARL login, browse, track resolve), models, session store
  audio/     IAudioBackend (SDL2), streaming player, Blowfish stripe-decrypt, MP3 decode
  ui/        browser/player, on-screen keyboard, text, credits
  platform/  wut init, SD, network (nn::ac), input, SDL video
third_party/ cJSON, minimp3
data/        bundled into the binary: font (Roboto), CA bundle
```

No business logic in `ui/`; network/track fetches run on worker threads so the
60fps loop never blocks. Assets are embedded in the RPX (the `.wuhb` content
mount isn't reachable via fopen on the Wii U).

## Controls

Sticks/D-pad move, A open/play, B back. Y play/pause, L/R skip, D-pad ←/→ seek,
minus repeat, plus credits. Touchscreen works too (keys, rows, scrub bar,
play/pause).
