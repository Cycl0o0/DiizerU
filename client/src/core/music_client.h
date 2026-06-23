// core/music_client — the browse + playback-resolve seam the UI talks to.
//
// Two implementations:
//   - RelayClient : the original path. Browse + decode happen on the relay; the
//     console streams ADPCM. ARL lives on the relay.
//   - DeezerClient : the native path. The console logs in to Deezer with the
//     ARL itself, browses, resolves a CDN URL, and decrypts + decodes locally.
//     The relay is not involved; the ARL never leaves the Wii U.
//
// prepare_stream() returns a StreamPlan describing how StreamPlayer should pull
// audio for the chosen track (relay ADPCM vs Deezer CDN + on-device decode).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "models.h"

namespace core {

struct StreamPlan {
    bool native = false;
    // relay path:
    std::string base_url; // relay /v1 root
    std::string bearer;   // relay session token
    std::string fmt;      // e.g. "adpcm_ima"
    // native (Deezer) path:
    std::string cdn_url;  // encrypted track URL
    std::string track_id; // for the Blowfish key
};

class IMusicClient {
public:
    virtual ~IMusicClient() = default;

    virtual std::optional<SearchResults> search(
        const std::string& query,
        const std::string& types = "track,album,artist,playlist") = 0;
    virtual std::optional<std::vector<Playlist>> playlists() = 0;
    virtual std::optional<std::vector<Track>> playlist_tracks(const std::string& id) = 0;
    virtual std::optional<std::vector<Track>> album_tracks(const std::string& id) = 0;
    virtual std::optional<std::vector<Track>> favorites() = 0;

    // Resolve how to stream a track ("deezer:track:<id>" or a bare id).
    virtual std::optional<StreamPlan> prepare_stream(const std::string& uri) = 0;
    // Seek the current track; returns false if unsupported (native path for now).
    virtual bool seek_to(long position_ms) = 0;
};

} // namespace core
