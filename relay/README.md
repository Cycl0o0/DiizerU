# relay — DiizerU relay (Rust)

axum + tokio. Pairs the console, proxies Deezer for browse/search, logs in with
the user's ARL, and streams decrypted+decoded audio to the Wii U as IMA ADPCM.

## Build & run

```sh
cp .env.example .env          # fill in the secrets (see below)
cargo run                     # Deezer audio is built in — no feature flags
```

Minimum env:

```ini
RELAY_MODE=self-hosted
PUBLIC_BASE_URL=https://your-domain.example
DIIZERU_MASTER_KEY=...   # head -c32 /dev/urandom | base64  (encrypts the ARL)
DIIZERU_ADMIN_TOKEN=...  # head -c24 /dev/urandom | base64
```

## API

The client↔relay contract is in [`../proto`](../proto). Highlights:

- `POST /v1/pair/start`, `POST /v1/pair/poll`, `GET /v1/pair` (web), `POST /v1/pair/deezer`
- `GET /v1/search`, `/v1/browse/{playlists,favorites,playlist/:id,album/:id}`
- `GET /v1/playback`, `POST /v1/playback/command`, `GET/POST /v1/queue`
- `GET /v1/stream?fmt=adpcm_ima` — chunked audio
- `POST /v1/admin/{revoke/:id,killswitch}` (admin bearer)

## Layout

```
src/
  config.rs   env config (RELAY_MODE)
  crypto.rs   ARL sealing (XChaCha20-Poly1305), random tokens
  deezer/     ARL login, Blowfish decrypt, MP3/FLAC decode, AudioSource, browse
  audio/      AudioSource + StreamEncoder traits; PCM + IMA-ADPCM encoders
  session/    per-user player session (idle GC, max-concurrency)
  store/      allowlist (for revocation), encrypted ARL store, relay sessions
  api/        axum routers + handlers
```

The ARL is encrypted at rest and never logged — see [`../SECURITY.md`](../SECURITY.md).
