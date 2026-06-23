//! Thin proxy over the Spotify Web API for search/browse. The user's Spotify
//! token NEVER leaves the relay — the console calls our endpoints, we call
//! Spotify with the server-side token and reshape into /proto DTOs.

use std::collections::HashMap;
use std::sync::Mutex;

use serde_json::Value;

use crate::auth::refresh_token;
use crate::error::{ApiError, ApiResult};
use crate::model::*;
use crate::state::AppState;

/// Short-lived access-token cache (user_id -> (token, expires_at_epoch)).
#[derive(Default)]
pub struct TokenCache {
    inner: Mutex<HashMap<String, (String, i64)>>,
}

impl TokenCache {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn evict(&self, user_id: &str) {
        self.inner.lock().unwrap().remove(user_id);
    }
}

/// Obtain a valid access token for a user, refreshing if needed.
pub async fn access_token(state: &AppState, user_id: &str) -> ApiResult<String> {
    let now = crate::now_epoch();
    if let Some((tok, exp)) = state.token_cache.inner.lock().unwrap().get(user_id) {
        if *exp - 30 > now {
            return Ok(tok.clone());
        }
    }
    let rec = state
        .store
        .get_user(user_id)
        .ok_or(ApiError::Unauthorized)?;
    let refresh = state
        .cipher
        .open(&rec.enc_refresh_token)
        .map_err(|_| ApiError::Internal("token decrypt".into()))?;
    let tr = refresh_token(
        &state.http,
        &state.cfg.spotify_client_id,
        state.cfg.spotify_client_secret.as_deref(),
        &refresh,
    )
    .await
    .map_err(|e| ApiError::Upstream(e.to_string()))?;
    tracing::info!(refreshed_scope = %tr.scope, "access token refreshed");
    // Spotify's keymaster client ROTATES refresh tokens: each refresh returns a
    // new one and invalidates the old. Persist it or the next refresh 400s.
    if let Some(new_rt) = &tr.refresh_token {
        if let (Ok(sealed), Some(mut rec)) = (state.cipher.seal(new_rt), state.store.get_user(user_id)) {
            rec.enc_refresh_token = sealed;
            state.store.upsert_user(rec);
        }
    }
    state
        .token_cache
        .inner
        .lock()
        .unwrap()
        .insert(user_id.to_string(), (tr.access_token.clone(), now + tr.expires_in));
    Ok(tr.access_token)
}

async fn get(state: &AppState, user_id: &str, url: &str) -> ApiResult<Value> {
    let tok = access_token(state, user_id).await?;
    let mut resp = state
        .http
        .get(url)
        .bearer_auth(&tok)
        .send()
        .await
        .map_err(|e| ApiError::Upstream(e.to_string()))?;
    // Retry on 429 honoring Retry-After (Spotify throttles, esp. keymaster client).
    let mut attempt = 0;
    while resp.status().as_u16() == 429 && attempt < 3 {
        let wait = resp
            .headers()
            .get("retry-after")
            .and_then(|v| v.to_str().ok())
            .and_then(|s| s.parse::<u64>().ok())
            .unwrap_or(2)
            .min(8);
        tokio::time::sleep(std::time::Duration::from_secs(wait + 1)).await;
        resp = state
            .http
            .get(url)
            .bearer_auth(&tok)
            .send()
            .await
            .map_err(|e| ApiError::Upstream(e.to_string()))?;
        attempt += 1;
    }
    if !resp.status().is_success() {
        return Err(ApiError::Upstream(format!("spotify {}", resp.status())));
    }
    resp.json::<Value>()
        .await
        .map_err(|e| ApiError::Upstream(e.to_string()))
}

// ---- mappers (Spotify JSON -> /proto DTO) ----

fn map_images(v: &Value) -> Vec<Image> {
    v.get("images")
        .and_then(|i| i.as_array())
        .map(|arr| {
            arr.iter()
                .filter_map(|im| {
                    Some(Image {
                        url: im.get("url")?.as_str()?.to_string(),
                        width: im.get("width").and_then(|x| x.as_u64()).map(|x| x as u32),
                        height: im.get("height").and_then(|x| x.as_u64()).map(|x| x as u32),
                    })
                })
                .collect()
        })
        .unwrap_or_default()
}

fn map_artist(v: &Value) -> Artist {
    Artist {
        id: v.get("id").and_then(|x| x.as_str()).unwrap_or("").to_string(),
        name: v.get("name").and_then(|x| x.as_str()).unwrap_or("").to_string(),
        images: map_images(v),
    }
}

fn map_album(v: &Value) -> Album {
    Album {
        id: v.get("id").and_then(|x| x.as_str()).unwrap_or("").to_string(),
        name: v.get("name").and_then(|x| x.as_str()).unwrap_or("").to_string(),
        artists: v
            .get("artists")
            .and_then(|a| a.as_array())
            .map(|a| a.iter().map(map_artist).collect())
            .unwrap_or_default(),
        images: map_images(v),
    }
}

fn map_track(v: &Value) -> Track {
    Track {
        id: v.get("id").and_then(|x| x.as_str()).unwrap_or("").to_string(),
        uri: v.get("uri").and_then(|x| x.as_str()).unwrap_or("").to_string(),
        name: v.get("name").and_then(|x| x.as_str()).unwrap_or("").to_string(),
        duration_ms: v.get("duration_ms").and_then(|x| x.as_u64()).unwrap_or(0),
        artists: v
            .get("artists")
            .and_then(|a| a.as_array())
            .map(|a| a.iter().map(map_artist).collect())
            .unwrap_or_default(),
        album: v.get("album").map(map_album),
    }
}

fn map_playlist(v: &Value) -> Playlist {
    Playlist {
        id: v.get("id").and_then(|x| x.as_str()).unwrap_or("").to_string(),
        name: v.get("name").and_then(|x| x.as_str()).unwrap_or("").to_string(),
        owner: v
            .get("owner")
            .and_then(|o| o.get("display_name").or_else(|| o.get("id")))
            .and_then(|x| x.as_str())
            .unwrap_or("")
            .to_string(),
        track_count: v
            .get("tracks")
            .and_then(|t| t.get("total"))
            .and_then(|x| x.as_u64())
            .unwrap_or(0) as u32,
        images: map_images(v),
    }
}

// ---- public proxy ops ----

pub async fn search(
    state: &AppState,
    user_id: &str,
    q: &str,
    types: &str,
    limit: u32,
    offset: u32,
) -> ApiResult<SearchResults> {
    let url = format!(
        "https://api.spotify.com/v1/search?q={}&type={}&limit={}&offset={}",
        urlencoding::encode(q),
        urlencoding::encode(types),
        limit,
        offset
    );
    let v = get(state, user_id, &url).await?;
    let arr = |key: &str| -> Vec<Value> {
        v.get(key)
            .and_then(|x| x.get("items"))
            .and_then(|x| x.as_array())
            .cloned()
            .unwrap_or_default()
    };
    Ok(SearchResults {
        tracks: arr("tracks").iter().map(map_track).collect(),
        albums: arr("albums").iter().map(map_album).collect(),
        artists: arr("artists").iter().map(map_artist).collect(),
        playlists: arr("playlists").iter().map(map_playlist).collect(),
    })
}

pub async fn playlists(state: &AppState, user_id: &str, limit: u32, offset: u32) -> ApiResult<(Vec<Playlist>, u64)> {
    let url = format!("https://api.spotify.com/v1/me/playlists?limit={limit}&offset={offset}");
    let v = get(state, user_id, &url).await?;
    let items = v
        .get("items")
        .and_then(|x| x.as_array())
        .map(|a| a.iter().map(map_playlist).collect())
        .unwrap_or_default();
    let total = v.get("total").and_then(|x| x.as_u64()).unwrap_or(0);
    Ok((items, total))
}

pub async fn playlist_tracks(state: &AppState, user_id: &str, id: &str) -> ApiResult<(Playlist, Vec<Track>)> {
    let meta = get(state, user_id, &format!("https://api.spotify.com/v1/playlists/{id}")).await?;
    let pl = map_playlist(&meta);
    let tracks = meta
        .get("tracks")
        .and_then(|t| t.get("items"))
        .and_then(|x| x.as_array())
        .map(|a| a.iter().filter_map(|it| it.get("track")).map(map_track).collect())
        .unwrap_or_default();
    Ok((pl, tracks))
}

pub async fn album(state: &AppState, user_id: &str, id: &str) -> ApiResult<(Album, Vec<Track>)> {
    let v = get(state, user_id, &format!("https://api.spotify.com/v1/albums/{id}")).await?;
    let al = map_album(&v);
    let tracks = v
        .get("tracks")
        .and_then(|t| t.get("items"))
        .and_then(|x| x.as_array())
        .map(|a| a.iter().map(map_track).collect())
        .unwrap_or_default();
    Ok((al, tracks))
}

pub async fn artist(state: &AppState, user_id: &str, id: &str) -> ApiResult<(Artist, Vec<Track>, Vec<Album>)> {
    let a = get(state, user_id, &format!("https://api.spotify.com/v1/artists/{id}")).await?;
    let top = get(
        state,
        user_id,
        &format!("https://api.spotify.com/v1/artists/{id}/top-tracks?market=from_token"),
    )
    .await?;
    let albums = get(
        state,
        user_id,
        &format!("https://api.spotify.com/v1/artists/{id}/albums?limit=50"),
    )
    .await?;
    Ok((
        map_artist(&a),
        top.get("tracks")
            .and_then(|x| x.as_array())
            .map(|x| x.iter().map(map_track).collect())
            .unwrap_or_default(),
        albums
            .get("items")
            .and_then(|x| x.as_array())
            .map(|x| x.iter().map(map_album).collect())
            .unwrap_or_default(),
    ))
}

pub async fn favorites(state: &AppState, user_id: &str) -> ApiResult<Vec<Track>> {
    let v = get(state, user_id, "https://api.spotify.com/v1/me/tracks?limit=50").await?;
    Ok(v.get("items")
        .and_then(|x| x.as_array())
        .map(|a| a.iter().filter_map(|it| it.get("track")).map(map_track).collect())
        .unwrap_or_default())
}
