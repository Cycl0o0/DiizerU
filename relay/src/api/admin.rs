//! Admin endpoints (separate bearer credential, see SECURITY.md): immediate
//! user revocation and the global kill switch. Onboarding is open (no invites).

use std::sync::atomic::Ordering;

use axum::{
    extract::{Path, State},
    http::StatusCode,
    Json,
};
use serde::Deserialize;

use axum::{
    extract::Query,
    http::header,
    response::{IntoResponse, Response},
};

use crate::api::AdminAuth;
use crate::state::AppState;

/// Revoke a user: delete tokens, kill live session, invalidate relay tokens.
pub async fn revoke(
    _a: AdminAuth,
    State(state): State<AppState>,
    Path(user_id): Path<String>,
) -> StatusCode {
    let _sealed = state.store.revoke_user(&user_id); // token dropped from store
    state.sessions.destroy(&user_id);
    tracing::warn!(user = %user_id, "user revoked (token deleted, session killed)");
    StatusCode::NO_CONTENT
}

#[derive(Deserialize)]
pub struct KillReq {
    #[serde(default = "default_true")]
    enabled: bool,
}
fn default_true() -> bool {
    true
}

pub async fn killswitch(
    _a: AdminAuth,
    State(state): State<AppState>,
    body: Option<Json<KillReq>>,
) -> StatusCode {
    let enabled = body.map(|b| b.0.enabled).unwrap_or(true);
    state.killswitch.store(enabled, Ordering::Relaxed);
    if enabled {
        state.sessions.destroy_all();
        tracing::error!("KILL SWITCH ENGAGED — all sessions torn down, new ones refused");
    } else {
        tracing::warn!("kill switch released");
    }
    StatusCode::NO_CONTENT
}

// ---- Deezer test (feature = "deezer") ----
// GET /v1/admin/deezer-test?track=<id>  (admin bearer)
// Proves the full chain: ARL login -> resolve -> download -> Blowfish decrypt ->
// decode -> raw PCM s16le. ARL from env DEEZER_ARL.
//   ffplay -f s16le -ar <X-Sample-Rate> -ch_layout stereo out.raw
#[derive(Deserialize)]
pub struct DeezerTestQ {
    track: String,
}

pub async fn deezer_test(
    _a: AdminAuth,
    State(state): State<AppState>,
    Query(q): Query<DeezerTestQ>,
) -> Response {
    let arl = match std::env::var("DEEZER_ARL") {
        Ok(a) if !a.is_empty() => a,
        _ => return (StatusCode::BAD_REQUEST, "DEEZER_ARL not set").into_response(),
    };
    let client = crate::deezer::DeezerClient::new(state.http.clone(), arl);
    let session = match client.login().await {
        Ok(s) => s,
        Err(e) => return (StatusCode::BAD_GATEWAY, format!("deezer login: {e}")).into_response(),
    };
    let track = match client.fetch_track(&session, &q.track).await {
        Ok(t) => t,
        Err(e) => return (StatusCode::BAD_GATEWAY, format!("fetch: {e}")).into_response(),
    };
    let fmt = track.format.clone();
    let (pcm, rate) = match crate::deezer::decode::decode_to_pcm(track.data) {
        Ok(v) => v,
        Err(e) => return (StatusCode::BAD_GATEWAY, format!("decode: {e}")).into_response(),
    };
    let mut out = Vec::with_capacity(pcm.len() * 2);
    for &x in &pcm {
        let v = (x.clamp(-1.0, 1.0) * i16::MAX as f32) as i16;
        out.extend_from_slice(&v.to_le_bytes());
    }
    tracing::info!(track = %q.track, %fmt, rate, samples = pcm.len(), "deezer test decoded");
    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "application/octet-stream")
        .header("X-Sample-Rate", rate.to_string())
        .header("X-Channels", "2")
        .header("X-Deezer-Format", fmt)
        .body(axum::body::Body::from(out))
        .unwrap()
}
