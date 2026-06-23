//! Browse/search endpoints. With feature "deezer" they're backed by Deezer
//! (public REST for search/tracks, gw-light for the user's favorites/playlists);
//! otherwise by the Spotify Web API proxy. Both return /proto models, so the
//! client is identical either way.

use axum::{
    extract::{Path, Query, State},
    Json,
};
use serde::Deserialize;
use serde_json::{json, Value};

use crate::api::AuthUser;
use crate::error::ApiResult;
use crate::state::AppState;

#[cfg(not(feature = "deezer"))]
use crate::proxy;

#[derive(Deserialize)]
pub struct SearchQuery {
    q: String,
    #[serde(default = "default_types")]
    r#type: String,
    #[serde(default = "default_limit")]
    limit: u32,
    #[serde(default)]
    offset: u32,
}
fn default_types() -> String {
    "track,album,artist,playlist".into()
}
fn default_limit() -> u32 {
    20
}

#[derive(Deserialize)]
pub struct Page {
    #[serde(default = "default_limit")]
    limit: u32,
    #[serde(default)]
    offset: u32,
}

// ---- Deezer login helper (per-user ARL) ----
#[cfg(feature = "deezer")]
async fn deezer_session(
    state: &AppState,
    uid: &str,
) -> ApiResult<(crate::deezer::DeezerClient, crate::deezer::client::DeezerSession)> {
    use crate::error::ApiError;
    let rec = state.store.get_user(uid).ok_or(ApiError::Unauthorized)?;
    let arl = state
        .cipher
        .open(&rec.enc_refresh_token)
        .map_err(|_| ApiError::Internal("arl decrypt".into()))?;
    let client = crate::deezer::DeezerClient::new(state.http.clone(), arl);
    let session = client.login().await.map_err(|e| ApiError::Upstream(e.to_string()))?;
    Ok((client, session))
}

pub async fn search(
    State(state): State<AppState>,
    AuthUser(_uid): AuthUser,
    Query(q): Query<SearchQuery>,
) -> ApiResult<Json<Value>> {
    #[cfg(feature = "deezer")]
    {
        let r = crate::deezer::proxy::search(&state.http, &q.q)
            .await
            .map_err(|e| crate::error::ApiError::Upstream(e.to_string()))?;
        return Ok(Json(serde_json::to_value(r).unwrap()));
    }
    #[cfg(not(feature = "deezer"))]
    {
        let r = proxy::search(&state, &_uid, &q.q, &q.r#type, q.limit.min(50), q.offset).await?;
        Ok(Json(serde_json::to_value(r).unwrap()))
    }
}

pub async fn playlists(
    State(state): State<AppState>,
    AuthUser(uid): AuthUser,
    Query(_p): Query<Page>,
) -> ApiResult<Json<Value>> {
    #[cfg(feature = "deezer")]
    {
        let (client, s) = deezer_session(&state, &uid).await?;
        let items = crate::deezer::proxy::playlists(&client, &s)
            .await
            .map_err(|e| crate::error::ApiError::Upstream(e.to_string()))?;
        let total = items.len();
        return Ok(Json(json!({ "items": items, "total": total })));
    }
    #[cfg(not(feature = "deezer"))]
    {
        let (items, total) = proxy::playlists(&state, &uid, _p.limit, _p.offset).await?;
        Ok(Json(json!({ "items": items, "total": total })))
    }
}

pub async fn playlist(
    State(state): State<AppState>,
    AuthUser(_uid): AuthUser,
    Path(id): Path<String>,
) -> ApiResult<Json<Value>> {
    #[cfg(feature = "deezer")]
    {
        let (client, s) = deezer_session(&state, &_uid).await?;
        let tracks = crate::deezer::proxy::playlist_tracks(&client, &s, &id)
            .await
            .map_err(|e| crate::error::ApiError::Upstream(e.to_string()))?;
        return Ok(Json(json!({ "tracks": tracks })));
    }
    #[cfg(not(feature = "deezer"))]
    {
        let (playlist, tracks) = proxy::playlist_tracks(&state, &_uid, &id).await?;
        Ok(Json(json!({ "playlist": playlist, "tracks": tracks })))
    }
}

pub async fn album(
    State(state): State<AppState>,
    AuthUser(_uid): AuthUser,
    Path(id): Path<String>,
) -> ApiResult<Json<Value>> {
    #[cfg(feature = "deezer")]
    {
        let tracks = crate::deezer::proxy::album_tracks(&state.http, &id)
            .await
            .map_err(|e| crate::error::ApiError::Upstream(e.to_string()))?;
        return Ok(Json(json!({ "tracks": tracks })));
    }
    #[cfg(not(feature = "deezer"))]
    {
        let (album, tracks) = proxy::album(&state, &_uid, &id).await?;
        Ok(Json(json!({ "album": album, "tracks": tracks })))
    }
}

pub async fn artist(
    State(state): State<AppState>,
    AuthUser(_uid): AuthUser,
    Path(id): Path<String>,
) -> ApiResult<Json<Value>> {
    #[cfg(feature = "deezer")]
    {
        let _ = (&state, &id);
        return Ok(Json(json!({ "top_tracks": [], "albums": [] })));
    }
    #[cfg(not(feature = "deezer"))]
    {
        let (artist, top_tracks, albums) = proxy::artist(&state, &_uid, &id).await?;
        Ok(Json(json!({ "artist": artist, "top_tracks": top_tracks, "albums": albums })))
    }
}

pub async fn favorites(
    State(state): State<AppState>,
    AuthUser(uid): AuthUser,
) -> ApiResult<Json<Value>> {
    #[cfg(feature = "deezer")]
    {
        let (client, s) = deezer_session(&state, &uid).await?;
        let tracks = crate::deezer::proxy::favorites(&client, &s)
            .await
            .map_err(|e| crate::error::ApiError::Upstream(e.to_string()))?;
        return Ok(Json(json!({ "tracks": tracks })));
    }
    #[cfg(not(feature = "deezer"))]
    {
        let tracks = proxy::favorites(&state, &uid).await?;
        Ok(Json(json!({ "tracks": tracks })))
    }
}
