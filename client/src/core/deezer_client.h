// core/deezer_client — native (relay-optional) Deezer client running on the
// Wii U. Logs in with the ARL, browses via gw-light + the public REST API, and
// resolves a track to an encrypted CDN URL. Decryption + decode happen in
// audio/ (deezer_decrypt + minimp3). The ARL never leaves the console.
//
// Faithful port of the relay's deezer/{client,proxy}.rs request shapes.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "models.h"
#include "music_client.h"

namespace core {

class DeezerClient : public IMusicClient {
public:
    explicit DeezerClient(std::string arl);

    // Authenticate: fetch api_token + license_token + sid + user id.
    bool login();
    bool logged_in() const { return !api_token_.empty(); }
    const std::string& user_id() const { return user_id_; }
    std::optional<Me> me();

    // IMusicClient
    std::optional<SearchResults> search(
        const std::string& query,
        const std::string& types = "track,album,artist,playlist") override;
    std::optional<std::vector<Playlist>> playlists() override;
    std::optional<std::vector<Track>> playlist_tracks(const std::string& id) override;
    std::optional<std::vector<Track>> album_tracks(const std::string& id) override;
    std::optional<std::vector<Track>> favorites() override;
    std::optional<StreamPlan> prepare_stream(const std::string& uri) override;
    bool seek_to(long /*position_ms*/) override { return false; } // not yet on native path

private:
    std::string cookie() const;
    // gw-light call: returns the response body ("" on failure).
    std::string gw(const std::string& method, const std::string& json_body);
    std::string track_token(const std::string& track_id);
    bool media_url(const std::string& track_token, std::string& url, std::string& format);

    std::string arl_;
    std::string api_token_;
    std::string license_token_;
    std::string sid_;
    std::string user_id_;
    long timeout_s_ = 20;
};

} // namespace core
