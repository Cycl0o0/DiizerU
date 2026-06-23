# DiizerU

Deezer on the Wii U. It's a homebrew app for the console plus a little relay
server that does the heavy lifting, so you can sit on the couch and play your own
Deezer Premium library straight to the TV — search, playlists, the usual
transport controls, album art, all driven from the GamePad (sticks/buttons or the
touchscreen, your call).

It started as a "can I even do this" weekend thing and grew. It works on real
hardware. It's also a grey-zone hobby project, so please read the
[fine print](#the-fine-print) before diving in.

## State of things

Working right now:

- Pairing from your phone (the console shows a code, you finish on the web)
- Liked songs, your playlists (private ones too), albums
- Search with an on-screen keyboard — D-pad or just tap the touchscreen
- Play/pause that actually resumes where it left off, next/prev, seek, repeat (off/all/one)
- Now-playing with album art and a progress bar
- Audio gets decoded on the relay and sent to the console as a tiny ADPCM stream,
  so the Wii U never touches any DRM and the bandwidth is trivial

It's a beta. Rough edges exist. Nothing here is guaranteed to stay up.

## Trying it without hosting anything

I run a small relay for the beta. There's a public invite that's good for a week
(until **2026-06-30**) and can be used by more than one person:

```
Relay:   https://diizeru.cyclooo.fr
Invite:  hEYQPFQbJ0yF
```

What you need:

- A Wii U with Aroma
- A Deezer Premium account

Then:

1. Download `DiizerU.wuhb` from the [Releases](../../releases) page, drop it in
   `sd:/wiiu/apps/`.
2. Launch it. You'll get a code on the TV.
3. On your phone go to <https://diizeru.cyclooo.fr/v1/pair>, type in the TV code,
   the invite, and your Deezer ARL. The page walks you through finding the ARL —
   it's a cookie in your browser.
4. That's it, go listen to music.

If handing your ARL to my server makes you uneasy (totally fair), host your own —
then it never leaves your machine.

## Hosting your own

Two pieces: the relay (a single Rust binary) and the client (the `.wuhb`).

You'll want a Linux box — a cheap VPS with a domain, or honestly just something on
your LAN. Plus Deezer Premium, and the usual toolchains if you're building from
source (Rust for the relay, devkitPro for the client).

Quickest path is Docker, which also handles TLS via Caddy:

```sh
git clone https://github.com/Cycl0o0/DiizerU && cd DiizerU/deploy
cp ../relay/.env.example .env      # edit it, see below
docker compose up -d --build
```

The bits of `.env` that matter:

```ini
RELAY_MODE=self-hosted             # you're auto-allowlisted, no invite needed
PUBLIC_BASE_URL=https://your-domain.example
DIIZERU_MASTER_KEY=...             # head -c32 /dev/urandom | base64  -> encrypts your ARL
DIIZERU_ADMIN_TOKEN=...            # head -c24 /dev/urandom | base64
```

Point DNS at the box, bring it up, pair against `https://your-domain.example/v1/pair`.
Full walkthrough (native install, sitting behind an existing nginx, the build
flags) is in [docs/SELF-HOST.md](docs/SELF-HOST.md).

For the client:

```sh
sudo dkp-pacman -S --needed wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-sdl2_mixer
cd client && make                  # -> DiizerU.wuhb
```

Copy it to `sd:/wiiu/apps/`, and tell it where your relay lives by putting the URL
in `sd:/diizeru/relay.cfg` (e.g. `https://your-domain.example/v1`), or by changing
the default in `client/src/main.cpp` before you build.

## How it fits together

```
Wii U (wut + SDL2) ──pairing/REST──▶ Relay (Rust / axum)
       ▲                                  │ logs in with your ARL (stored encrypted)
       │ ◀──── ADPCM audio stream ────────┤ Deezer: resolve → download →
       └────────── controls ───────────────┘ decrypt → decode → re-encode
```

The console holds no credentials and decrypts nothing — it plays a light stream
and draws the UI. The client/relay API is versioned in [proto/](proto), so the
exact same client works against my relay or yours; only the URL changes. More in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Controls

Sticks/D-pad to move, A opens or plays, B goes back. Y is play/pause, L/R skip
tracks, D-pad left/right seeks ten seconds, minus cycles the repeat mode, X
re-links the account. Or ignore all that and poke the touchscreen — keys, rows,
the scrub bar and play/pause all respond to taps.

## Security, briefly

Your ARL is encrypted at rest (XChaCha20-Poly1305, key kept out of the repo) and
never written to logs. Everything public is over TLS. The admin API sits behind a
separate token, and there's a kill switch plus per-user revocation. Details in
[SECURITY.md](SECURITY.md). Still — hosting it yourself is the safest option.

## The fine print

- It's a beta and a hobby. No SLA, things may break.
- Your own Deezer Premium account only. No sharing accounts.
- It reaches Deezer the unofficial way, which almost certainly breaks Deezer's
  terms for third-party apps. Personal/educational use, your own account, your own
  risk. You're responsible for what you do with it.
- Not affiliated with Deezer or Nintendo. The names belong to them.

## License

[AGPL-3.0](LICENSE). If you run a modified version as a service, you owe your users
the source. The bundled Roboto font is Apache-2.0; the CA bundle comes from the
curl/Mozilla set.
