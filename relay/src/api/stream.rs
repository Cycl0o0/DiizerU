//! GET /v1/stream — endless HTTP chunked audio for the active session.
//!
//! Pulls f32 from the session's AudioSource, encodes via the negotiated
//! StreamEncoder (v1: pcm_s16le), and paces to real time so the relay doesn't
//! flood the console. The Wii U feeds these bytes straight into its ring buffer
//! and AX/SDL2 backend — ZERO decode on the console.
//!
//! Bandwidth (pcm_s16le): 44100 * 2ch * 2bytes = 176_400 B/s ≈ 1.41 Mbit/s.

use std::time::Duration;

use axum::{
    body::Body,
    extract::{Query, State},
    http::{header, StatusCode},
    response::{IntoResponse, Response},
};
use bytes::Bytes;
use serde::Deserialize;

use crate::api::AuthUser;
use crate::audio::{encoder_for, CHANNELS, SAMPLE_RATE};
use crate::state::AppState;

#[derive(Deserialize)]
pub struct StreamQuery {
    #[serde(default)]
    fmt: Option<String>,
}

// 20 ms chunks: smooth pacing, small latency.
const CHUNK_MS: u64 = 20;

pub async fn stream(
    State(state): State<AppState>,
    AuthUser(uid): AuthUser,
    Query(q): Query<StreamQuery>,
) -> Response {
    if state.killswitch_on() {
        return (StatusCode::SERVICE_UNAVAILABLE, "busy").into_response();
    }
    let fmt = q.fmt.unwrap_or_else(|| "pcm_s16le".into());
    let mut encoder = match encoder_for(&fmt) {
        Some(e) => e,
        None => return (StatusCode::BAD_REQUEST, "unknown fmt").into_response(),
    };
    let session = match state.sessions.get_or_create(&uid, crate::now_epoch()) {
        Some(s) => s,
        None => return (StatusCode::SERVICE_UNAVAILABLE, "busy").into_response(),
    };

    let frames_per_chunk = (SAMPLE_RATE as u64 * CHUNK_MS / 1000) as usize;
    let samples_per_chunk = frames_per_chunk * CHANNELS as usize;
    let fmt_name = encoder.format_name().to_string();
    let out_rate = encoder.out_sample_rate();

    let body = Body::from_stream(async_stream::stream! {
        let mut scratch = vec![0f32; samples_per_chunk];
        // Pace to real time but allow running up to LEAD_MS ahead, so the client
        // can build and keep a cushion that survives network jitter. The client
        // caps its own buffer (TCP backpressure), so this never floods.
        const LEAD_MS: u64 = 3000;
        let start = std::time::Instant::now();
        let mut sent_ms: u64 = 0;
        loop {
            let elapsed = start.elapsed().as_millis() as u64;
            if sent_ms > elapsed + LEAD_MS {
                tokio::time::sleep(Duration::from_millis(sent_ms - elapsed - LEAD_MS)).await;
            }
            // Lock briefly: copy/encode samples, then release before next await.
            let bytes = {
                let mut g = session.lock().unwrap();
                g.last_active = crate::now_epoch();
                let n = g.source.read(&mut scratch);
                // underrun -> emit silence to keep the console's buffer fed
                for s in scratch.iter_mut().take(samples_per_chunk).skip(n) {
                    *s = 0.0;
                }
                let mut out = Vec::with_capacity(samples_per_chunk * 2);
                encoder.encode(&scratch, &mut out);
                out
            };
            yield Ok::<Bytes, std::io::Error>(Bytes::from(bytes));
            sent_ms += CHUNK_MS;
        }
    });

    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "application/octet-stream")
        .header("X-Sample-Rate", out_rate.to_string())
        .header("X-Channels", CHANNELS.to_string())
        .header("X-Audio-Format", fmt_name)
        .body(body)
        .unwrap()
}
