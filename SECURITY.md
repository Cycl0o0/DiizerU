# Security & Privacy

DiizerU is a hobby project. This is the threat model and what the relay does with
your data. The short version: **self-host if you can** — then nothing sensitive
ever leaves your machine.

## What's sensitive

Your **Deezer ARL** — the session token from your browser cookies. It grants
access to your Deezer account, so treat it like a password.

## What the relay does with it

- **Encrypted at rest.** The ARL is sealed with XChaCha20-Poly1305 before it
  touches disk. The key (`DIIZERU_MASTER_KEY`) lives in the environment, outside
  the repo and outside the data store.
- **Never logged.** No log line prints the ARL, at any level.
- **Stays server-side.** The console never receives the ARL or any Deezer
  credential — only an opaque relay session token it uses as a bearer.
- **Revocable.** `POST /v1/admin/revoke/{user_id}` deletes the stored token and
  kills the session. `POST /v1/admin/killswitch` stops everything at once.

## Transport & access

- TLS on all public endpoints (Caddy/Let's Encrypt in the Docker deploy, or your
  own reverse proxy).
- The admin API is behind a separate token (`DIIZERU_ADMIN_TOKEN`), never the
  user path.
- Onboarding is open: any account whose Deezer ARL logs in successfully can pair.
  Access control is after the fact — an operator can revoke any user, which
  invalidates their relay tokens immediately.

## Privacy

- The only personal data stored is your encrypted ARL and a Deezer user id,
  needed to key the session and to support revocation.
- No listening history is persisted beyond the live now-playing state.

## GDPR-ish note

If you run a relay for others, you're the data controller for their encrypted
ARLs and ids; lawful basis is their consent (they paste their own token), and
erasure is the revoke endpoint. In **self-hosted** mode there's no third party —
it's just your own data on your own box.

## Reporting

Open a GitHub issue. No bug bounty — it's a hobby project.

## The honest caveat

DiizerU reaches Deezer over the unofficial streaming path and decrypts your own
entitled content server-side. That almost certainly breaks Deezer's terms for
third-party clients. Personal/educational use, your own Premium account, your own
risk.
