//! Deezer gw-light + media API client. Auth via an ARL cookie. Resolves a track
//! to a CDN URL, downloads the encrypted file, and decrypts it (crypto.rs).
//!
//! STATUS: implemented against Deezer's documented request shapes but NOT yet
//! validated end-to-end (needs a real ARL). The crypto half is unit-tested.

use serde_json::{json, Value};

use super::crypto;

const GW_LIGHT: &str = "https://www.deezer.com/ajax/gw-light.php";
const MEDIA_URL: &str = "https://media.deezer.com/v1/get_url";

#[derive(Debug, Clone)]
pub struct DeezerSession {
    pub user_id: String,       // Deezer account id
    pub api_token: String,     // gw "checkForm" csrf token
    pub license_token: String, // for media/get_url
    pub cookie: String,        // "arl=...; sid=..."
}

pub struct DeezerClient {
    http: reqwest::Client,
    arl: String,
}

/// A resolved, decrypted track ready to decode.
pub struct DecryptedTrack {
    pub track_id: String,
    pub format: String, // MP3_320 | MP3_128 | FLAC
    pub data: Vec<u8>,  // decrypted MP3/FLAC bytes
}

impl DeezerClient {
    pub fn new(http: reqwest::Client, arl: String) -> Self {
        DeezerClient { http, arl }
    }

    /// Authenticate with the ARL: fetch the api_token + license_token and the
    /// session cookie needed for subsequent gw calls.
    pub async fn login(&self) -> anyhow::Result<DeezerSession> {
        let arl_cookie = format!("arl={}", self.arl);
        let resp = self
            .http
            .post(GW_LIGHT)
            .query(&[
                ("method", "deezer.getUserData"),
                ("input", "3"),
                ("api_version", "1.0"),
                ("api_token", ""),
            ])
            .header("Cookie", &arl_cookie)
            .header("Content-Type", "application/json")
            .body("{}")
            .send()
            .await?;

        // capture sid from Set-Cookie
        let sid = resp
            .headers()
            .get_all("set-cookie")
            .iter()
            .filter_map(|h| h.to_str().ok())
            .find_map(|c| c.split(';').next().and_then(|kv| kv.strip_prefix("sid=")))
            .map(|s| s.to_string());

        let v: Value = resp.json().await?;
        let results = v.get("results").ok_or_else(|| anyhow::anyhow!("no results"))?;
        let api_token = results
            .get("checkForm")
            .and_then(|x| x.as_str())
            .ok_or_else(|| anyhow::anyhow!("no api_token (bad/expired ARL?)"))?
            .to_string();
        let license_token = results
            .get("USER")
            .and_then(|u| u.get("OPTIONS"))
            .and_then(|o| o.get("license_token"))
            .and_then(|x| x.as_str())
            .ok_or_else(|| anyhow::anyhow!("no license_token (not Premium?)"))?
            .to_string();

        let user_id = results
            .get("USER")
            .and_then(|u| u.get("USER_ID"))
            .map(|x| match x {
                Value::Number(n) => n.to_string(),
                Value::String(s) => s.clone(),
                _ => String::new(),
            })
            .filter(|s| !s.is_empty() && s != "0")
            .ok_or_else(|| anyhow::anyhow!("not logged in (bad/expired ARL?)"))?;

        let cookie = match sid {
            Some(s) => format!("{}; sid={}", arl_cookie, s),
            None => arl_cookie,
        };
        Ok(DeezerSession {
            user_id,
            api_token,
            license_token,
            cookie,
        })
    }

    /// Call a gw-light API method (authenticated browse: favorites, playlists).
    pub async fn gw(
        &self,
        s: &DeezerSession,
        method: &str,
        body: Value,
    ) -> anyhow::Result<Value> {
        let resp = self
            .http
            .post(GW_LIGHT)
            .query(&[
                ("method", method),
                ("input", "3"),
                ("api_version", "1.0"),
                ("api_token", s.api_token.as_str()),
            ])
            .header("Cookie", &s.cookie)
            .json(&body)
            .send()
            .await?;
        Ok(resp.json::<Value>().await?)
    }

    pub fn http(&self) -> &reqwest::Client {
        &self.http
    }

    /// Get the per-track token required by the media endpoint.
    async fn track_token(&self, s: &DeezerSession, track_id: &str) -> anyhow::Result<String> {
        let resp = self
            .http
            .post(GW_LIGHT)
            .query(&[
                ("method", "song.getData"),
                ("input", "3"),
                ("api_version", "1.0"),
                ("api_token", s.api_token.as_str()),
            ])
            .header("Cookie", &s.cookie)
            .json(&json!({ "sng_id": track_id }))
            .send()
            .await?;
        let v: Value = resp.json().await?;
        v.get("results")
            .and_then(|r| r.get("TRACK_TOKEN"))
            .and_then(|x| x.as_str())
            .map(|s| s.to_string())
            .ok_or_else(|| anyhow::anyhow!("no TRACK_TOKEN for {track_id}"))
    }

    /// Resolve a CDN URL for the track at the best available format.
    async fn media_url(
        &self,
        s: &DeezerSession,
        track_token: &str,
    ) -> anyhow::Result<(String, String)> {
        // Prefer 320, fall back to 128 (FLAC needs a HiFi sub).
        let body = json!({
            "license_token": s.license_token,
            "media": [{
                "type": "FULL",
                "formats": [
                    {"cipher": "BF_CBC_STRIPE", "format": "MP3_320"},
                    {"cipher": "BF_CBC_STRIPE", "format": "MP3_128"}
                ]
            }],
            "track_tokens": [track_token]
        });
        let resp = self.http.post(MEDIA_URL).json(&body).send().await?;
        let v: Value = resp.json().await?;
        let media = v
            .get("data")
            .and_then(|d| d.get(0))
            .and_then(|d| d.get("media"))
            .and_then(|m| m.get(0))
            .ok_or_else(|| anyhow::anyhow!("no media (token expired / geo-blocked?)"))?;
        let format = media
            .get("format")
            .and_then(|x| x.as_str())
            .unwrap_or("MP3_128")
            .to_string();
        let url = media
            .get("sources")
            .and_then(|s| s.get(0))
            .and_then(|s| s.get("url"))
            .and_then(|x| x.as_str())
            .ok_or_else(|| anyhow::anyhow!("no source url"))?
            .to_string();
        Ok((url, format))
    }

    /// Full path: resolve + download + decrypt a track id.
    pub async fn fetch_track(
        &self,
        s: &DeezerSession,
        track_id: &str,
    ) -> anyhow::Result<DecryptedTrack> {
        let token = self.track_token(s, track_id).await?;
        let (url, format) = self.media_url(s, &token).await?;
        let encrypted = self.http.get(&url).send().await?.bytes().await?;
        let data = crypto::decrypt_track(track_id, &encrypted);
        Ok(DecryptedTrack {
            track_id: track_id.to_string(),
            format,
            data,
        })
    }
}
