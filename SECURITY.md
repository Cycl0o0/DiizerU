# Security & Privacy

DiizerU is a hobby project. This is the threat model and what happens with your
data. The short version: **nothing sensitive ever leaves your devices** — the app
runs entirely on the console and your token lives only on your SD card.

## What's sensitive

Your **Deezer ARL** — the session token from your browser cookies. It grants
access to your Deezer account, so treat it like a password.

## Where the ARL lives

- **On your computer / SD card only.** You put the ARL in `sd:/diizeru/arl.txt`.
  The console reads it from there and logs into Deezer directly. It is never sent
  to any server of ours — there is no server.
- **The web config generator runs in your browser.** <https://diizeru.cyclooo.fr>
  turns your ARL into an `arl.txt` file using client-side JavaScript (a `Blob`
  download). Nothing is uploaded; the page could be saved and run offline.
- **Plain text on the SD card.** `arl.txt` is not encrypted on disk — it's a
  local file on removable media you control, like any other homebrew config.
  Delete it when you're done if you like; pull the SD card to remove it entirely.

## Transport

- The console talks to Deezer over HTTPS (libcurl + mbedTLS, with a bundled
  CA root set). Your ARL goes only to Deezer, over TLS, exactly as a browser
  would send its cookie.

## Privacy

- No accounts, no pairing, no telemetry. The app stores nothing about you beyond
  the `arl.txt` you placed there yourself.
- No listening history is persisted beyond the live now-playing state in memory.

## Reporting

Open a GitHub issue. No bug bounty — it's a hobby project.

## The honest caveat

DiizerU reaches Deezer over the unofficial streaming path and decrypts your own
entitled content on the console. That almost certainly breaks Deezer's terms for
third-party clients. Personal/educational use, your own Premium account, your own
risk.
