#include "relay_client.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstring>

#include "../../third_party/cjson/cJSON.h"

#ifdef __WIIU__
#include "cacert_pem.h" // embedded CA bundle (bin2o from data/cacert.pem)
#define DIIZERU_SET_CA(c)                                                     \
    do {                                                                        \
        static const struct curl_blob _ca = {(void*)cacert_pem,                 \
                                             (size_t)cacert_pem_size,           \
                                             CURL_BLOB_NOCOPY};                 \
        curl_easy_setopt((c), CURLOPT_CAINFO_BLOB, &_ca);                       \
    } while (0)
#else
#define DIIZERU_SET_CA(c) ((void)0)
#endif

namespace core {

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string json_str(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : std::string();
}

int json_int(const cJSON* o, const char* key, int dflt = 0) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}

PairStatus parse_status(const std::string& s) {
    if (s == "pending") return PairStatus::Pending;
    if (s == "approved") return PairStatus::Approved;
    if (s == "denied") return PairStatus::Denied;
    if (s == "expired") return PairStatus::Expired;
    return PairStatus::Unknown;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            o += (char)c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 0x0f];
        }
    }
    return o;
}

std::string first_image(const cJSON* o) {
    const cJSON* imgs = cJSON_GetObjectItemCaseSensitive(o, "images");
    const cJSON* first = imgs ? cJSON_GetArrayItem(imgs, 0) : nullptr;
    return first ? json_str(first, "url") : std::string();
}

std::vector<Artist> parse_artists(const cJSON* o) {
    std::vector<Artist> out;
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(o, "artists");
    const cJSON* a = nullptr;
    cJSON_ArrayForEach(a, arr) {
        out.push_back({json_str(a, "id"), json_str(a, "name")});
    }
    return out;
}

Track parse_track(const cJSON* t) {
    Track tr;
    tr.id = json_str(t, "id");
    tr.uri = json_str(t, "uri");
    tr.name = json_str(t, "name");
    tr.duration_ms = json_int(t, "duration_ms");
    tr.artists = parse_artists(t);
    const cJSON* alb = cJSON_GetObjectItemCaseSensitive(t, "album");
    if (alb) {
        tr.album_name = json_str(alb, "name");
        tr.artwork_url = first_image(alb);
    }
    return tr;
}

Album parse_album(const cJSON* a) {
    Album al;
    al.id = json_str(a, "id");
    al.name = json_str(a, "name");
    al.artists = parse_artists(a);
    al.artwork_url = first_image(a);
    return al;
}

Playlist parse_playlist(const cJSON* p) {
    Playlist pl;
    pl.id = json_str(p, "id");
    pl.name = json_str(p, "name");
    pl.owner = json_str(p, "owner");
    pl.track_count = json_int(p, "track_count");
    pl.artwork_url = first_image(p);
    return pl;
}

std::vector<Track> parse_track_array(const cJSON* arr) {
    std::vector<Track> out;
    const cJSON* t = nullptr;
    cJSON_ArrayForEach(t, arr) out.push_back(parse_track(t));
    return out;
}

PlayerState parse_player_state(const std::string& s) {
    if (s == "playing") return PlayerState::Playing;
    if (s == "paused") return PlayerState::Paused;
    if (s == "loading") return PlayerState::Loading;
    if (s == "error") return PlayerState::Error;
    return PlayerState::Stopped;
}

RepeatMode parse_repeat(const std::string& s) {
    if (s == "one") return RepeatMode::One;
    if (s == "all") return RepeatMode::All;
    return RepeatMode::Off;
}

PlaybackState parse_playback(const cJSON* j) {
    PlaybackState pb;
    pb.state = parse_player_state(json_str(j, "state"));
    pb.position_ms = json_int(j, "position_ms");
    pb.duration_ms = json_int(j, "duration_ms");
    pb.repeat = parse_repeat(json_str(j, "repeat"));
    const cJSON* sh = cJSON_GetObjectItemCaseSensitive(j, "shuffle");
    pb.shuffle = cJSON_IsTrue(sh);
    const cJSON* tr = cJSON_GetObjectItemCaseSensitive(j, "track");
    if (tr && cJSON_IsObject(tr)) {
        pb.track = parse_track(tr);
        pb.has_track = true;
    }
    pb.error = json_str(j, "error");
    return pb;
}

} // namespace

RelayClient::RelayClient(std::string base_url) : base_url_(std::move(base_url)) {
    // tolerate a trailing slash
    if (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
}

HttpResult RelayClient::get(const std::string& path) {
    HttpResult r;
    CURL* c = curl_easy_init();
    if (!c) {
        r.error = "curl init failed";
        return r;
    }
    std::string url = base_url_ + path;
    struct curl_slist* hdrs = nullptr;
    if (!bearer_.empty())
        hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + bearer_).c_str());

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s_);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "DiizerU-WiiU/0.1");
    DIIZERU_SET_CA(c);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

HttpResult RelayClient::post(const std::string& path, const std::string& json_body) {
    HttpResult r;
    CURL* c = curl_easy_init();
    if (!c) {
        r.error = "curl init failed";
        return r;
    }
    std::string url = base_url_ + path;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    if (!bearer_.empty())
        hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + bearer_).c_str());

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)json_body.size());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s_);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "DiizerU-WiiU/0.1");
    DIIZERU_SET_CA(c);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

std::optional<Capabilities> RelayClient::capabilities() {
    HttpResult r = get("/capabilities");
    if (!r.ok()) return std::nullopt;
    cJSON* j = cJSON_Parse(r.body.c_str());
    if (!j) return std::nullopt;
    Capabilities cap;
    cap.api_version = json_str(j, "api_version");
    cap.relay_mode = json_str(j, "relay_mode");
    cap.sample_rate = json_int(j, "sample_rate");
    cap.channels = json_int(j, "channels");
    const cJSON* fmts = cJSON_GetObjectItemCaseSensitive(j, "audio_formats");
    const cJSON* f = nullptr;
    cJSON_ArrayForEach(f, fmts) {
        if (cJSON_IsString(f) && f->valuestring) cap.audio_formats.push_back(f->valuestring);
    }
    cJSON_Delete(j);
    return cap;
}

std::optional<PairStart> RelayClient::pair_start(const std::string& device_name) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_name", device_name.c_str());
    char* bs = cJSON_PrintUnformatted(body);
    HttpResult r = post("/pair/start", bs ? bs : "{}");
    cJSON_free(bs);
    cJSON_Delete(body);
    if (!r.ok()) return std::nullopt;
    cJSON* j = cJSON_Parse(r.body.c_str());
    if (!j) return std::nullopt;
    PairStart p;
    p.device_code = json_str(j, "device_code");
    p.user_code = json_str(j, "user_code");
    p.verify_url = json_str(j, "verify_url");
    p.interval = json_int(j, "interval", 5);
    p.expires_in = json_int(j, "expires_in", 900);
    cJSON_Delete(j);
    return p;
}

std::optional<PairPoll> RelayClient::pair_poll(const std::string& device_code) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_code", device_code.c_str());
    char* bs = cJSON_PrintUnformatted(body);
    HttpResult r = post("/pair/poll", bs ? bs : "{}");
    cJSON_free(bs);
    cJSON_Delete(body);
    if (!r.ok()) return std::nullopt;
    cJSON* j = cJSON_Parse(r.body.c_str());
    if (!j) return std::nullopt;
    PairPoll p;
    p.status = parse_status(json_str(j, "status"));
    p.relay_session_token = json_str(j, "relay_session_token");
    cJSON_Delete(j);
    return p;
}

std::optional<Me> RelayClient::me() {
    HttpResult r = get("/me");
    if (!r.ok()) return std::nullopt;
    cJSON* j = cJSON_Parse(r.body.c_str());
    if (!j) return std::nullopt;
    Me m;
    m.user_id = json_str(j, "user_id");
    m.display_name = json_str(j, "display_name");
    m.product = json_str(j, "product");
    cJSON_Delete(j);
    return m;
}

std::optional<SearchResults> RelayClient::search(const std::string& query, const std::string& types) {
    std::string path = "/search?q=" + url_encode(query) + "&type=" + url_encode(types);
    HttpResult r = get(path);
    if (!r.ok()) return std::nullopt;
    cJSON* j = cJSON_Parse(r.body.c_str());
    if (!j) return std::nullopt;
    SearchResults sr;
    const cJSON* a;
    a = nullptr;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(j, "tracks")) sr.tracks.push_back(parse_track(a));
    a = nullptr;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(j, "albums")) sr.albums.push_back(parse_album(a));
    a = nullptr;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(j, "artists")) sr.artists.push_back({json_str(a, "id"), json_str(a, "name")});
    a = nullptr;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(j, "playlists")) sr.playlists.push_back(parse_playlist(a));
    cJSON_Delete(j);
    return sr;
}

std::optional<std::vector<Playlist>> RelayClient::playlists() {
    HttpResult r = get("/browse/playlists");
    if (!r.ok()) return std::nullopt;
    cJSON* j = cJSON_Parse(r.body.c_str());
    if (!j) return std::nullopt;
    std::vector<Playlist> out;
    const cJSON* p = nullptr;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(j, "items")) out.push_back(parse_playlist(p));
    cJSON_Delete(j);
    return out;
}

static std::optional<std::vector<Track>> tracks_from(const HttpResult& r) {
    if (!r.ok()) return std::nullopt;
    cJSON* j = cJSON_Parse(r.body.c_str());
    if (!j) return std::nullopt;
    auto out = parse_track_array(cJSON_GetObjectItemCaseSensitive(j, "tracks"));
    cJSON_Delete(j);
    return out;
}

std::optional<std::vector<Track>> RelayClient::playlist_tracks(const std::string& id) {
    return tracks_from(get("/browse/playlist/" + id));
}
std::optional<std::vector<Track>> RelayClient::album_tracks(const std::string& id) {
    return tracks_from(get("/browse/album/" + id));
}
std::optional<std::vector<Track>> RelayClient::favorites() {
    return tracks_from(get("/browse/favorites"));
}

std::optional<PlaybackState> RelayClient::playback() {
    HttpResult r = get("/playback");
    if (!r.ok()) return std::nullopt;
    cJSON* j = cJSON_Parse(r.body.c_str());
    if (!j) return std::nullopt;
    PlaybackState pb = parse_playback(j);
    cJSON_Delete(j);
    return pb;
}

std::optional<PlaybackState> RelayClient::playback_command(const std::string& json_body) {
    HttpResult r = post("/playback/command", json_body);
    if (!r.ok()) return std::nullopt;
    cJSON* j = cJSON_Parse(r.body.c_str());
    if (!j) return std::nullopt;
    PlaybackState pb = parse_playback(j);
    cJSON_Delete(j);
    return pb;
}

std::optional<Queue> RelayClient::queue() {
    HttpResult r = get("/queue");
    if (!r.ok()) return std::nullopt;
    cJSON* j = cJSON_Parse(r.body.c_str());
    if (!j) return std::nullopt;
    Queue q;
    q.current_index = json_int(j, "current_index");
    q.items = parse_track_array(cJSON_GetObjectItemCaseSensitive(j, "items"));
    cJSON_Delete(j);
    return q;
}

std::optional<Queue> RelayClient::queue_command(const std::string& json_body) {
    HttpResult r = post("/queue", json_body);
    if (!r.ok()) return std::nullopt;
    cJSON* j = cJSON_Parse(r.body.c_str());
    if (!j) return std::nullopt;
    Queue q;
    q.current_index = json_int(j, "current_index");
    q.items = parse_track_array(cJSON_GetObjectItemCaseSensitive(j, "items"));
    cJSON_Delete(j);
    return q;
}

std::optional<PlaybackState> RelayClient::play_uri(const std::string& uri) {
    return playback_command(R"({"action":"play_uri","uri":")" + uri + "\"}");
}
std::optional<PlaybackState> RelayClient::toggle() {
    return playback_command(R"({"action":"toggle"})");
}
std::optional<PlaybackState> RelayClient::next() {
    return playback_command(R"({"action":"next"})");
}
std::optional<PlaybackState> RelayClient::prev() {
    return playback_command(R"({"action":"prev"})");
}
std::optional<PlaybackState> RelayClient::seek(long position_ms) {
    char b[64];
    std::snprintf(b, sizeof(b), R"({"action":"seek","position_ms":%ld})", position_ms);
    return playback_command(b);
}

} // namespace core
