> **Note (beta):** DiizerU now streams from **Deezer** (per-user ARL token, decrypted server-side). Some sections below describe the original Spotify/librespot design and are kept for historical context; the security model (encrypted-at-rest token, never logged, allowlist, kill switch) applies to the Deezer ARL the same way.

# DiizerU — Security & Privacy

> English first. *FR* summary inline.

## Threat model (scope)

- **In scope:** protecting beta users' Spotify tokens, preventing non-invited
  access, keeping the Wii U free of any Spotify credential, resisting basic
  abuse (auth/pairing brute force) of a single-VPS beta.
- **Out of scope:** nation-state attackers, physical access to the VPS, Spotify
  changing terms. This is a **private beta, grey-zone hobby project** — see the
  README's limitations section.

## 1. Spotify credentials — NON-NEGOTIABLE

- OAuth 2.0 **Authorization Code + PKCE** only. The user always authenticates on
  the **real** `accounts.spotify.com`. We never render a Spotify password field.
- Each user authorizes **their own** Premium account. No shared account, no DRM
  bypass — librespot streams what the user is already entitled to.
- **The Wii U never receives a Spotify token.** It holds only an opaque relay
  session token. All Spotify API/audio access happens server-side.

*FR : OAuth PKCE sur le vrai site Spotify, jamais de mot de passe collecté. La
console ne reçoit qu'un token de session relais, jamais un token Spotify.*

## 2. Token storage — encrypted at rest

- Refresh tokens are encrypted before touching disk using libsodium
  `crypto_secretbox` (XSalsa20-Poly1305). Key material lives **outside the repo**,
  injected via `DIIZERU_MASTER_KEY` env / Docker secret.
- Access tokens are kept in memory only; refreshed on demand.
- DB stores: `user_id`, `enc_refresh_token`, `nonce`, `created_at`,
  `allowlist_status`. **Never** a plaintext token.
- Key rotation: `enc_refresh_token` carries a key-id prefix; rotating the master
  key re-wraps lazily on next refresh.

## 3. Allowlist & onboarding

- Onboarding is gated by a **single-use invite code** (relay-generated) OR an
  explicit Spotify user-id allowlist. A non-listed account cannot complete the
  OAuth callback — the relay drops the exchange and stores nothing.
- Admin endpoints (`/admin/*`) use a **separate** bearer credential
  (`DIIZERU_ADMIN_TOKEN`), never the user auth path, and are rate-limited.

## 4. Revocation & kill switch

- `POST /admin/revoke/{user_id}`: deletes encrypted tokens, calls Spotify token
  revocation, kills the live librespot session, invalidates the relay session
  token. Effective immediately.
- **Kill switch:** `POST /admin/killswitch` (or `touch KILLSWITCH` file checked
  at startup + SIGHUP): refuse all new sessions, tear down all live sessions,
  return `503` everywhere except `/admin`. One command stops the service.

*FR : révocation immédiate (tokens supprimés + session tuée) et kill switch
global pour tout couper vite.*

## 5. Transport & rate limiting

- TLS on every public endpoint (Caddy auto Let's Encrypt, see `/deploy`).
- HSTS, no plaintext fallback.
- Rate limits: per-IP on `/v1/pair/*` and OAuth callback; exponential backoff on
  failed invite-code attempts; global cap via `MAX_CONCURRENT_SESSIONS`.

## 6. Logging — privacy-aware

- Structured logs (`tracing`) with a redaction layer: tokens, codes, and
  `Authorization` headers are filtered before emit.
- No PII beyond Spotify `user_id` (needed for allowlist). No track-level history
  persisted beyond the live now-playing state.
- Log level for token/auth modules defaults to `info`; secrets never logged even
  at `trace`.

## 7. GDPR / data-controller posture

- For the central deployment, **the operator is the data controller** for invited
  users' tokens and `user_id`s.
- Lawful basis: consent (the user accepts the invite and authorizes via Spotify).
- Data minimization: only tokens + `user_id` + minimal session metadata.
- Right to erasure: `POST /admin/revoke/{user_id}` deletes all stored data for
  that user.
- In **self-hosted** mode the user is their own controller — no third-party data.

*FR : en mode central tu es responsable de traitement (tokens + user_id des
invités). Base légale : consentement. Effacement via la révocation. En
self-hosted, chacun est son propre responsable.*

## 8. Reporting

This is a private hobby beta. Report issues via GitHub
(GitHub issues). No bug-bounty.
