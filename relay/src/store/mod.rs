//! Persistence: allowlist (for revocation), encrypted user tokens, relay sessions.
//!
//! Onboarding is open: any account with a valid Deezer ARL can pair. The
//! allowlist is retained only so an operator can revoke a user (mark Revoked,
//! which invalidates their relay tokens) — it is not a beta gate.
//!
//! v1 uses an in-memory store with optional JSON snapshot persistence. The
//! `Store` API is deliberately narrow so a SQLite/Postgres backend can replace
//! it later without touching callers. Refresh tokens are stored ONLY in sealed
//! (encrypted) form — see crypto.rs and SECURITY.md.

use std::collections::HashMap;
use std::sync::RwLock;

use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub enum AllowState {
    Allowed,
    Revoked,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct UserRecord {
    pub user_id: String,
    pub display_name: String,
    pub product: String,
    /// Encrypted refresh token (k1:nonce:ct). Never plaintext.
    pub enc_refresh_token: String,
    pub created_at: i64,
    pub state: AllowState,
}

/// A live relay session token -> user_id mapping (the console's bearer).
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct RelaySession {
    pub token: String,
    pub user_id: String,
    pub created_at: i64,
}

#[derive(Default, Serialize, Deserialize)]
struct Snapshot {
    allowlist: HashMap<String, AllowState>, // user_id -> state (for revocation)
    users: HashMap<String, UserRecord>,     // user_id -> record
    sessions: HashMap<String, RelaySession>,// relay token -> session
}

pub struct Store {
    inner: RwLock<Snapshot>,
    path: Option<std::path::PathBuf>,
}

impl Store {
    pub fn new(path: Option<std::path::PathBuf>) -> Self {
        let inner = path
            .as_ref()
            .and_then(|p| std::fs::read(p).ok())
            .and_then(|b| serde_json::from_slice::<Snapshot>(&b).ok())
            .unwrap_or_default();
        Store {
            inner: RwLock::new(inner),
            path,
        }
    }

    fn persist(&self, snap: &Snapshot) {
        if let Some(p) = &self.path {
            if let Ok(bytes) = serde_json::to_vec_pretty(snap) {
                let _ = std::fs::write(p, bytes); // best-effort; tokens already sealed
            }
        }
    }

    // ---- allowlist ----

    pub fn allow_user(&self, user_id: &str) {
        let mut g = self.inner.write().unwrap();
        g.allowlist.insert(user_id.to_string(), AllowState::Allowed);
        self.persist(&g);
    }

    // ---- users / tokens ----

    pub fn upsert_user(&self, rec: UserRecord) {
        let mut g = self.inner.write().unwrap();
        g.users.insert(rec.user_id.clone(), rec);
        self.persist(&g);
    }

    pub fn get_user(&self, user_id: &str) -> Option<UserRecord> {
        self.inner.read().unwrap().users.get(user_id).cloned()
    }

    // ---- relay sessions ----

    pub fn create_relay_session(&self, s: RelaySession) {
        let mut g = self.inner.write().unwrap();
        g.sessions.insert(s.token.clone(), s);
        self.persist(&g);
    }

    pub fn user_for_token(&self, token: &str) -> Option<String> {
        let g = self.inner.read().unwrap();
        g.sessions.get(token).and_then(|s| {
            // a token is only valid if the user is still Allowed
            match g.allowlist.get(&s.user_id) {
                Some(AllowState::Allowed) => Some(s.user_id.clone()),
                _ => None,
            }
        })
    }

    // ---- revocation ----

    /// Revoke a user: mark revoked, drop their record + all relay sessions.
    /// Returns the sealed token (if any) for the caller to handle.
    ///
    pub fn revoke_user(&self, user_id: &str) -> Option<String> {
        let mut g = self.inner.write().unwrap();
        g.allowlist.insert(user_id.to_string(), AllowState::Revoked);
        let sealed = g.users.remove(user_id).map(|u| u.enc_refresh_token);
        g.sessions.retain(|_, s| s.user_id != user_id);
        self.persist(&g);
        sealed
    }
}
