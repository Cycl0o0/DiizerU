# Self-hosting DiizerU

Run your own relay so your Deezer ARL never leaves your machine. You need the
**relay** (server) and the **client** (`.wuhb` on the Wii U).

## 1. Relay

### Option A — Docker + Caddy (auto-TLS)

Best for a VPS with a domain.

```sh
cd deploy
cp ../relay/.env.example .env
$EDITOR .env                 # see env vars below
docker compose up -d --build
```

`deploy/docker-compose.yml` runs the relay + Caddy; Caddy gets a Let's Encrypt
cert automatically. Set your domain in `deploy/caddy/Caddyfile` and point DNS at
the box first.

### Option B — native (systemd)

```sh
# build (real Deezer audio needs the deezer feature)
cd relay && cargo build --release --features deezer
sudo useradd -r -s /usr/sbin/nologin diizeru
sudo mkdir -p /opt/diizeru && sudo cp -r . /opt/diizeru/relay
sudo cp deploy/systemd/diizeru-relay.service /etc/systemd/system/
sudo $EDITOR /opt/diizeru/relay/.env         # env vars below
sudo systemctl enable --now diizeru-relay
```

Put a TLS reverse proxy (Caddy/nginx) in front, terminating HTTPS to
`127.0.0.1:8080`. For `/v1/stream` disable proxy buffering and use a long read
timeout; for `/v1/ws` pass the WebSocket upgrade headers.

### Environment (`relay/.env`)

```ini
RELAY_MODE=self-hosted        # implicit allowlist = you; no invite codes needed
BIND_ADDR=0.0.0.0:8080
PUBLIC_BASE_URL=https://your-domain.example
DIIZERU_MASTER_KEY=...        # head -c32 /dev/urandom | base64  (encrypts the ARL)
DIIZERU_ADMIN_TOKEN=...       # head -c24 /dev/urandom | base64  (admin API)
SESSION_IDLE_TIMEOUT_SECS=900
MAX_CONCURRENT_SESSIONS=5
STORE_PATH=/opt/diizeru/data/store.json
LIBRESPOT_CACHE_DIR=/opt/diizeru/data/librespot   # only used by the librespot build
DEEZER_ARL=                   # optional: only for the /admin/deezer-test endpoint
```

Build flags:
- `--features deezer` — real Deezer audio (recommended).
- no features — streams a test tone (handy to verify the pipeline without an ARL).
- `--features librespot` — the legacy Spotify path (currently blocked upstream;
  see the main README).

## 2. Client (`.wuhb`)

```sh
# one-time portlibs
sudo dkp-pacman -S --needed wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_image wiiu-sdl2_mixer
# requires DEVKITPRO + DEVKITPPC in the environment
cd client && make            # -> DiizerU.wuhb
```

Copy `DiizerU.wuhb` to `sd:/wiiu/apps/`. Tell it which relay to use, either:

- create `sd:/diizeru/relay.cfg` containing `https://your-domain.example/v1`, **or**
- edit `kDefaultRelayUrl` in `client/src/main.cpp` and rebuild.

## 3. Pair

1. Launch DiizerU on the Wii U → it shows a TV code.
2. On a phone open `https://your-domain.example/v1/pair`.
3. Enter the TV code and your **Deezer ARL** (the page shows how to find it in
   your browser cookies). In `self-hosted` mode no invite code is required.
4. Browse and play.

## Admin (optional)

```sh
A="Authorization: Bearer $DIIZERU_ADMIN_TOKEN"
# single-use invite (central mode):
curl -X POST -H "$A" https://your-domain.example/v1/admin/invite
# public multi-use invite, valid N days:
curl -X POST -H "$A" "https://your-domain.example/v1/admin/invite?multi=true&days=7"
# revoke a user (delete token + kill session):
curl -X POST -H "$A" https://your-domain.example/v1/admin/revoke/<user_id>
# kill switch (stop everything):
curl -X POST -H "$A" https://your-domain.example/v1/admin/killswitch
```

## Notes

- Bandwidth: the console stream is ADPCM ~22 KB/s per listener — tiny.
- The relay holds your ARL encrypted at rest and never logs it. Back up
  `STORE_PATH` if you want pairings to survive a reinstall.
- Grey-zone tool — personal use, your own Deezer Premium. See the main README.
