#!/usr/bin/env bash
# Provision a fresh Debian/Ubuntu DigitalOcean droplet for the DiizerU relay.
# Run AS ROOT on the droplet (YOUR_SERVER_IP):
#   ssh -i ~/.ssh/your_key root@YOUR_SERVER_IP 'bash -s' < provision.sh
set -euo pipefail

echo "[*] apt update + base packages"
apt-get update -y
apt-get install -y ca-certificates curl git ufw

echo "[*] Docker"
if ! command -v docker >/dev/null; then
  curl -fsSL https://get.docker.com | sh
fi

echo "[*] firewall: allow ssh/http/https only"
ufw allow OpenSSH
ufw allow 80/tcp
ufw allow 443/tcp
ufw --force enable

echo "[*] clone/update repo"
install -d /opt/diizeru
if [ ! -d /opt/diizeru/.git ]; then
  echo "    -> place the repo at /opt/diizeru (git clone <your-remote> /opt/diizeru)"
fi

cat <<'NOTE'

[next steps]
  1) Put the repo at /opt/diizeru
  2) cd /opt/diizeru/deploy && cp ../relay/.env.example .env
     - set RELAY_MODE=central
     - set SPOTIFY_CLIENT_ID + OAUTH_REDIRECT_URI=https://your-domain.example/v1/auth/callback
     - DIIZERU_MASTER_KEY=$(head -c32 /dev/urandom | base64)
     - DIIZERU_ADMIN_TOKEN=$(head -c24 /dev/urandom | base64)
  3) Point DNS A record your-domain.example -> this droplet IP
  4) docker compose up -d --build
  5) Add the same redirect URI in the Spotify developer dashboard.

[kill switch] docker compose exec relay sh -c 'curl -XPOST -H "Authorization: Bearer $DIIZERU_ADMIN_TOKEN" localhost:8080/v1/admin/killswitch'
NOTE
