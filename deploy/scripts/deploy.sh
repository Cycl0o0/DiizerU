#!/usr/bin/env bash
# Build + (re)start the stack on the droplet. Run from /opt/diizeru/deploy.
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f .env ] || { echo "missing deploy/.env (cp ../relay/.env.example .env)"; exit 1; }
docker compose pull caddy || true
docker compose up -d --build
docker compose ps
echo "[*] tailing relay logs (Ctrl-C to stop)"
docker compose logs -f --tail=50 relay
