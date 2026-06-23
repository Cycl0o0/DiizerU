//! DiizerU relay entrypoint.
//!
//! Boot order: load env/config -> build state -> start GC ticker -> serve axum.
//! See ARCHITECTURE.md / SECURITY.md.

mod api;
mod audio;
mod auth;
mod config;
mod crypto;
#[cfg(feature = "deezer")]
mod deezer;
mod error;
mod model;
mod proxy;
mod session;
mod state;
mod store;

use std::sync::atomic::AtomicBool;
use std::sync::Arc;
use std::time::Duration;

use crate::config::Config;
use crate::crypto::Cipher;
use crate::state::AppState;

/// Seconds since the Unix epoch (UTC).
pub fn now_epoch() -> i64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

/// RFC3339 timestamp for API responses.
pub fn iso8601(epoch: i64) -> String {
    time::OffsetDateTime::from_unix_timestamp(epoch)
        .ok()
        .and_then(|t| t.format(&time::format_description::well_known::Rfc3339).ok())
        .unwrap_or_default()
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    dotenvy::dotenv().ok();
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "info,diizeru_relay=debug".into()),
        )
        .init();

    let cfg = Config::from_env()?;
    tracing::info!(mode = cfg.mode.as_str(), bind = %cfg.bind_addr, "starting DiizerU relay");

    // Kill switch can also be pre-armed by a KILLSWITCH file at boot.
    let killswitch = Arc::new(AtomicBool::new(std::path::Path::new("KILLSWITCH").exists()));

    let store_path = std::env::var("STORE_PATH").ok().map(std::path::PathBuf::from);
    let state = AppState {
        cfg: Arc::new(cfg.clone()),
        store: Arc::new(store::Store::new(store_path)),
        cipher: Arc::new(Cipher::new(&cfg.master_key)),
        http: reqwest::Client::builder()
            .user_agent("diizeru-relay/0.1")
            .timeout(Duration::from_secs(20))
            .build()?,
        pairing: Arc::new(auth::pairing::PairingManager::new()),
        sessions: session::SessionManager::new(
            cfg.max_concurrent_sessions,
            cfg.session_idle_timeout.as_secs() as i64,
        ),
        token_cache: Arc::new(proxy::TokenCache::new()),
        killswitch,
    };

    // DEV ONLY: seed an allowed user + relay session token so the PCM pipeline
    // is curl-testable before OAuth is wired to a real Spotify app. Set
    // DEV_SEED_TOKEN=<token> in .env; NEVER set this in production.
    if let Ok(tok) = std::env::var("DEV_SEED_TOKEN") {
        let uid = "dev-user";
        state.store.allow_user(uid);
        state.store.create_relay_session(store::RelaySession {
            token: tok.clone(),
            user_id: uid.to_string(),
            created_at: now_epoch(),
        });
        tracing::warn!("DEV_SEED_TOKEN active — bearer '{tok}' -> {uid} (tone audio). DO NOT use in prod.");
    }

    // Background GC: reap idle sessions + expired pairings.
    {
        let st = state.clone();
        tokio::spawn(async move {
            let mut tick = tokio::time::interval(Duration::from_secs(30));
            loop {
                tick.tick().await;
                let now = now_epoch();
                for uid in st.sessions.gc(now) {
                    tracing::info!(user = %uid, "session idle-timed out");
                }
                st.pairing.gc(now);
            }
        });
    }

    let app = api::router(state.clone());
    let listener = tokio::net::TcpListener::bind(&cfg.bind_addr).await?;
    tracing::info!("listening on http://{}", cfg.bind_addr);
    axum::serve(listener, app).await?;
    Ok(())
}
