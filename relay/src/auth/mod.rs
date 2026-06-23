//! Spotify OAuth 2.0 Authorization Code + PKCE, and the relay's device-code
//! pairing state machine.
//!
//! NON-NEGOTIABLE (SECURITY.md): the user always authenticates on the real
//! accounts.spotify.com. We never collect a Spotify password. Tokens stay
//! server-side; the console only ever gets an opaque relay session token.

pub mod pairing;

use base64::Engine;
use serde::Deserialize;
use sha2::{Digest, Sha256};

// streaming (librespot, ready for when Spotify's audio-key block #1649 lifts) +
// Web API scopes for browse/search/library (M6). Audio keys are blocked
// server-side regardless of scope right now, so requesting both is fine.
const SCOPES: &str = "streaming user-read-private user-library-read \
playlist-read-private playlist-read-collaborative";

/// PKCE pair. `verifier` is kept server-side (in the pairing record), only the
/// `challenge` goes to Spotify.
pub struct Pkce {
    pub verifier: String,
    pub challenge: String,
}

pub fn gen_pkce() -> Pkce {
    let verifier = crate::crypto::random_token(64);
    let digest = Sha256::digest(verifier.as_bytes());
    let challenge = base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(digest);
    Pkce { verifier, challenge }
}

pub fn authorize_url(client_id: &str, redirect_uri: &str, challenge: &str, state: &str) -> String {
    format!(
        "https://accounts.spotify.com/authorize?response_type=code\
&client_id={cid}&scope={scope}&redirect_uri={redir}&state={state}\
&code_challenge_method=S256&code_challenge={chal}",
        cid = urlencoding::encode(client_id),
        scope = urlencoding::encode(SCOPES),
        redir = urlencoding::encode(redirect_uri),
        state = urlencoding::encode(state),
        chal = challenge,
    )
}

#[derive(Debug, Deserialize)]
pub struct TokenResponse {
    pub access_token: String,
    pub refresh_token: Option<String>,
    pub expires_in: i64,
    #[serde(default)]
    pub scope: String,
}

/// Exchange an authorization code for tokens (PKCE; no client secret required
/// for public clients, but supported if configured for a confidential client).
pub async fn exchange_code(
    http: &reqwest::Client,
    client_id: &str,
    client_secret: Option<&str>,
    redirect_uri: &str,
    code: &str,
    verifier: &str,
) -> anyhow::Result<TokenResponse> {
    let mut form = vec![
        ("grant_type", "authorization_code"),
        ("code", code),
        ("redirect_uri", redirect_uri),
        ("client_id", client_id),
        ("code_verifier", verifier),
    ];
    if let Some(sec) = client_secret {
        form.push(("client_secret", sec));
    }
    let resp = http
        .post("https://accounts.spotify.com/api/token")
        .form(&form)
        .send()
        .await?;
    if !resp.status().is_success() {
        anyhow::bail!("token exchange failed: {}", resp.status());
    }
    Ok(resp.json::<TokenResponse>().await?)
}

/// Refresh an access token from a stored refresh token.
pub async fn refresh_token(
    http: &reqwest::Client,
    client_id: &str,
    client_secret: Option<&str>,
    refresh_token: &str,
) -> anyhow::Result<TokenResponse> {
    let mut form = vec![
        ("grant_type", "refresh_token"),
        ("refresh_token", refresh_token),
        ("client_id", client_id),
    ];
    if let Some(sec) = client_secret {
        form.push(("client_secret", sec));
    }
    let resp = http
        .post("https://accounts.spotify.com/api/token")
        .form(&form)
        .send()
        .await?;
    if !resp.status().is_success() {
        anyhow::bail!("token refresh failed: {}", resp.status());
    }
    Ok(resp.json::<TokenResponse>().await?)
}

#[derive(Debug, Deserialize)]
pub struct SpotifyProfile {
    pub id: String,
    #[serde(default)]
    pub display_name: Option<String>,
    #[serde(default)]
    pub product: Option<String>,
}

pub async fn fetch_profile(
    http: &reqwest::Client,
    access_token: &str,
) -> anyhow::Result<SpotifyProfile> {
    // Retry on 429 (rate limit) honoring Retry-After, so pairing rides through
    // transient Spotify throttling.
    for attempt in 0..4 {
        let resp = http
            .get("https://api.spotify.com/v1/me")
            .bearer_auth(access_token)
            .send()
            .await?;
        let status = resp.status();
        if status.is_success() {
            return Ok(resp.json::<SpotifyProfile>().await?);
        }
        if status.as_u16() == 429 && attempt < 3 {
            let wait = resp
                .headers()
                .get("retry-after")
                .and_then(|v| v.to_str().ok())
                .and_then(|s| s.parse::<u64>().ok())
                .unwrap_or(2)
                .min(10);
            tokio::time::sleep(std::time::Duration::from_secs(wait + 1)).await;
            continue;
        }
        anyhow::bail!("profile fetch failed: {}", status);
    }
    anyhow::bail!("profile fetch failed: rate limited")
}
