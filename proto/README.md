# /proto — client↔relay contract (v1)

**Source of truth** for both `/relay` and `/client`. Same wire format in
`central` and `self-hosted` modes (PRIME DIRECTIVE) — only base URL / onboarding
differ, never the protocol.

- `openapi.yaml` — REST API, OpenAPI 3.1.
- `ws-events.schema.json` — WebSocket push event frames.

## Versioning

- Path-pinned major version: `/v1/...`. Breaking change → `/v2`; the relay may
  serve both during migration.
- Client pins a major version and feature-detects via `GET /v1/capabilities`
  before assuming optional features (e.g. future Opus audio).

## Key invariants

1. The client sends `Authorization: Bearer <relay_session_token>` (from
   `/pair/poll`). **This is never a Spotify token.**
2. All Spotify Web API access is proxied server-side. Spotify tokens never cross
   this boundary.
3. Audio is an opaque chunked byte stream; format negotiated via `?fmt=`.
   v1: `pcm_s16le` 44.1kHz stereo. Adding Opus = new `fmt` value, no break.

## Validate / lint

```sh
npx @redocly/cli lint proto/openapi.yaml
```
