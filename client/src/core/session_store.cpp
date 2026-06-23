#include "session_store.h"

#include <sys/stat.h>

#include <cstdio>

#include "../../third_party/cjson/cJSON.h"

namespace core {

SessionStore::SessionStore(std::string base_dir) : base_dir_(std::move(base_dir)) {
    if (!base_dir_.empty() && base_dir_.back() == '/') base_dir_.pop_back();
}

std::string SessionStore::path(const char* file) const {
    return base_dir_ + "/" + file;
}

bool SessionStore::ensure_dir() const {
    // mkdir is a no-op if it exists; SD paths on Wii U accept this too.
    ::mkdir(base_dir_.c_str(), 0755);
    return true;
}

static std::optional<std::string> read_file(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return std::nullopt;
    std::string out;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

static bool write_file(const std::string& p, const std::string& data) {
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return false;
    size_t w = std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return w == data.size();
}

std::optional<std::string> SessionStore::load_relay_url() const {
    auto s = read_file(path("relay.cfg"));
    if (!s) return std::nullopt;
    // trim whitespace/newlines
    size_t a = s->find_first_not_of(" \t\r\n");
    size_t b = s->find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return std::nullopt;
    return s->substr(a, b - a + 1);
}

bool SessionStore::save_relay_url(const std::string& url) const {
    ensure_dir();
    return write_file(path("relay.cfg"), url + "\n");
}

std::optional<std::string> SessionStore::load_arl() const {
    auto s = read_file(path("arl.txt"));
    if (!s) return std::nullopt;
    size_t a = s->find_first_not_of(" \t\r\n");
    size_t b = s->find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return std::nullopt;
    return s->substr(a, b - a + 1);
}

std::optional<std::string> SessionStore::load_token() const {
    auto s = read_file(path("session.json"));
    if (!s) return std::nullopt;
    cJSON* j = cJSON_Parse(s->c_str());
    if (!j) return std::nullopt;
    const cJSON* t = cJSON_GetObjectItemCaseSensitive(j, "relay_session_token");
    std::optional<std::string> out;
    if (cJSON_IsString(t) && t->valuestring && t->valuestring[0]) out = t->valuestring;
    cJSON_Delete(j);
    return out;
}

bool SessionStore::save_token(const std::string& token) const {
    ensure_dir();
    cJSON* j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "relay_session_token", token.c_str());
    char* s = cJSON_PrintUnformatted(j);
    bool ok = s && write_file(path("session.json"), s);
    cJSON_free(s);
    cJSON_Delete(j);
    return ok;
}

bool SessionStore::clear_token() const {
    return std::remove(path("session.json").c_str()) == 0;
}

} // namespace core
