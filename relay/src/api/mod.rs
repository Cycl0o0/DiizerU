//! axum router assembly + the bearer-auth extractor.

pub mod admin;
pub mod browse;
pub mod meta;
pub mod pairing;
pub mod playback;
pub mod stream;

use axum::{
    async_trait,
    extract::FromRequestParts,
    http::request::Parts,
    routing::{get, post},
    Router,
};
use tower_http::cors::CorsLayer;
use tower_http::trace::TraceLayer;

use crate::error::ApiError;
use crate::state::AppState;

/// Extractor: validates `Authorization: Bearer <relay_session_token>` and
/// yields the authenticated user id. NOT a Deezer/ARL token.
pub struct AuthUser(pub String);

#[async_trait]
impl FromRequestParts<AppState> for AuthUser {
    type Rejection = ApiError;

    async fn from_request_parts(parts: &mut Parts, state: &AppState) -> Result<Self, Self::Rejection> {
        let token = parts
            .headers
            .get(axum::http::header::AUTHORIZATION)
            .and_then(|h| h.to_str().ok())
            .and_then(|s| s.strip_prefix("Bearer "))
            .ok_or(ApiError::Unauthorized)?;
        let user_id = state.store.user_for_token(token).ok_or(ApiError::Unauthorized)?;
        Ok(AuthUser(user_id))
    }
}

/// Extractor for the separate admin bearer credential.
pub struct AdminAuth;

#[async_trait]
impl FromRequestParts<AppState> for AdminAuth {
    type Rejection = ApiError;

    async fn from_request_parts(parts: &mut Parts, state: &AppState) -> Result<Self, Self::Rejection> {
        let token = parts
            .headers
            .get(axum::http::header::AUTHORIZATION)
            .and_then(|h| h.to_str().ok())
            .and_then(|s| s.strip_prefix("Bearer "))
            .ok_or(ApiError::Unauthorized)?;
        // constant-time-ish compare
        if token.len() == state.cfg.admin_token.len()
            && token
                .bytes()
                .zip(state.cfg.admin_token.bytes())
                .fold(0u8, |acc, (a, b)| acc | (a ^ b))
                == 0
        {
            Ok(AdminAuth)
        } else {
            Err(ApiError::Forbidden("admin".into()))
        }
    }
}

pub fn router(state: AppState) -> Router {
    let v1 = Router::new()
        // meta
        .route("/capabilities", get(meta::capabilities))
        .route("/me", get(meta::me))
        // pairing (no bearer)
        .route("/pair/start", post(pairing::pair_start))
        .route("/pair/poll", post(pairing::pair_poll))
        .route("/pair", get(pairing::verify_page))
        .route("/pair/deezer", post(pairing::deezer_pair))
        // browse
        .route("/search", get(browse::search))
        .route("/browse/playlists", get(browse::playlists))
        .route("/browse/playlist/:id", get(browse::playlist))
        .route("/browse/album/:id", get(browse::album))
        .route("/browse/artist/:id", get(browse::artist))
        .route("/browse/favorites", get(browse::favorites))
        // playback + queue
        .route("/playback", get(playback::get_playback))
        .route("/playback/command", post(playback::command))
        .route("/queue", get(playback::get_queue).post(playback::queue_command))
        .route("/ws", get(playback::ws_upgrade))
        // stream
        .route("/stream", get(stream::stream))
        // admin
        .route("/admin/revoke/:user_id", post(admin::revoke))
        .route("/admin/killswitch", post(admin::killswitch))
        .route("/admin/deezer-test", get(admin::deezer_test));

    Router::new()
        .nest("/v1", v1)
        .route("/healthz", get(|| async { "ok" }))
        .layer(TraceLayer::new_for_http())
        .layer(CorsLayer::permissive())
        .with_state(state)
}
