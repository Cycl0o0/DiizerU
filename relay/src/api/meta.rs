//! /capabilities and /me.

use axum::{extract::State, Json};
use serde_json::{json, Value};

use crate::api::AuthUser;
use crate::audio::{CHANNELS, SAMPLE_RATE};
use crate::error::ApiResult;
use crate::state::AppState;

pub async fn capabilities(State(state): State<AppState>) -> Json<Value> {
    Json(json!({
        "api_version": "1.0.0",
        "relay_mode": state.cfg.mode.as_str(),
        "audio_formats": ["pcm_s16le"],
        "sample_rate": SAMPLE_RATE,
        "channels": CHANNELS,
    }))
}

pub async fn me(State(state): State<AppState>, AuthUser(user_id): AuthUser) -> ApiResult<Json<Value>> {
    let rec = state
        .store
        .get_user(&user_id)
        .ok_or(crate::error::ApiError::Unauthorized)?;
    Ok(Json(json!({
        "user_id": rec.user_id,
        "display_name": rec.display_name,
        "product": rec.product,
    })))
}
