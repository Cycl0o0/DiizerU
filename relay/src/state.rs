//! Shared application state handed to every axum handler.

use std::sync::atomic::AtomicBool;
use std::sync::Arc;

use crate::auth::pairing::PairingManager;
use crate::config::Config;
use crate::crypto::Cipher;
use crate::session::SessionManager;
use crate::store::Store;

#[derive(Clone)]
pub struct AppState {
    pub cfg: Arc<Config>,
    pub store: Arc<Store>,
    pub cipher: Arc<Cipher>,
    pub http: reqwest::Client,
    pub pairing: Arc<PairingManager>,
    pub sessions: SessionManager,
    /// When true: refuse new sessions, tear down existing (kill switch).
    pub killswitch: Arc<AtomicBool>,
}

impl AppState {
    pub fn killswitch_on(&self) -> bool {
        self.killswitch.load(std::sync::atomic::Ordering::Relaxed)
    }
}
