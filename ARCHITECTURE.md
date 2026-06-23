# Architecture

Two components, one versioned API between them.

```
Wii U client (wut + SDL2)            Relay (Rust, axum + tokio)
  pairing / browse / controls  ─────▶  /v1 REST  ┐
  ADPCM audio  ◀─────────────────────  /v1/stream ┘
                                         │
                                         ▼  ARL (encrypted at rest)
                                       Deezer: gw-light + media API
                                       resolve → download → Blowfish
                                       decrypt → decode → re-encode (ADPCM)
```

## Why a relay

The Wii U can't reasonably do the Deezer streaming path itself (HTTP, TLS, the
Blowfish stripe-decrypt, MP3/FLAC decoding) and shouldn't hold your credentials.
The relay does all of that and hands the console a tiny, already-decoded ADPCM
stream. The console only plays bytes and draws the UI — no credentials, no DRM.

## Relay (`relay/`)

| Module | Job |
|--------|-----|
| `api/` | axum routers: pairing, browse/search, playback/queue, the audio stream, admin |
| `deezer/` | ARL login (`client`), key derivation + Blowfish decrypt (`crypto`), MP3/FLAC decode (`decode`), `AudioSource` (`source`), browse mapping (`proxy`) |
| `audio/` | `AudioSource` + `StreamEncoder` traits; PCM and IMA-ADPCM encoders |
| `session/` | one player session per user, idle GC, max-concurrency cap |
| `store/` | allowlist, invite codes, encrypted token (ARL) store, relay sessions |
| `auth/pairing` | device-code pairing state |

Audio path: Deezer track → decrypted MP3/FLAC → decoded to 44.1k f32 → IMA ADPCM
decimated to 22050 Hz stereo (~22 KB/s) → HTTP chunked to the console. ADPCM
keeps bandwidth tiny, which matters on the Wii U's Wi-Fi. The `StreamEncoder`
trait leaves room for other formats without touching the client.

## Client (`client/`)

```
core/      relay API client (libcurl + cJSON), models, session store
audio/     IAudioBackend (SDL2), streaming player, IMA ADPCM decoder
ui/        pairing screen, browser/player, on-screen keyboard, text, credits
platform/  wut init, SD, network (nn::ac), input, SDL video
```

No business logic in `ui/`; network and track fetches run on worker threads so
the 60 fps loop never blocks. Bundled assets (font, CA bundle, icon) are embedded
in the binary — the `.wuhb` content mount isn't reachable via `fopen` on the Wii U.

## The seam (`proto/`)

The client↔relay API is an OpenAPI spec under `/v1`. The client only ever reads
its relay URL from config, so the exact same build works against the central
relay or a self-hosted one — only the URL and onboarding differ
(`RELAY_MODE = central | self-hosted`). The relay is a deployment choice, not a
hard dependency.

## Data flow: first launch → playing

1. Console has no saved session → shows a device code on the TV.
2. User opens the relay's `/v1/pair` on a phone, enters the code + Deezer ARL
   (+ invite, in central mode).
3. Relay validates the ARL, stores it encrypted, issues an opaque relay session
   token to the console.
4. Console browses (relay proxies Deezer) and plays: `play_uri` makes the relay
   fetch + decrypt + decode the track and stream it as ADPCM.
