//! Deezer source (feature = "deezer").
//!
//! Why Deezer where Spotify failed: Deezer's per-track decryption key is
//! DERIVED CLIENT-SIDE from the track id (no server audio-key request that can
//! be denied, unlike Spotify's #1649). The relay downloads the encrypted
//! MP3/FLAC from Deezer's CDN and decrypts it locally — works from any IP.
//!
//! Auth: a user-supplied ARL token (Deezer session cookie). Onboarding pastes
//! the ARL (no OAuth). Grey-zone like the Spotify path; requires Deezer Premium.

pub mod client;
pub mod crypto;
pub mod decode;
pub mod proxy;
pub mod source;

pub use client::DeezerClient;
pub use source::DeezerSource;
