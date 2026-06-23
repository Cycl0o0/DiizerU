# /relay — DiizerU relay (Rust)

axum + tokio service. OAuth (PKCE) + allowlist + per-user session manager +
PCM audio stream. librespot is used **as a library** (feature-gated) — never the
CLI. The control/audio contract lives in [`/proto`](../proto).

## Build & run

```sh
cp .env.example .env          # fill in secrets
# minimum to boot:
export DIIZERU_MASTER_KEY=$(head -c32 /dev/urandom | base64)
export DIIZERU_ADMIN_TOKEN=$(head -c24 /dev/urandom | base64)

cargo run                     # default build: tone AudioSource, curl-testable
cargo run --features librespot  # real Spotify audio (heavier compile; see status)
```

## Status & limitations (beta)

| Piece | State |
|-------|-------|
| OAuth 2.0 Auth Code + PKCE (real accounts.spotify.com) | ✅ implemented |
| Allowlist (invite codes + user-id), admin revoke, kill switch | ✅ |
| Encrypted-at-rest refresh tokens (XChaCha20-Poly1305) | ✅ |
| Device-code pairing (start/poll/verify/callback) | ✅ |
| Spotify Web API proxy (search/browse/playlists/favorites) | ✅ |
| Per-user session manager (idle GC, max-concurrency) | ✅ |
| PCM s16le 44.1k chunked stream, curl-verified | ✅ (tone source) |
| **librespot real decode → PCM ring buffer** (`--features librespot`) | ✅ compiles + boots; live playback pending a real Spotify token |

The default build streams a 440 Hz tone so the **whole HTTP→PCM pipeline is
testable today**. The `librespot` feature wires librespot 0.8's audio `Sink`
into `PcmRing` (`audio::librespot_source`): a real per-user `Session` + `Player`
is connected lazily on first playback/stream, and the session's `AudioSource` is
swapped from the tone to the ring buffer — **no protocol or client change**.
Verifying actual Spotify audio needs the OAuth pairing flow against a real
Spotify app (a Premium token); the code path is in place and the feature build
runs.

### librespot build note

librespot-core 0.8.0 has a dependency skew (`vergen` 9.1.0 pulls `vergen-lib`
9.1.0 while `vergen-gitcl` 1.0.8 pins 0.1.6), which breaks its build script. The
committed `Cargo.lock` pins `vergen = 9.0.6` to unify on `vergen-lib` 0.1.6. If
you regenerate the lock, re-apply:

```sh
cargo update -p vergen@9.1.0 --precise 9.0.6
```

## Curl smoke test (no Spotify app needed)

```sh
export DIIZERU_MASTER_KEY=$(head -c32 /dev/urandom | base64)
export DIIZERU_ADMIN_TOKEN=devadmintoken1234567890
export DEV_SEED_TOKEN=devtoken BIND_ADDR=127.0.0.1:8099
cargo run &

curl -s localhost:8099/v1/capabilities | jq
# 2 seconds of raw PCM (≈352800 bytes = 2s * 44100 * 2ch * 2B):
curl -s -H 'Authorization: Bearer devtoken' \
  'localhost:8099/v1/stream?fmt=pcm_s16le' --max-time 2 -o tone.raw
# play it:  ffplay -f s16le -ar 44100 -ch_layout stereo tone.raw
```

## Admin

```sh
A="Authorization: Bearer $DIIZERU_ADMIN_TOKEN"
curl -X POST  -H "$A" localhost:8080/v1/admin/invite          # single-use code
curl -X PUT   -H "$A" localhost:8080/v1/admin/allow/<userid>  # allowlist
curl -X POST  -H "$A" localhost:8080/v1/admin/revoke/<userid> # kill + wipe tokens
curl -X POST  -H "$A" localhost:8080/v1/admin/killswitch      # stop everything
```

## Layout

```
src/
  config.rs      env config, RELAY_MODE
  crypto.rs      XChaCha20-Poly1305 token sealing, random tokens
  error.rs       ApiError -> /proto Error
  model.rs       wire DTOs (mirror /proto)
  state.rs       AppState
  store/         allowlist, invites, sealed tokens, relay sessions
  auth/          OAuth PKCE + device-code pairing
  session/       per-user PlayerSession + manager (GC, max-concurrency)
  audio/         AudioSource + StreamEncoder traits, tone + librespot sources
  proxy/         Spotify Web API proxy (token stays server-side)
  api/           axum routers + handlers
```
