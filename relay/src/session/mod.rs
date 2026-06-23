//! Per-user playback session manager (multi-tenancy).
//!
//! - One session per active user, keyed by user id.
//! - Created lazily when the console first needs audio/playback; destroyed after
//!   `idle_timeout` of inactivity (GC tick).
//! - `max_concurrent` caps load on the beta VPS (new sessions get Busy).
//! - Holds the playback state + queue + the live `AudioSource` (a tone until a
//!   track is played, then a DeezerSource).

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use crate::audio::{tone::ToneSource, AudioSource};
use crate::model::{PlaybackState, PlayerState, Queue, RepeatMode};

pub struct PlayerSession {
    pub user_id: String,
    pub last_active: i64,
    pub playback: PlaybackState,
    pub queue: Queue,
    /// Live audio source pulled by the /stream handler.
    pub source: Box<dyn AudioSource>,
}

impl PlayerSession {
    fn new(user_id: String, now: i64) -> Self {
        PlayerSession {
            user_id,
            last_active: now,
            playback: PlaybackState {
                state: PlayerState::Stopped,
                track: None,
                position_ms: 0,
                duration_ms: 0,
                repeat: RepeatMode::Off,
                shuffle: false,
                error: None,
            },
            queue: Queue::default(),
            // Starts as a tone; play_uri swaps in a DeezerSource (api::playback).
            source: Box::new(ToneSource::default()),
        }
    }

    pub fn touch(&mut self, now: i64) {
        self.last_active = now;
    }
}

#[derive(Clone)]
pub struct SessionManager {
    sessions: Arc<Mutex<HashMap<String, Arc<Mutex<PlayerSession>>>>>,
    max_concurrent: usize,
    idle_timeout_secs: i64,
}

impl SessionManager {
    pub fn new(max_concurrent: usize, idle_timeout_secs: i64) -> Self {
        SessionManager {
            sessions: Arc::new(Mutex::new(HashMap::new())),
            max_concurrent,
            idle_timeout_secs,
        }
    }

    /// Get the existing session or create one. Errors with `None` if at capacity.
    pub fn get_or_create(&self, user_id: &str, now: i64) -> Option<Arc<Mutex<PlayerSession>>> {
        let mut g = self.sessions.lock().unwrap();
        if let Some(s) = g.get(user_id) {
            s.lock().unwrap().touch(now);
            return Some(s.clone());
        }
        if g.len() >= self.max_concurrent {
            return None; // -> Busy (503)
        }
        let s = Arc::new(Mutex::new(PlayerSession::new(user_id.to_string(), now)));
        g.insert(user_id.to_string(), s.clone());
        Some(s)
    }

    pub fn get(&self, user_id: &str) -> Option<Arc<Mutex<PlayerSession>>> {
        self.sessions.lock().unwrap().get(user_id).cloned()
    }

    pub fn destroy(&self, user_id: &str) {
        self.sessions.lock().unwrap().remove(user_id);
    }

    pub fn destroy_all(&self) {
        self.sessions.lock().unwrap().clear();
    }

    pub fn count(&self) -> usize {
        self.sessions.lock().unwrap().len()
    }

    /// Drop sessions idle longer than the timeout. Returns dropped user_ids.
    pub fn gc(&self, now: i64) -> Vec<String> {
        let mut g = self.sessions.lock().unwrap();
        let dead: Vec<String> = g
            .iter()
            .filter(|(_, s)| now - s.lock().unwrap().last_active > self.idle_timeout_secs)
            .map(|(k, _)| k.clone())
            .collect();
        for k in &dead {
            g.remove(k);
        }
        dead
    }
}
