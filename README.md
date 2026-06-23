# DiizerU 🎵 — Deezer on the Wii U

**DiizerU turns a Wii U into a Deezer music player.** A homebrew app on the
console talks to a small relay server that streams your own Deezer Premium
library to the TV — browse, search, play, queue, seek, loop, album art, all from
the GamePad (buttons **or** touchscreen).

> **Private, invite-only beta. Hobby / grey-zone project.** Read the
> [limitations](#-limitations--legal) before using. You use your **own** Deezer
> Premium account.

---

## ✨ What works today

- **Pairing** — show a code on the TV, link from your phone (no password on our pages)
- **Browse** — your Liked Songs, your Playlists (incl. private), albums
- **Search** — any track, via an on-screen keyboard (D-pad **or** touch)
- **Playback** — play / pause (true resume), next / prev, **seek**, **loop** (off / all / one)
- **Now-playing** — title, artist, **album art**, progress bar
- **Audio** — relay decodes + re-encodes to a light stream (~22 KB/s) so the
  console does zero DRM work
- **Self-hostable** — the relay is a single Rust binary; the central relay is just
  a deployment choice, not a hard dependency

## 🟣 Why a private beta?

1. **Grey zone.** DiizerU plays your own Deezer Premium content but uses the
   unofficial streaming path (like `deemix`-style tools). This likely sits
   outside Deezer's Terms of Service. Invite-only keeps it small and personal.
2. **Your token is sensitive.** Onboarding uses your Deezer **ARL** (a session
   token). On the central relay we encrypt it at rest and never log it — but you
   should prefer **self-hosting** so the token never leaves your own machine.
3. **Hobby reliability.** One small server, bounded concurrent sessions, no SLA.

---

## 🎟️ Join the central beta (1-week public invite)

Want to try it without self-hosting? Use the public invite below — valid for
**one week (until 2026-06-30)**, multi-use:

```
Relay:        https://diizeru.cyclooo.fr
Invite code:  hEYQPFQbJ0yF
```

Steps:
1. Get a Wii U running **Aroma** (homebrew environment).
2. Grab `DiizerU.wuhb` from [Releases](../../releases) and copy it to
   `sd:/wiiu/apps/`. *(Or build it yourself — see below — and put a one-line
   `relay.cfg` containing `https://diizeru.cyclooo.fr/v1` at
   `sd:/diizeru/relay.cfg`.)*
3. Launch DiizerU → it shows a **TV code**.
4. On your phone open **https://diizeru.cyclooo.fr/v1/pair**, enter the TV code,
   the invite code, and your **Deezer ARL** (the page explains how to find it).
5. Done — browse and play on the TV.

> Prefer not to share your ARL with someone else's server? **Self-host** instead. 👇

---

## 🏠 Self-host (recommended)

Run your own relay so your ARL never leaves your machine. Two parts: the **relay**
(server) and the **client** (`.wuhb` on the Wii U).

### Prerequisites

- A small always-on machine/VPS (Linux) with a domain + ports 80/443, **or** any
  box on your LAN.
- **Deezer Premium** account.
- A Wii U with **Aroma**.
- To build from source: **Rust** (relay) and **devkitPro + wut + SDL2 portlibs**
  (client).

### Relay — Docker (easiest)

```sh
git clone https://github.com/<you>/DiizerU && cd DiizerU/deploy
cp ../relay/.env.example .env        # then edit .env (see below)
docker compose up -d --build         # relay + Caddy (auto-TLS for your domain)
```

`.env` essentials:

```ini
RELAY_MODE=self-hosted               # implicit allowlist = you; no invite needed
PUBLIC_BASE_URL=https://your-domain.example
BIND_ADDR=0.0.0.0:8080
DIIZERU_MASTER_KEY=...   # head -c32 /dev/urandom | base64  (encrypts your ARL)
DIIZERU_ADMIN_TOKEN=...  # head -c24 /dev/urandom | base64  (admin API)
```

Point DNS at the box, bring up the stack, then pair the console against
`https://your-domain.example/v1/pair`. In `self-hosted` mode you're
auto-allowlisted (no invite needed). See [`docs/SELF-HOST.md`](docs/SELF-HOST.md)
for the native (no-Docker) install, running behind an existing nginx, and the
real-audio build flag (`--features deezer`).

### Client — build the `.wuhb`

```sh
sudo dkp-pacman -S --needed wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-sdl2_mixer
cd client && make                    # -> DiizerU.wuhb
```

Copy `DiizerU.wuhb` to `sd:/wiiu/apps/`. Set your relay URL by putting it in
`sd:/diizeru/relay.cfg` (e.g. `https://your-domain.example/v1`), or change the
default in `client/src/main.cpp` before building.

---

## 🧭 How it works

```
Wii U (wut + SDL2)  ──REST/pairing──▶  Relay (Rust, axum)
        ▲                                   │ ARL login (encrypted at rest)
        │ ◀── chunked ADPCM audio ──────────┤ Deezer: resolve → download →
        └────────── control ────────────────┘ Blowfish-decrypt → decode → ADPCM
```

- The console never holds Deezer credentials or does any decryption — it just
  plays a light PCM/ADPCM stream and renders the UI.
- The client↔relay API is versioned in [`proto/`](proto) (OpenAPI), so the same
  client works against the central relay or your self-hosted one — only the URL
  changes (the `RELAY_MODE` seam).
- Details: [`ARCHITECTURE.md`](ARCHITECTURE.md) · security & privacy:
  [`SECURITY.md`](SECURITY.md).

## 📂 Layout

| Path | What |
|------|------|
| [`relay/`](relay) | Rust relay (axum, Deezer source, ADPCM stream) |
| [`client/`](client) | Wii U homebrew (devkitPro/wut, SDL2) |
| [`proto/`](proto) | client↔relay API contract (OpenAPI + WS schema) |
| [`deploy/`](deploy) | Docker Compose + Caddy + scripts |
| [`docs/`](docs) | self-host guide & notes |

## 🎮 Controls

D-pad move · **A** open/play · **B** back · **Y** play/pause · **L/R** prev/next ·
**(−)** cycle repeat · **D-pad ←/→** seek ∓10s · **X** re-link.
Or just **tap the GamePad touchscreen** (keys, list rows, scrub bar, play/pause).

## 🔒 Security & privacy

- Your **ARL is encrypted at rest** (XChaCha20-Poly1305, key out of the repo) and
  **never logged**. Self-host to keep it entirely on your machine.
- TLS on all public endpoints. Admin API behind a separate token. Kill switch +
  per-user revocation. See [`SECURITY.md`](SECURITY.md).
- **No DRM is shipped to the console**; decryption of your own entitled content
  happens server-side. Personal-use tool.

## ⚠️ Limitations & legal

- **Beta.** Expect rough edges; no uptime guarantees.
- **Your own Deezer Premium only.** No account sharing.
- Uses Deezer's **unofficial** streaming path → likely **against Deezer's ToS for
  third-party clients**. Use **at your own risk**, for personal/educational
  purposes, on an account you own. You are responsible for your use.
- Not affiliated with or endorsed by Deezer or Nintendo. "Deezer", "Wii U" and
  related marks belong to their respective owners.

## 📝 License

**[GNU AGPL-3.0](LICENSE)** — copyleft, including over a network: if you run a
modified DiizerU relay as a service, you must offer your users the source.
Bundled font (Roboto) is Apache-2.0; the CA bundle is from the Mozilla/curl
project. Trademarks belong to their owners.
