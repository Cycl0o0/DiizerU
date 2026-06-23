//! Playback control, queue mutation, and the WS push channel.
//!
//! v1 maintains a coherent player state machine + queue per session. Audio in
//! the default build is the tone source; with feature "librespot" these commands
//! drive the real librespot Player (see audio::librespot_source outline). The
//! wire contract is identical either way.

use axum::{
    extract::{
        ws::{Message, WebSocket, WebSocketUpgrade},
        Query, State,
    },
    response::IntoResponse,
    Json,
};
use serde::Deserialize;
use serde_json::{json, Value};

use crate::api::AuthUser;
use crate::error::{ApiError, ApiResult};
use crate::model::*;
use crate::state::AppState;

fn session_or_busy(state: &AppState, uid: &str) -> ApiResult<std::sync::Arc<std::sync::Mutex<crate::session::PlayerSession>>> {
    if state.killswitch_on() {
        return Err(ApiError::Busy);
    }
    state
        .sessions
        .get_or_create(uid, crate::now_epoch())
        .ok_or(ApiError::Busy)
}

pub async fn get_playback(
    State(state): State<AppState>,
    AuthUser(uid): AuthUser,
) -> ApiResult<Json<PlaybackState>> {
    let s = session_or_busy(&state, &uid)?;
    let g = s.lock().unwrap();
    Ok(Json(g.playback.clone()))
}

pub async fn command(
    State(state): State<AppState>,
    AuthUser(uid): AuthUser,
    Json(cmd): Json<PlaybackCommand>,
) -> ApiResult<Json<PlaybackState>> {
    let s = session_or_busy(&state, &uid)?;
    #[cfg(feature = "librespot")]
    ensure_librespot(&state, &uid, &s).await?;

    // Deezer: on play_uri, fetch+decrypt+decode the track and swap it in as the
    // session's audio source (done before locking — it awaits network/decode).
    #[cfg(feature = "deezer")]
    if let PlaybackAction::PlayUri = cmd.action {
        if let Some(uri) = &cmd.uri {
            let id = deezer_track_id(uri);
            if !id.is_empty() {
                match fetch_deezer_pcm(&state, &uid, &id).await {
                    Ok(pcm) => {
                        let mut g = s.lock().unwrap();
                        g.source = Box::new(crate::deezer::DeezerSource::new(pcm));
                    }
                    Err(e) => tracing::warn!(track = %id, "deezer play failed: {e:?}"),
                }
            }
        }
    }

    let mut g = s.lock().unwrap();
    g.last_active = crate::now_epoch();
    apply_command(&mut g, &cmd);
    #[cfg(feature = "librespot")]
    drive_librespot(&g, &cmd);
    let pb = g.playback.clone();
    Ok(Json(pb))
}

/// Extract a Deezer numeric track id from a uri ("deezer:track:12345" or "12345").
#[cfg(feature = "deezer")]
fn deezer_track_id(uri: &str) -> String {
    uri.rsplit(|c| c == ':' || c == '/')
        .next()
        .unwrap_or("")
        .chars()
        .filter(|c| c.is_ascii_digit())
        .collect()
}

/// Log in to Deezer with the user's stored ARL, download+decrypt+decode a track.
#[cfg(feature = "deezer")]
async fn fetch_deezer_pcm(state: &AppState, uid: &str, track_id: &str) -> ApiResult<Vec<f32>> {
    let rec = state.store.get_user(uid).ok_or(ApiError::Unauthorized)?;
    let arl = state
        .cipher
        .open(&rec.enc_refresh_token)
        .map_err(|_| ApiError::Internal("arl decrypt".into()))?;
    let client = crate::deezer::DeezerClient::new(state.http.clone(), arl);
    let sess = client.login().await.map_err(|e| ApiError::Upstream(e.to_string()))?;
    let track = client
        .fetch_track(&sess, track_id)
        .await
        .map_err(|e| ApiError::Upstream(e.to_string()))?;
    let (pcm, _rate) = crate::deezer::decode::decode_to_pcm(track.data)
        .map_err(|e| ApiError::Upstream(e.to_string()))?;
    tracing::info!(track = %track_id, samples = pcm.len(), "deezer track ready");
    Ok(pcm)
}

/// Lazily connect a real librespot player for this user and swap the session's
/// audio source to the ring buffer it feeds. Idempotent.
#[cfg(feature = "librespot")]
pub(crate) async fn ensure_librespot(
    state: &AppState,
    uid: &str,
    s: &std::sync::Arc<std::sync::Mutex<crate::session::PlayerSession>>,
) -> ApiResult<()> {
    if s.lock().unwrap().has_librespot() {
        return Ok(());
    }
    let token = crate::proxy::access_token(state, uid).await?;
    // Per-user librespot credentials cache (cached creds can fetch audio keys).
    let cache_dir = std::env::var("LIBRESPOT_CACHE_DIR")
        .ok()
        .map(|b| std::path::Path::new(&b).join(uid));
    let lp = crate::audio::librespot_source::LibrespotPlayer::connect(&token, cache_dir)
        .await
        .map_err(|e| ApiError::Upstream(e.to_string()))?;
    let src = lp.source();
    {
        let mut g = s.lock().unwrap();
        if g.has_librespot() {
            return Ok(()); // raced; another caller attached
        }
        g.source = Box::new(src);
        g.librespot = Some(lp);
    }

    // Until the browse/queue UI exists (M6), auto-start a track so the console
    // produces real audio on play. Prefer the user's first liked track; fall
    // back to a default URI if the library scope isn't granted (librespot loads
    // by URI via its own session, no Web API scope needed).
    let track = match crate::proxy::favorites(state, uid).await {
        Ok(tracks) => tracks.into_iter().find(|t| !t.uri.is_empty()),
        Err(_) => None,
    };
    let uri = track
        .as_ref()
        .map(|t| t.uri.clone())
        .unwrap_or_else(|| "spotify:track:0VjIjW4GlUZAMYd2vXMi3b".to_string()); // Blinding Lights
    let mut g = s.lock().unwrap();
    if let Some(lp) = g.librespot.as_ref() {
        let _ = lp.load_uri(&uri, true, 0);
    }
    if let Some(t) = track {
        g.queue.items = vec![t.clone()];
        g.queue.current_index = 0;
        g.playback.duration_ms = t.duration_ms;
        g.playback.track = Some(t);
    }
    g.playback.state = crate::model::PlayerState::Playing;
    Ok(())
}

/// Translate a control command into librespot player calls. Runs after
/// `apply_command` has updated our state machine, so `s.playback`/`s.queue`
/// already reflect the target state.
#[cfg(feature = "librespot")]
fn drive_librespot(s: &crate::session::PlayerSession, cmd: &PlaybackCommand) {
    use PlaybackAction::*;
    let Some(lp) = s.librespot.as_ref() else {
        return;
    };
    match cmd.action {
        Play => lp.play(),
        Pause => lp.pause(),
        Toggle => match s.playback.state {
            PlayerState::Playing => lp.play(),
            _ => lp.pause(),
        },
        Seek => {
            if let Some(p) = cmd.position_ms {
                lp.seek(p as u32);
            }
        }
        Next | Prev => {
            if let Some(t) = s.queue.items.get(s.queue.current_index) {
                let _ = lp.load_uri(&t.uri, true, 0);
            }
        }
        PlayUri => {
            let uri = cmd
                .uri
                .clone()
                .or_else(|| s.queue.items.get(s.queue.current_index).map(|t| t.uri.clone()));
            if let Some(u) = uri {
                let _ = lp.load_uri(&u, true, 0);
            }
        }
        SetRepeat | SetShuffle => {}
    }
}

fn apply_command(s: &mut crate::session::PlayerSession, cmd: &PlaybackCommand) {
    use PlaybackAction::*;
    match cmd.action {
        Play => s.playback.state = PlayerState::Playing,
        Pause => s.playback.state = PlayerState::Paused,
        Toggle => {
            s.playback.state = match s.playback.state {
                PlayerState::Playing => PlayerState::Paused,
                _ => PlayerState::Playing,
            }
        }
        Next => advance(s, 1),
        Prev => advance(s, -1),
        Seek => {
            if let Some(pos) = cmd.position_ms {
                s.playback.position_ms = pos;
                s.source.seek(pos);
            }
        }
        SetRepeat => {
            if let Some(r) = cmd.repeat {
                s.playback.repeat = r;
            }
        }
        SetShuffle => {
            if let Some(sh) = cmd.shuffle {
                s.playback.shuffle = sh;
            }
        }
        PlayUri => {
            // Load an explicit track list if provided; otherwise leave queue.
            if let Some(uris) = &cmd.context_uris {
                s.queue.items = uris
                    .iter()
                    .map(|u| Track {
                        id: u.rsplit(':').next().unwrap_or("").to_string(),
                        uri: u.clone(),
                        name: String::new(),
                        duration_ms: 0,
                        artists: vec![],
                        album: None,
                    })
                    .collect();
                s.queue.current_index = 0;
            }
            s.playback.state = PlayerState::Loading;
            set_current(s);
            s.playback.state = PlayerState::Playing;
        }
    }
}

fn advance(s: &mut crate::session::PlayerSession, delta: i64) {
    if s.queue.items.is_empty() {
        return;
    }
    let len = s.queue.items.len() as i64;
    let mut idx = s.queue.current_index as i64 + delta;
    match s.playback.repeat {
        RepeatMode::All => idx = (idx % len + len) % len,
        _ => idx = idx.clamp(0, len - 1),
    }
    s.queue.current_index = idx as usize;
    s.playback.position_ms = 0;
    set_current(s);
}

fn set_current(s: &mut crate::session::PlayerSession) {
    if let Some(t) = s.queue.items.get(s.queue.current_index) {
        s.playback.duration_ms = t.duration_ms;
        s.playback.track = Some(t.clone());
    }
}

pub async fn get_queue(
    State(state): State<AppState>,
    AuthUser(uid): AuthUser,
) -> ApiResult<Json<Queue>> {
    let s = session_or_busy(&state, &uid)?;
    let q = s.lock().unwrap().queue.clone();
    Ok(Json(q))
}

pub async fn queue_command(
    State(state): State<AppState>,
    AuthUser(uid): AuthUser,
    Json(cmd): Json<QueueCommand>,
) -> ApiResult<Json<Queue>> {
    let s = session_or_busy(&state, &uid)?;
    let mut g = s.lock().unwrap();
    let q = &mut g.queue;
    match cmd.op {
        QueueOp::Add => {
            if let Some(uri) = cmd.uri {
                q.items.push(Track {
                    id: uri.rsplit(':').next().unwrap_or("").to_string(),
                    uri,
                    name: String::new(),
                    duration_ms: 0,
                    artists: vec![],
                    album: None,
                });
            }
        }
        QueueOp::Remove => {
            if let Some(i) = cmd.index {
                if i < q.items.len() {
                    q.items.remove(i);
                    if q.current_index >= q.items.len() && q.current_index > 0 {
                        q.current_index -= 1;
                    }
                }
            }
        }
        QueueOp::Move => {
            if let (Some(from), Some(to)) = (cmd.from_index, cmd.to_index) {
                if from < q.items.len() && to < q.items.len() {
                    let it = q.items.remove(from);
                    q.items.insert(to, it);
                }
            }
        }
        QueueOp::Clear => {
            q.items.clear();
            q.current_index = 0;
        }
        QueueOp::Replace => {
            if let Some(uris) = cmd.uris {
                q.items = uris
                    .into_iter()
                    .map(|uri| Track {
                        id: uri.rsplit(':').next().unwrap_or("").to_string(),
                        uri,
                        name: String::new(),
                        duration_ms: 0,
                        artists: vec![],
                        album: None,
                    })
                    .collect();
                q.current_index = 0;
            }
        }
    }
    Ok(Json(g.queue.clone()))
}

#[derive(Deserialize)]
pub struct WsQuery {
    #[serde(default)]
    token: Option<String>,
}

/// WS upgrade. Auth via `?token=` (browsers can't set headers on WS) or bearer.
pub async fn ws_upgrade(
    State(state): State<AppState>,
    Query(q): Query<WsQuery>,
    ws: WebSocketUpgrade,
) -> impl IntoResponse {
    let uid = q.token.and_then(|t| state.store.user_for_token(&t));
    ws.on_upgrade(move |socket| handle_ws(socket, state, uid))
}

async fn handle_ws(mut socket: WebSocket, state: AppState, uid: Option<String>) {
    let uid = match uid {
        Some(u) => u,
        None => {
            let _ = socket
                .send(Message::Text(
                    json!({"type":"error","error":"unauthorized","fatal":true}).to_string(),
                ))
                .await;
            return;
        }
    };
    let mut tick = tokio::time::interval(std::time::Duration::from_millis(1000));
    loop {
        tokio::select! {
            _ = tick.tick() => {
                if state.killswitch_on() {
                    let _ = socket.send(Message::Text(
                        json!({"type":"session","event":"killswitch"}).to_string())).await;
                    break;
                }
                let frame: Value = match state.sessions.get(&uid) {
                    Some(s) => {
                        let g = s.lock().unwrap();
                        json!({"type":"playback","state": g.playback})
                    }
                    None => json!({"type":"session","event":"idle_timeout"}),
                };
                if socket.send(Message::Text(frame.to_string())).await.is_err() {
                    break;
                }
            }
            msg = socket.recv() => {
                match msg {
                    Some(Ok(Message::Close(_))) | None => break,
                    _ => {} // ignore client frames (control is via REST)
                }
            }
        }
    }
}
