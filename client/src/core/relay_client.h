// core/relay_client — talks to the relay's /proto API over HTTP (libcurl).
// Pure logic + libcurl + cJSON; no SDL/wut, so it builds & runs on the host for
// tests (tests/) and on the Wii U unchanged. The relay base URL is injected
// (never compiled in) — this is what keeps the central<->self-hosted seam free
// on the client side (ARCHITECTURE §2).
#pragma once

#include <optional>
#include <string>

#include "models.h"

namespace core {

struct HttpResult {
    long status = 0;       // 0 == transport error (see error)
    std::string body;
    std::string error;     // libcurl/transport error, empty on HTTP response
    bool ok() const { return status >= 200 && status < 300; }
};

class RelayClient {
public:
    explicit RelayClient(std::string base_url);

    // Bearer is the opaque relay session token (NOT a Deezer token).
    void set_bearer(std::string token) { bearer_ = std::move(token); }
    const std::string& bearer() const { return bearer_; }
    const std::string& base_url() const { return base_url_; }

    // Endpoints (each returns std::nullopt on transport/parse failure).
    std::optional<Capabilities> capabilities();
    std::optional<PairStart> pair_start(const std::string& device_name);
    std::optional<PairPoll> pair_poll(const std::string& device_code);
    std::optional<Me> me();

    // ---- M6 browse / library ----
    std::optional<SearchResults> search(const std::string& query, const std::string& types = "track,album,artist,playlist");
    std::optional<std::vector<Playlist>> playlists();
    std::optional<std::vector<Track>> playlist_tracks(const std::string& id);
    std::optional<std::vector<Track>> album_tracks(const std::string& id);
    std::optional<std::vector<Track>> favorites();

    // ---- M6 playback / queue ----
    std::optional<PlaybackState> playback();
    std::optional<PlaybackState> playback_command(const std::string& json_body);
    std::optional<Queue> queue();
    std::optional<Queue> queue_command(const std::string& json_body);

    // Convenience command builders.
    std::optional<PlaybackState> play_uri(const std::string& uri);
    std::optional<PlaybackState> toggle();
    std::optional<PlaybackState> next();
    std::optional<PlaybackState> prev();
    std::optional<PlaybackState> seek(long position_ms);

    // Low-level (also used by tests). path is appended to base_url ("/v1/...").
    HttpResult get(const std::string& path);
    HttpResult post(const std::string& path, const std::string& json_body);

private:
    std::string base_url_;   // e.g. https://your-domain.example/v1
    std::string bearer_;
    long timeout_s_ = 20;
};

} // namespace core
