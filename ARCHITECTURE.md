> **Note (beta):** DiizerU now streams from **Deezer** (per-user ARL token, decrypted server-side). Some sections below describe the original Spotify/librespot design and are kept for historical context; the security model (encrypted-at-rest token, never logged, allowlist, kill switch) applies to the Deezer ARL the same way.

# DiizerU — Architecture

> English first. Résumé français en fin de chaque section (*FR*).

## 1. Overview

DiizerU is a two-component system that lets a Wii U homebrew app act as a
front-end for a user's **own** Spotify Premium account.

```
┌─────────────┐      device-code pairing       ┌──────────────────────────┐
│  Wii U      │ ─────────────────────────────► │  RELAY  (Rust / VPS)     │
│  client     │                                 │                          │
│  (wut/SDL2) │  ◄── REST + WS control ───────► │  ┌────────────────────┐  │
│             │                                 │  │ axum HTTP/WS API   │  │
│             │  ◄── HTTP chunked PCM ───────── │  ├────────────────────┤  │
└─────────────┘                                 │  │ Session manager    │  │
                                                │  │  (1 librespot/user)│  │
                                                │  ├────────────────────┤  │
                                                │  │ OAuth + allowlist  │  │
                                                │  │ Token store (enc.) │  │
                                                │  │ Spotify Web API    │  │
                                                │  │  proxy             │  │
                                                │  └────────────────────┘  │
                                                └────────────┬─────────────┘
                                                             │ OAuth + audio
                                                             ▼
                                                   accounts.spotify.com
                                                   api.spotify.com
                                                   Spotify AP (librespot)
```

**Why a relay?** The Wii U cannot run librespot (no Rust toolchain target, no
Spotify DRM/Vorbis pipeline, weak CPU). The relay holds the Spotify session,
decodes audio, and hands the console raw PCM. The console never sees a Spotify
token.

*FR : la Wii U ne peut pas faire tourner librespot ni gérer le DRM Spotify. Le
relais détient la session, décode l'audio, envoie du PCM brut. La console ne
voit jamais de token.*

## 2. PRIME DIRECTIVE — central vs self-hosted seam

The central relay is a **deployment choice, not a client dependency**. The client
talks to a relay through the versioned API in [`/proto`](./proto). Nothing in the
protocol assumes "one shared server".

A single config flag drives the whole difference:

```
RELAY_MODE = central | self-hosted
```

| Concern        | central                              | self-hosted                          |
|----------------|--------------------------------------|--------------------------------------|
| Relay URL      | `https://your-domain.example`       | user-supplied (`sd:/diizeru/relay.cfg`) |
| Onboarding     | invite code → our allowlist          | user owns the box, allowlist = {self}|
| Spotify app    | our client_id                        | user's own client_id (their dashboard)|
| Protocol       | **identical**                        | **identical**                        |
| Client code    | **identical**                        | **identical**                        |

The seam is enforced by: (a) the client only ever reads `relay_base_url` from
config — never a compile-time constant; (b) the relay reads `RELAY_MODE` at
startup and only changes onboarding/URL behavior, never wire format. Switching a
user to self-hosted = ship them the relay container + a one-line config on SD.

*FR : le relais central est un choix de déploiement. Le flag `RELAY_MODE` ne
change que l'URL et l'onboarding, jamais le protocole. Passer en self-hosted =
livrer le conteneur relais + une ligne de config sur SD, sans retoucher le
client.*

## 3. Repository layout

```
/proto    OpenAPI 3.1 + WS event schema — SOURCE OF TRUTH for client↔relay
/relay    Rust service (axum, tokio, librespot-as-library)
/client   Wii U homebrew (devkitPro + wut, C++17, SDL2, libcurl)
/deploy   docker-compose, Caddy TLS reverse proxy, VPS scripts
/docs     diagrams, notes
README.md / ARCHITECTURE.md / SECURITY.md
```

## 4. Relay internals

| Module            | Responsibility                                                   |
|-------------------|-----------------------------------------------------------------|
| `api/`            | axum routers: REST control + WS, pairing, browse proxy, stream  |
| `auth/`           | OAuth 2.0 Auth Code + PKCE, device-code pairing, admin auth      |
| `store/`          | allowlist + encrypted token store (libsodium sealed/secretbox)   |
| `session/`        | per-user librespot session lifecycle, inactivity GC, max-concurrency |
| `audio/`          | `AudioSource` (librespot) → `StreamEncoder` (PCM now, Opus later)|
| `proxy/`          | thin proxy of Spotify Web API for search/browse (token stays here)|

### 4.1 Session manager

- One librespot `Session` per authenticated user, keyed by Spotify user-id.
- Started lazily when the user's console connects; destroyed after
  `SESSION_IDLE_TIMEOUT` (configurable).
- `MAX_CONCURRENT_SESSIONS` caps load on the beta VPS. New connections past the
  cap get `503 Busy`.
- Resource bounds (CPU/mem) are enforced at the **container** layer in `/deploy`
  (cgroups via compose `deploy.resources`), plus an in-process per-session
  buffer cap so one stream can't balloon memory.

### 4.2 Audio path (v1)

```
librespot decode (Ogg Vorbis → f32) ─► AudioSource ─► StreamEncoder ─► HTTP chunked
                                                          │
                                            v1: PcmS16LE (44.1k stereo) ~1.41 Mbps
                                            v2: OpusEncoder (~96-128 kbps)
```

`StreamEncoder` is a trait. The client negotiates format via the `Accept`-like
`?fmt=pcm_s16le` query param (default `pcm_s16le`). Adding Opus later = new
encoder impl + new fmt value; **client audio code already abstracts the decoder
behind `IAudioBackend`**, so only a new decode path is added, no protocol break.

Bandwidth: `44100 * 2ch * 16bit = 1.411 Mbit/s` per listener. Documented so the
VPS egress is sized (`MAX_CONCURRENT_SESSIONS * 1.41 Mbps`).

## 5. Client internals

```
core/      API client, data models, player state machine, queue, cache
audio/     IAudioBackend (AX/sndcore2 or SDL2_mixer), ring buffer, PCM pull
ui/        SDL2 screens, mini-player, focus/nav, on-screen keyboard
platform/  wut init, SD mount, network init, input (VPAD/KPAD)
```

Strict rule: **no business logic in `ui/`**. UI observes `core/` state and emits
intents. Network fetches are async (worker thread + queue) so the 60fps render
loop never blocks.

### Player state machine

```
Stopped ──play──► Loading ──ready──► Playing ⇄ Paused
   ▲                 │                  │
   └──────error──────┴─────error────────┴──► Error ──retry──► Loading
```

## 6. Protocol seam (`/proto`)

OpenAPI 3.1 for REST + a versioned WS event schema. Versioned under
`/v1`. Breaking changes bump to `/v2`; relay can serve both during migration.
The client pins a major version and feature-detects via `GET /v1/capabilities`.

## 7. Data flow: first launch → playing

1. Console boots client → no session on SD → shows **device code**.
2. Console `POST /v1/pair/start` → relay returns `{ device_code, user_code,
   verify_url, interval }`. TV shows `user_code` + `your-domain.example`.
3. User opens verify URL on phone, enters `user_code`, completes Spotify OAuth
   (real accounts.spotify.com) **iff** their account is on the allowlist.
4. Console polls `POST /v1/pair/poll` → gets a long-lived **relay session token**
   (NOT a Spotify token). Persisted to `sd:/diizeru/session.json`.
5. Console uses relay token for all REST/WS + the PCM stream.
6. librespot session spins up; `GET /v1/stream?fmt=pcm_s16le` plays.

## 8. Tech choices & justification

| Choice | Why |
|--------|-----|
| Rust + axum + tokio | librespot is Rust; one async runtime end-to-end; strong typing for token handling |
| librespot **as crate** | clean per-user `Session` objects, no fragile CLI process babysitting |
| PCM first | zero decode on Wii U → fastest path to audible; Opus deferred behind trait |
| device-code pairing | console has no keyboard-friendly OAuth; phone does the hard part |
| Caddy TLS | automatic Let's Encrypt, tiny config, fits single-VPS beta |
| OpenAPI in `/proto` | one source of truth, enables self-hosted seam, client/relay stay in sync |
| SDL2 + libcurl on wut | mature wut ports, fastest route to a real GUI |
