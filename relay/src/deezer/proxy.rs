//! Deezer-backed browse, mapped to /proto models (client is unchanged).
//! - search / album / playlist tracks: public REST api.deezer.com (no auth).
//! - favorites / user playlists: gw-light API (needs the user's ARL session).

use serde_json::{json, Value};

use super::client::{DeezerClient, DeezerSession};
use crate::model::{Album, Artist, Image, Playlist, SearchResults, Track};

const REST: &str = "https://api.deezer.com";

fn num(v: &Value, k: &str) -> String {
    match v.get(k) {
        Some(Value::Number(n)) => n.to_string(),
        Some(Value::String(s)) => s.clone(),
        _ => String::new(),
    }
}
fn str_(v: &Value, k: &str) -> String {
    v.get(k).and_then(|x| x.as_str()).unwrap_or("").to_string()
}
fn img(url: String) -> Vec<Image> {
    if url.is_empty() {
        vec![]
    } else {
        vec![Image { url, width: None, height: None }]
    }
}
/// Deezer gw artwork md5 -> cover URL.
fn gw_cover(md5: &str) -> String {
    if md5.is_empty() {
        String::new()
    } else {
        format!("https://e-cdns-images.dzcdn.net/images/cover/{md5}/250x250-000000-80-0-0.jpg")
    }
}

// ---- REST mappers ----
fn rest_track(v: &Value) -> Track {
    let id = num(v, "id");
    let album = v.get("album");
    Track {
        id: id.clone(),
        uri: format!("deezer:track:{id}"),
        name: str_(v, "title"),
        duration_ms: v.get("duration").and_then(|x| x.as_u64()).unwrap_or(0) * 1000,
        artists: vec![Artist {
            id: v.get("artist").map(|a| num(a, "id")).unwrap_or_default(),
            name: v.get("artist").map(|a| str_(a, "name")).unwrap_or_default(),
            images: vec![],
        }],
        album: album.map(|a| Album {
            id: num(a, "id"),
            name: str_(a, "title"),
            artists: vec![],
            images: img(str_(a, "cover_medium")),
        }),
    }
}

async fn rest_get(http: &reqwest::Client, path: &str) -> anyhow::Result<Value> {
    Ok(http.get(format!("{REST}{path}")).send().await?.json::<Value>().await?)
}

pub async fn search(http: &reqwest::Client, q: &str) -> anyhow::Result<SearchResults> {
    let enc = urlencoding::encode(q);
    let mut sr = SearchResults::default();
    if let Ok(v) = rest_get(http, &format!("/search?q={enc}&limit=40")).await {
        if let Some(arr) = v.get("data").and_then(|x| x.as_array()) {
            sr.tracks = arr.iter().map(rest_track).collect();
        }
    }
    if let Ok(v) = rest_get(http, &format!("/search/album?q={enc}&limit=20")).await {
        if let Some(arr) = v.get("data").and_then(|x| x.as_array()) {
            sr.albums = arr
                .iter()
                .map(|a| Album {
                    id: num(a, "id"),
                    name: str_(a, "title"),
                    artists: a.get("artist").map(|ar| vec![Artist { id: num(ar, "id"), name: str_(ar, "name"), images: vec![] }]).unwrap_or_default(),
                    images: img(str_(a, "cover_medium")),
                })
                .collect();
        }
    }
    if let Ok(v) = rest_get(http, &format!("/search/playlist?q={enc}&limit=20")).await {
        if let Some(arr) = v.get("data").and_then(|x| x.as_array()) {
            sr.playlists = arr
                .iter()
                .map(|p| Playlist {
                    id: num(p, "id"),
                    name: str_(p, "title"),
                    owner: p.get("user").map(|u| str_(u, "name")).unwrap_or_default(),
                    track_count: p.get("nb_tracks").and_then(|x| x.as_u64()).unwrap_or(0) as u32,
                    images: img(str_(p, "picture_medium")),
                })
                .collect();
        }
    }
    Ok(sr)
}

pub async fn album_tracks(http: &reqwest::Client, id: &str) -> anyhow::Result<Vec<Track>> {
    let v = rest_get(http, &format!("/album/{id}/tracks?limit=100")).await?;
    Ok(v.get("data")
        .and_then(|x| x.as_array())
        .map(|a| a.iter().map(rest_track).collect())
        .unwrap_or_default())
}

/// Playlist tracks via gw (works for the user's PRIVATE playlists; public REST
/// returns empty for those).
pub async fn playlist_tracks(
    client: &DeezerClient,
    s: &DeezerSession,
    id: &str,
) -> anyhow::Result<Vec<Track>> {
    let v = client
        .gw(s, "playlist.getSongs", json!({"playlist_id": id, "nb": 500, "start": 0}))
        .await?;
    Ok(v.get("results")
        .and_then(|r| r.get("data"))
        .and_then(|x| x.as_array())
        .map(|a| a.iter().map(gw_track).collect())
        .unwrap_or_default())
}

// ---- gw (authenticated) mappers ----
fn gw_track(v: &Value) -> Track {
    let id = num(v, "SNG_ID");
    Track {
        id: id.clone(),
        uri: format!("deezer:track:{id}"),
        name: str_(v, "SNG_TITLE"),
        duration_ms: v
            .get("DURATION")
            .and_then(|x| x.as_str().and_then(|s| s.parse::<u64>().ok()).or_else(|| x.as_u64()))
            .unwrap_or(0)
            * 1000,
        artists: vec![Artist { id: num(v, "ART_ID"), name: str_(v, "ART_NAME"), images: vec![] }],
        album: Some(Album {
            id: num(v, "ALB_ID"),
            name: str_(v, "ALB_TITLE"),
            artists: vec![],
            images: img(gw_cover(&str_(v, "ALB_PICTURE"))),
        }),
    }
}

pub async fn favorites(client: &DeezerClient, s: &DeezerSession) -> anyhow::Result<Vec<Track>> {
    let v = client
        .gw(s, "favorite_song.getList", json!({"user_id": s.user_id, "nb": 100, "start": 0}))
        .await?;
    Ok(v.get("results")
        .and_then(|r| r.get("data"))
        .and_then(|x| x.as_array())
        .map(|a| a.iter().map(gw_track).collect())
        .unwrap_or_default())
}

pub async fn playlists(client: &DeezerClient, s: &DeezerSession) -> anyhow::Result<Vec<Playlist>> {
    let v = client
        .gw(s, "deezer.pageProfile", json!({"user_id": s.user_id, "tab": "playlists", "nb": 100}))
        .await?;
    let arr = v
        .get("results")
        .and_then(|r| r.get("TAB"))
        .and_then(|t| t.get("playlists"))
        .and_then(|p| p.get("data"))
        .and_then(|x| x.as_array())
        .cloned()
        .unwrap_or_default();
    Ok(arr
        .iter()
        .map(|p| Playlist {
            id: num(p, "PLAYLIST_ID"),
            name: str_(p, "TITLE"),
            owner: String::new(),
            track_count: p
                .get("NB_SONG")
                .and_then(|x| x.as_u64().or_else(|| x.as_str().and_then(|s| s.parse().ok())))
                .unwrap_or(0) as u32,
            images: img(gw_cover(&str_(p, "PLAYLIST_PICTURE"))),
        })
        .collect())
}
