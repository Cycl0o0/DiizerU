// core/session_store — persists the relay URL + relay session token on SD
// (sd:/diizeru). The relay URL is config, never compiled in (central<->self
// -hosted seam). Plain stdio so it works on the Wii U and the host (tests use a
// temp dir).
#pragma once

#include <optional>
#include <string>

namespace core {

class SessionStore {
public:
    // base_dir defaults to the SD path on Wii U; tests pass a temp dir.
    explicit SessionStore(std::string base_dir = "sd:/diizeru");

    // relay.cfg: a single line with the relay base URL (incl. /v1).
    std::optional<std::string> load_relay_url() const;
    bool save_relay_url(const std::string& url) const;

    // session.json: { "relay_session_token": "..." }
    std::optional<std::string> load_token() const;
    bool save_token(const std::string& token) const;
    bool clear_token() const;

    const std::string& base_dir() const { return base_dir_; }

private:
    std::string base_dir_;
    std::string path(const char* file) const;
    bool ensure_dir() const;
};

} // namespace core
