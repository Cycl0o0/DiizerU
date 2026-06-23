//! Real per-user Spotify AudioSource backed by librespot (feature = "librespot").
//!
//! Pipeline:
//!   librespot Player (own thread) --decodes Ogg Vorbis--> our `RingSink`
//!     --pushes f32--> `PcmRing` --pulled by--> `RingSource` (impl AudioSource)
//!     --> /v1/stream encoder --> Wii U.
//!
//! librespot emits interleaved f64 stereo at 44_100 Hz (== our SAMPLE_RATE /
//! CHANNELS), so no resampling is needed — we just downcast f64 -> f32. The HTTP
//! /stream handler reads from `RingSource` exactly as it reads from `ToneSource`,
//! so neither the protocol nor the Wii U client changes (ARCHITECTURE §4.2).

use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

use librespot_core::authentication::Credentials;
use librespot_core::cache::Cache;
use librespot_core::config::SessionConfig;
use librespot_core::session::Session;
use librespot_core::spotify_uri::SpotifyUri;
use librespot_playback::audio_backend::{Sink, SinkError, SinkResult};
use librespot_playback::config::PlayerConfig;
use librespot_playback::convert::Converter;
use librespot_playback::decoder::AudioPacket;
use librespot_playback::mixer::NoOpVolume;
use librespot_playback::player::Player;

use super::{AudioSource, CHANNELS, SAMPLE_RATE};

/// Bounded shared PCM ring buffer (interleaved f32). Producer = librespot sink,
/// consumer = the HTTP stream handler. Bounded so a slow console cannot make the
/// relay buffer unbounded memory (per-session memory cap, SECURITY.md).
#[derive(Clone)]
pub struct PcmRing {
    inner: Arc<Mutex<VecDeque<f32>>>,
    cap: usize,
}

impl PcmRing {
    pub fn new(cap_samples: usize) -> Self {
        PcmRing {
            inner: Arc::new(Mutex::new(VecDeque::with_capacity(cap_samples.min(1 << 20)))),
            cap: cap_samples,
        }
    }

    /// Push decoded samples; drop oldest if over capacity (favor live audio).
    pub fn push(&self, samples: &[f32]) {
        let mut q = self.inner.lock().unwrap();
        for &s in samples {
            if q.len() >= self.cap {
                q.pop_front();
            }
            q.push_back(s);
        }
    }
}

/// The AudioSource end of the ring (what /stream pulls).
pub struct RingSource {
    ring: PcmRing,
}

impl RingSource {
    pub fn new(ring: PcmRing) -> Self {
        RingSource { ring }
    }
}

impl AudioSource for RingSource {
    fn read(&mut self, out: &mut [f32]) -> usize {
        let mut q = self.ring.inner.lock().unwrap();
        let n = out.len().min(q.len());
        for slot in out.iter_mut().take(n) {
            *slot = q.pop_front().unwrap();
        }
        n
    }
    fn sample_rate(&self) -> u32 {
        SAMPLE_RATE
    }
    fn channels(&self) -> u16 {
        CHANNELS
    }
}

/// librespot audio backend `Sink` that writes decoded PCM into a `PcmRing`.
struct RingSink {
    ring: PcmRing,
}

impl Sink for RingSink {
    fn write(&mut self, packet: AudioPacket, _converter: &mut Converter) -> SinkResult<()> {
        match packet.samples() {
            Ok(samples) => {
                // f64 interleaved stereo @ 44.1k -> f32
                let f32s: Vec<f32> = samples.iter().map(|&s| s as f32).collect();
                self.ring.push(&f32s);
                Ok(())
            }
            // Passthrough (raw Ogg) is disabled in our PlayerConfig, so this is
            // unexpected; treat as a benign empty write.
            Err(_) => Err(SinkError::OnWrite("expected decoded samples".into())),
        }
    }
}

/// Resolve the Spotify user_id from an OAuth access token via a librespot
/// session (avoids the Web API /me call, which Spotify rate-limits hard for the
/// keymaster client on a server IP).
pub async fn user_id_from_token(access_token: &str) -> anyhow::Result<String> {
    let session = Session::new(SessionConfig::default(), None);
    session
        .connect(Credentials::with_access_token(access_token), false)
        .await
        .map_err(|e| anyhow::anyhow!("librespot connect: {e}"))?;
    Ok(session.username())
}

/// A live per-user librespot player + the ring the /stream handler reads.
pub struct LibrespotPlayer {
    pub session: Session,
    pub player: Arc<Player>,
    pub ring: PcmRing,
}

impl LibrespotPlayer {
    /// Connect a librespot session. Uses cached reusable credentials when
    /// available (these can fetch audio keys); otherwise authenticates with the
    /// OAuth access token and caches the resulting credentials for next time.
    pub async fn connect(
        access_token: &str,
        cache_dir: Option<std::path::PathBuf>,
    ) -> anyhow::Result<Self> {
        let cache = cache_dir.clone().and_then(|d| Cache::new(Some(d), None, None, None).ok());

        // Prefer cached reusable credentials (these CAN fetch audio keys). If we
        // don't have them yet, do a throwaway connect with the OAuth token to
        // obtain + cache them, then use them for the real session — a raw
        // access-token session cannot fetch audio keys, but the reusable blob it
        // caches can.
        let mut creds = cache.as_ref().and_then(|c| c.credentials());
        if creds.is_none() {
            if let Some(d) = cache_dir {
                if let Ok(warm_cache) = Cache::new(Some(d), None, None, None) {
                    let warm = Session::new(SessionConfig::default(), Some(warm_cache.clone()));
                    warm.connect(Credentials::with_access_token(access_token), true)
                        .await
                        .map_err(|e| anyhow::anyhow!("librespot warm connect: {e}"))?;
                    creds = warm_cache.credentials();
                    drop(warm);
                }
            }
        }
        let creds = creds.unwrap_or_else(|| Credentials::with_access_token(access_token));
        let session = Session::new(SessionConfig::default(), cache);
        session
            .connect(creds, true)
            .await
            .map_err(|e| anyhow::anyhow!("librespot connect: {e}"))?;

        // ~2 seconds of headroom: 44100 * 2ch * 2s.
        let ring = PcmRing::new(SAMPLE_RATE as usize * CHANNELS as usize * 2);
        let sink_ring = ring.clone();

        let player = Player::new(
            PlayerConfig::default(),
            session.clone(),
            Box::new(NoOpVolume),
            move || Box::new(RingSink { ring: sink_ring }),
        );

        Ok(LibrespotPlayer { session, player, ring })
    }

    pub fn source(&self) -> RingSource {
        RingSource::new(self.ring.clone())
    }

    /// Load + start a track/episode by Spotify URI.
    pub fn load_uri(&self, uri: &str, start_playing: bool, position_ms: u32) -> anyhow::Result<()> {
        let id = SpotifyUri::from_uri(uri).map_err(|e| anyhow::anyhow!("bad uri {uri}: {e:?}"))?;
        self.player.load(id, start_playing, position_ms);
        Ok(())
    }

    pub fn play(&self) {
        self.player.play();
    }
    pub fn pause(&self) {
        self.player.pause();
    }
    pub fn seek(&self, position_ms: u32) {
        self.player.seek(position_ms);
    }
    pub fn stop(&self) {
        self.player.stop();
    }
}
