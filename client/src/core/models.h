// core/ data models — mirror /proto DTOs. Plain structs, no platform deps, so
// core/ stays unit-testable on the host (see tests/).
#pragma once

#include <string>
#include <vector>

namespace core {

struct Capabilities {
    std::string api_version;
    std::string relay_mode;        // "central" | "self-hosted"
    std::vector<std::string> audio_formats;
    int sample_rate = 0;
    int channels = 0;
};

struct PairStart {
    std::string device_code;       // secret, polled by us
    std::string user_code;         // shown on TV
    std::string verify_url;
    int interval = 5;
    int expires_in = 900;
};

enum class PairStatus { Pending, Approved, Denied, Expired, Unknown };

struct PairPoll {
    PairStatus status = PairStatus::Unknown;
    std::string relay_session_token; // present only when Approved
};

struct Me {
    std::string user_id;
    std::string display_name;
    std::string product;
};

struct Artist {
    std::string id, name;
};

struct Track {
    std::string id, uri, name;
    long duration_ms = 0;
    std::vector<Artist> artists;
    std::string album_name;
    std::string artwork_url;
    // Convenience: "Artist A, Artist B"
    std::string artist_line() const {
        std::string s;
        for (size_t i = 0; i < artists.size(); ++i) {
            if (i) s += ", ";
            s += artists[i].name;
        }
        return s;
    }
};

struct Album {
    std::string id, name;
    std::vector<Artist> artists;
    std::string artwork_url;
};

struct Playlist {
    std::string id, name, owner;
    int track_count = 0;
    std::string artwork_url;
};

struct SearchResults {
    std::vector<Track> tracks;
    std::vector<Album> albums;
    std::vector<Artist> artists;
    std::vector<Playlist> playlists;
};

struct Queue {
    int current_index = 0;
    std::vector<Track> items;
};

enum class PlayerState { Stopped, Loading, Playing, Paused, Error };
enum class RepeatMode { Off, One, All };

struct PlaybackState {
    PlayerState state = PlayerState::Stopped;
    Track track;
    bool has_track = false;
    long position_ms = 0;
    long duration_ms = 0;
    RepeatMode repeat = RepeatMode::Off;
    bool shuffle = false;
    std::string error;
};

} // namespace core
