//! Wire DTOs. These mirror /proto/openapi.yaml — the shared contract. Keep in
//! sync with that file (it is the source of truth).

use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Image {
    pub url: String,
    #[serde(default)]
    pub width: Option<u32>,
    #[serde(default)]
    pub height: Option<u32>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Artist {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub images: Vec<Image>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Album {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub artists: Vec<Artist>,
    #[serde(default)]
    pub images: Vec<Image>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Track {
    pub id: String,
    pub uri: String,
    pub name: String,
    pub duration_ms: u64,
    #[serde(default)]
    pub artists: Vec<Artist>,
    #[serde(default)]
    pub album: Option<Album>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Playlist {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub owner: String,
    #[serde(default)]
    pub track_count: u32,
    #[serde(default)]
    pub images: Vec<Image>,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct SearchResults {
    #[serde(default)]
    pub tracks: Vec<Track>,
    #[serde(default)]
    pub albums: Vec<Album>,
    #[serde(default)]
    pub artists: Vec<Artist>,
    #[serde(default)]
    pub playlists: Vec<Playlist>,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum PlayerState {
    Stopped,
    Loading,
    Playing,
    Paused,
    Error,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum RepeatMode {
    Off,
    One,
    All,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PlaybackState {
    pub state: PlayerState,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub track: Option<Track>,
    pub position_ms: u64,
    pub duration_ms: u64,
    pub repeat: RepeatMode,
    pub shuffle: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Queue {
    pub current_index: usize,
    pub items: Vec<Track>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PlaybackAction {
    Play,
    Pause,
    Toggle,
    Next,
    Prev,
    Seek,
    SetRepeat,
    SetShuffle,
    PlayUri,
}

#[derive(Clone, Debug, Deserialize)]
pub struct PlaybackCommand {
    pub action: PlaybackAction,
    #[serde(default)]
    pub position_ms: Option<u64>,
    #[serde(default)]
    pub repeat: Option<RepeatMode>,
    #[serde(default)]
    pub shuffle: Option<bool>,
    #[serde(default)]
    pub uri: Option<String>,
    #[serde(default)]
    pub context_uris: Option<Vec<String>>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum QueueOp {
    Add,
    Move,
    Remove,
    Clear,
    Replace,
}

#[derive(Clone, Debug, Deserialize)]
pub struct QueueCommand {
    pub op: QueueOp,
    #[serde(default)]
    pub uri: Option<String>,
    #[serde(default)]
    pub uris: Option<Vec<String>>,
    #[serde(default)]
    pub from_index: Option<usize>,
    #[serde(default)]
    pub to_index: Option<usize>,
    #[serde(default)]
    pub index: Option<usize>,
}
