//! Browse/search endpoints, backed by Deezer (public REST for search/tracks,
//! gw-light for the user's favorites/playlists). All return /proto models.

use axum::{
    extract::{Path, Query, State},
    Json,
};
use serde::Deserialize;
use serde_json::{json, Value};

use crate::api::AuthUser;
use crate::error::{ApiError, ApiResult};
use crate::state::AppState;

#[derive(Deserialize)]
pub struct SearchQuery {
    q: String,
}

#[derive(Deserialize)]
pub struct Page {
    #[serde(default)]
    _offset: u32,
}

/// Log in to Deezer with the user's stored ARL.
async fn deezer_session(
    state: &AppState,
    uid: &str,
) -> ApiResult<(crate::deezer::DeezerClient, crate::deezer::client::DeezerSession)> {
    let rec = state.store.get_user(uid).ok_or(ApiError::Unauthorized)?;
    let arl = state
        .cipher
        .open(&rec.enc_refresh_token)
        .map_err(|_| ApiError::Internal("arl decrypt".into()))?;
    let client = crate::deezer::DeezerClient::new(state.http.clone(), arl);
    let session = client.login().await.map_err(|e| ApiError::Upstream(e.to_string()))?;
    Ok((client, session))
}

fn up<T>(r: anyhow::Result<T>) -> ApiResult<T> {
    r.map_err(|e| ApiError::Upstream(e.to_string()))
}

pub async fn search(
    State(state): State<AppState>,
    AuthUser(_uid): AuthUser,
    Query(q): Query<SearchQuery>,
) -> ApiResult<Json<Value>> {
    let r = up(crate::deezer::proxy::search(&state.http, &q.q).await)?;
    Ok(Json(serde_json::to_value(r).unwrap()))
}

pub async fn playlists(
    State(state): State<AppState>,
    AuthUser(uid): AuthUser,
    Query(_p): Query<Page>,
) -> ApiResult<Json<Value>> {
    let (client, s) = deezer_session(&state, &uid).await?;
    let items = up(crate::deezer::proxy::playlists(&client, &s).await)?;
    let total = items.len();
    Ok(Json(json!({ "items": items, "total": total })))
}

pub async fn playlist(
    State(state): State<AppState>,
    AuthUser(uid): AuthUser,
    Path(id): Path<String>,
) -> ApiResult<Json<Value>> {
    let (client, s) = deezer_session(&state, &uid).await?;
    let tracks = up(crate::deezer::proxy::playlist_tracks(&client, &s, &id).await)?;
    Ok(Json(json!({ "tracks": tracks })))
}

pub async fn album(
    State(state): State<AppState>,
    AuthUser(_uid): AuthUser,
    Path(id): Path<String>,
) -> ApiResult<Json<Value>> {
    let tracks = up(crate::deezer::proxy::album_tracks(&state.http, &id).await)?;
    Ok(Json(json!({ "tracks": tracks })))
}

pub async fn artist(
    State(_state): State<AppState>,
    AuthUser(_uid): AuthUser,
    Path(_id): Path<String>,
) -> ApiResult<Json<Value>> {
    Ok(Json(json!({ "top_tracks": [], "albums": [] })))
}

pub async fn favorites(
    State(state): State<AppState>,
    AuthUser(uid): AuthUser,
) -> ApiResult<Json<Value>> {
    let (client, s) = deezer_session(&state, &uid).await?;
    let tracks = up(crate::deezer::proxy::favorites(&client, &s).await)?;
    Ok(Json(json!({ "tracks": tracks })))
}
