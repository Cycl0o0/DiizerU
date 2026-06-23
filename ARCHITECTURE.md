# Architecture

One component: the Wii U client does everything on-device.

```
Wii U client (wut + SDL2)
  reads sd:/diizeru/arl.txt → logs into Deezer (gw-light + media API)
  browse / search / controls
  resolve track → download (HTTPS) → Blowfish stripe-decrypt → MP3 decode → play
```

## Why on-device

The Wii U is perfectly capable of the Deezer streaming path itself — HTTPS
(libcurl + mbedTLS), the Blowfish stripe-decrypt, and MP3 decode (minimp3) — so
there's no reason to hand your credentials to anyone else. The ARL stays on the
SD card and nothing leaves the console except the requests to Deezer.

## Client (`client/`)

```
core/      Deezer client (ARL login, browse, track resolve), models, session store
audio/     IAudioBackend (SDL2), streaming player, Blowfish stripe-decrypt, MP3 decode
ui/        browser/player, on-screen keyboard, text, credits
platform/  wut init, SD, network (nn::ac), input, SDL video
```

No business logic in `ui/`; network and track fetches run on worker threads so
the 60 fps loop never blocks. Bundled assets (font, CA bundle, icon) are embedded
in the binary — the `.wuhb` content mount isn't reachable via `fopen` on the Wii U.

## Audio path

Deezer serves the track as MP3 (preferring MP3_128 for the Wii U's bandwidth)
encrypted with `BF_CBC_STRIPE` — every third 2048-byte block is Blowfish-CBC
encrypted under a key derived from the track id (MD5 + XOR). The client decrypts
the stripes (mbedTLS), feeds the MP3 to minimp3, and emits s16 PCM at 44100 Hz to
an SDL2 audio device (pull/callback model over a ring buffer; SDL converts to the
AX hardware rate). The console is big-endian PowerPC, so PCM is written as
explicit little-endian to match the SDL device format.

## Configuration

The only config is `sd:/diizeru/arl.txt` — your Deezer ARL. Generate it in the
browser at <https://diizeru.cyclooo.fr> (nothing is uploaded) or paste it in by
hand. No pairing, no accounts, no server URL.

## Data flow: launch → playing

1. Boot reads `sd:/diizeru/arl.txt` and logs into Deezer (gw-light `getUserData`
   for the license token, etc.).
2. Browse: the client calls Deezer's gw-light + public API directly and maps the
   results to its models.
3. Play: resolve the track to an encrypted CDN URL, download it, decrypt the
   stripes, MP3-decode, and stream PCM to the audio backend.
