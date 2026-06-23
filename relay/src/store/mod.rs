//! Persistence: allowlist, invite codes, encrypted user tokens, relay sessions.
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

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct InviteCode {
    pub code: String,
    pub expires_at: i64,
    pub used_by: Option<String>,
    /// Public invite: usable by many users until it expires (single-use if false).
    #[serde(default)]
    pub multi_use: bool,
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
    allowlist: HashMap<String, AllowState>, // user_id -> state (explicit allowlist)
    invites: HashMap<String, InviteCode>,   // code -> invite
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

    pub fn is_allowed(&self, user_id: &str) -> bool {
        let g = self.inner.read().unwrap();
        matches!(g.allowlist.get(user_id), Some(AllowState::Allowed))
    }

    // ---- invite codes ----

    pub fn add_invite(&self, code: InviteCode) {
        let mut g = self.inner.write().unwrap();
        g.invites.insert(code.code.clone(), code);
        self.persist(&g);
    }

    /// Consume an invite code for a user. Returns true if valid+unused+unexpired.
    pub fn consume_invite(&self, code: &str, user_id: &str, now: i64) -> bool {
        let mut g = self.inner.write().unwrap();
        let ok = match g.invites.get_mut(code) {
            Some(inv) if inv.expires_at > now && (inv.multi_use || inv.used_by.is_none()) => {
                if !inv.multi_use {
                    inv.used_by = Some(user_id.to_string());
                }
                true
            }
            _ => false,
        };
        if ok {
            g.allowlist.insert(user_id.to_string(), AllowState::Allowed);
            self.persist(&g);
        }
        ok
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
