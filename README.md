# DiizerU

Deezer on the Wii U. A homebrew app that logs into Deezer straight from the
console, decrypts and decodes everything on-device, and plays your own Deezer
Premium library to the TV — search, playlists, the usual transport controls,
album art, all driven from the GamePad (sticks/buttons or the touchscreen, your
call).

It started as a "can I even do this" weekend thing and grew. It works on real
hardware. It's also a grey-zone hobby project, so please read the
[fine print](#the-fine-print) before diving in.

## State of things

Working right now:

- Liked songs, your playlists (private ones too), albums
- Search with an on-screen keyboard — D-pad or just tap the touchscreen
- Play/pause that actually resumes where it left off, next/prev, seek, repeat (off/all/one)
- Now-playing with album art and a progress bar
- Everything runs on the console: login, Blowfish stripe-decrypt and MP3 decode
  all happen on the Wii U. No server in the loop — your token never leaves the
  SD card.

It's young and a hobby project. Rough edges exist.

## What you need

- A Wii U with Aroma
- A Deezer Premium account

## Setup

1. Download `DiizerU.wuhb` from the [Releases](../../releases) page and drop it
   in `sd:/wiiu/apps/`.
2. Get your Deezer ARL into `sd:/diizeru/arl.txt`. Easiest way: open
   <https://diizeru.cyclooo.fr>, paste your ARL, and download the generated
   `arl.txt` — it's built entirely in your browser, nothing is uploaded. Copy it
   to `sd:/diizeru/` on the SD card. The page also walks you through finding the
   ARL (it's a cookie in your browser).
3. Launch it and go listen to music.

Your ARL is the session token from your Deezer cookies — it grants access to your
account, so treat it like a password. It only ever lives on your computer and
your SD card. More in [SECURITY.md](SECURITY.md).

## Building from source

You need devkitPro (devkitPPC + wut) and the Wii U SDL2 portlibs.

```sh
sudo dkp-pacman -S --needed wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-sdl2_mixer
cd client && make                  # -> DiizerU.wuhb
```

Copy the `.wuhb` to `sd:/wiiu/apps/` and drop your `arl.txt` in `sd:/diizeru/`.

## How it fits together

```
Wii U (wut + SDL2)
  │ reads your ARL from sd:/diizeru/arl.txt, logs into Deezer
  ▼
Deezer:  resolve track  →  download over HTTPS (libcurl)
                        →  Blowfish stripe-decrypt (mbedTLS)
                        →  MP3 decode (minimp3)
                        →  play (SDL2 → AX)
```

No relay, no middleman: the console does the whole job and holds the only copy of
your token (on the SD card). More in [ARCHITECTURE.md](ARCHITECTURE.md).

## Controls

Sticks/D-pad to move, A opens or plays, B goes back. Y is play/pause, L/R skip
tracks, D-pad left/right seeks ten seconds, minus cycles the repeat mode, plus
shows the credits. Or ignore all that and poke the touchscreen — keys, rows, the
scrub bar and play/pause all respond to taps.

## Security, briefly

Your ARL lives only on your computer and the SD card. The website's config
generator runs entirely in your browser — the token is never uploaded anywhere.
On the console it's read from `sd:/diizeru/arl.txt` and used to log in directly;
nothing is sent to any third party. Details in [SECURITY.md](SECURITY.md).

## The fine print

- It's a hobby project. No SLA, things may break.
- Your own Deezer Premium account only. No sharing accounts.
- It reaches Deezer the unofficial way and decrypts your own entitled content on
  the console, which almost certainly breaks Deezer's terms for third-party apps.
  Personal/educational use, your own account, your own risk. You're responsible
  for what you do with it.
- Not affiliated with Deezer or Nintendo. The names belong to them.

## License

[AGPL-3.0](LICENSE). If you run a modified version as a service, you owe your users
the source. The bundled Roboto font is Apache-2.0; the CA bundle comes from the
curl/Mozilla set.
