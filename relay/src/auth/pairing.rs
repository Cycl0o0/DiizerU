//! Device-code pairing state. The console starts a pairing, gets a `user_code`
//! shown on the TV; the user links it on their phone (enters the code + Deezer
//! ARL); the console polls until a relay session token is issued.

use std::collections::HashMap;
use std::sync::RwLock;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PairStatus {
    Pending,
    Approved,
    Denied,
    Expired,
}

#[derive(Clone)]
pub struct PairingRecord {
    pub device_code: String,
    pub user_code: String,
    pub device_name: String,
    pub status: PairStatus,
    /// Invite code the user supplied on the verify page (consumed on link).
    pub invite_code: Option<String>,
    /// Set when approved; the opaque relay session token for the console.
    pub relay_session_token: Option<String>,
    pub expires_at: i64,
}

#[derive(Default)]
pub struct PairingManager {
    by_device_code: RwLock<HashMap<String, PairingRecord>>,
    by_user_code: RwLock<HashMap<String, String>>, // user_code -> device_code
}

impl PairingManager {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn insert(&self, rec: PairingRecord) {
        self.by_user_code
            .write()
            .unwrap()
            .insert(rec.user_code.clone(), rec.device_code.clone());
        self.by_device_code
            .write()
            .unwrap()
            .insert(rec.device_code.clone(), rec);
    }

    pub fn get_by_device_code(&self, device_code: &str, now: i64) -> Option<PairingRecord> {
        let mut g = self.by_device_code.write().unwrap();
        let rec = g.get_mut(device_code)?;
        if rec.status == PairStatus::Pending && rec.expires_at <= now {
            rec.status = PairStatus::Expired;
        }
        Some(rec.clone())
    }

    pub fn device_code_for_user_code(&self, user_code: &str) -> Option<String> {
        self.by_user_code.read().unwrap().get(user_code).cloned()
    }

    pub fn get(&self, device_code: &str) -> Option<PairingRecord> {
        self.by_device_code.read().unwrap().get(device_code).cloned()
    }

    pub fn set_invite(&self, device_code: &str, invite: Option<String>) {
        if let Some(rec) = self.by_device_code.write().unwrap().get_mut(device_code) {
            rec.invite_code = invite;
        }
    }

    pub fn approve(&self, device_code: &str, relay_session_token: String) {
        if let Some(rec) = self.by_device_code.write().unwrap().get_mut(device_code) {
            rec.status = PairStatus::Approved;
            rec.relay_session_token = Some(relay_session_token);
        }
    }

    pub fn deny(&self, device_code: &str) {
        if let Some(rec) = self.by_device_code.write().unwrap().get_mut(device_code) {
            rec.status = PairStatus::Denied;
        }
    }

    /// Drop expired pending records (called by the session GC tick).
    pub fn gc(&self, now: i64) {
        let mut g = self.by_device_code.write().unwrap();
        let expired: Vec<String> = g
            .iter()
            .filter(|(_, r)| r.expires_at <= now && r.status == PairStatus::Pending)
            .map(|(k, _)| k.clone())
            .collect();
        for dc in expired {
            if let Some(r) = g.get_mut(&dc) {
                r.status = PairStatus::Expired;
            }
        }
    }
}
