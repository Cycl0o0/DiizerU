#include "deezer_client.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstring>

#include "../../third_party/cjson/cJSON.h"

#ifdef __WIIU__
#include "cacert_pem.h"
#define DIIZERU_SET_CA(c)                                                       \
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

const char* GW = "https://www.deezer.com/ajax/gw-light.php";
const char* MEDIA = "https://media.deezer.com/v1/get_url";
const char* REST = "https://api.deezer.com";

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

// Header sink: collects raw response headers so we can pull `sid` from Set-Cookie.
size_t header_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 0x0f]; }
    }
    return o;
}

// Extract sid=<value> from accumulated Set-Cookie headers (case-insensitive).
std::string find_sid(const std::string& headers) {
    std::string lower = headers;
    for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
    size_t p = 0;
    while ((p = lower.find("set-cookie:", p)) != std::string::npos) {
        size_t sid = lower.find("sid=", p);
        size_t eol = lower.find('\n', p);
        if (sid != std::string::npos && (eol == std::string::npos || sid < eol)) {
            size_t start = sid + 4;
            size_t end = headers.find_first_of(";\r\n", start);
            return headers.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
        p = (eol == std::string::npos) ? lower.size() : eol + 1;
    }
    return std::string();
}

// ---- cJSON helpers (Deezer uses mixed string/number ids) ----
std::string j_str(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : std::string();
}
std::string j_id(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
    if (cJSON_IsNumber(v)) { char b[32]; std::snprintf(b, sizeof(b), "%.0f", v->valuedouble); return b; }
    return std::string();
}
long j_dur_ms(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    long sec = 0;
    if (cJSON_IsNumber(v)) sec = (long)v->valuedouble;
    else if (cJSON_IsString(v) && v->valuestring) sec = atol(v->valuestring);
    return sec * 1000;
}
std::string gw_cover(const std::string& md5) {
    if (md5.empty()) return std::string();
    return "https://e-cdns-images.dzcdn.net/images/cover/" + md5 +
           "/250x250-000000-80-0-0.jpg";
}

Track rest_track(const cJSON* v) {
    Track t;
    t.id = j_id(v, "id");
    t.uri = "deezer:track:" + t.id;
    t.name = j_str(v, "title");
    t.duration_ms = j_dur_ms(v, "duration");
    const cJSON* ar = cJSON_GetObjectItemCaseSensitive(v, "artist");
    if (ar) t.artists.push_back({j_id(ar, "id"), j_str(ar, "name")});
    const cJSON* al = cJSON_GetObjectItemCaseSensitive(v, "album");
    if (al) { t.album_name = j_str(al, "title"); t.artwork_url = j_str(al, "cover_medium"); }
    return t;
}

Track gw_track(const cJSON* v) {
    Track t;
    t.id = j_id(v, "SNG_ID");
    t.uri = "deezer:track:" + t.id;
    t.name = j_str(v, "SNG_TITLE");
    t.duration_ms = j_dur_ms(v, "DURATION");
    t.artists.push_back({j_id(v, "ART_ID"), j_str(v, "ART_NAME")});
    t.album_name = j_str(v, "ALB_TITLE");
    t.artwork_url = gw_cover(j_str(v, "ALB_PICTURE"));
    return t;
}

// Extract a numeric track id from "deezer:track:123" / ".../123" / "123".
std::string track_id_of(const std::string& uri) {
    size_t p = uri.find_last_of(":/");
    std::string tail = (p == std::string::npos) ? uri : uri.substr(p + 1);
    std::string id;
    for (char c : tail) if (c >= '0' && c <= '9') id += c;
    return id;
}

} // namespace

DeezerClient::DeezerClient(std::string arl) : arl_(std::move(arl)) {}

std::string DeezerClient::cookie() const {
    std::string c = "arl=" + arl_;
    if (!sid_.empty()) c += "; sid=" + sid_;
    return c;
}

bool DeezerClient::login() {
    CURL* c = curl_easy_init();
    if (!c) return false;
    std::string url = std::string(GW) +
        "?method=deezer.getUserData&input=3&api_version=1.0&api_token=";
    std::string body, headers;
    struct curl_slist* h = nullptr;
    h = curl_slist_append(h, ("Cookie: arl=" + arl_).c_str());
    h = curl_slist_append(h, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, "{}");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s_);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 DiizerU-WiiU/0.1");
    DIIZERU_SET_CA(c);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return false;

    sid_ = find_sid(headers);
    cJSON* j = cJSON_Parse(body.c_str());
    if (!j) return false;
    bool ok = false;
    const cJSON* res = cJSON_GetObjectItemCaseSensitive(j, "results");
    if (res) {
        api_token_ = j_str(res, "checkForm");
        const cJSON* user = cJSON_GetObjectItemCaseSensitive(res, "USER");
        if (user) {
            user_id_ = j_id(user, "USER_ID");
            const cJSON* opt = cJSON_GetObjectItemCaseSensitive(user, "OPTIONS");
            if (opt) license_token_ = j_str(opt, "license_token");
        }
        ok = !api_token_.empty() && !user_id_.empty() && user_id_ != "0";
    }
    cJSON_Delete(j);
    return ok;
}

std::optional<Me> DeezerClient::me() {
    if (!logged_in()) return std::nullopt;
    Me m;
    m.user_id = "deezer:" + user_id_;
    m.product = "deezer";
    return m;
}

std::string DeezerClient::gw(const std::string& method, const std::string& json_body) {
    if (api_token_.empty()) return std::string();
    CURL* c = curl_easy_init();
    if (!c) return std::string();
    std::string url = std::string(GW) + "?method=" + method +
        "&input=3&api_version=1.0&api_token=" + url_encode(api_token_);
    std::string body, ck = cookie();
    struct curl_slist* h = nullptr;
    h = curl_slist_append(h, ("Cookie: " + ck).c_str());
    h = curl_slist_append(h, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)json_body.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s_);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 DiizerU-WiiU/0.1");
    DIIZERU_SET_CA(c);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return rc == CURLE_OK ? body : std::string();
}

static std::string rest_get(const std::string& path) {
    CURL* c = curl_easy_init();
    if (!c) return std::string();
    std::string url = std::string(REST) + path, body;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "DiizerU-WiiU/0.1");
    DIIZERU_SET_CA(c);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return rc == CURLE_OK ? body : std::string();
}

std::optional<SearchResults> DeezerClient::search(const std::string& query, const std::string&) {
    std::string enc = url_encode(query);
    SearchResults sr;
    if (cJSON* j = cJSON_Parse(rest_get("/search?q=" + enc + "&limit=40").c_str())) {
        const cJSON* t = nullptr;
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(j, "data")) sr.tracks.push_back(rest_track(t));
        cJSON_Delete(j);
    }
    if (cJSON* j = cJSON_Parse(rest_get("/search/album?q=" + enc + "&limit=20").c_str())) {
        const cJSON* a = nullptr;
        cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(j, "data")) {
            Album al;
            al.id = j_id(a, "id");
            al.name = j_str(a, "title");
            const cJSON* ar = cJSON_GetObjectItemCaseSensitive(a, "artist");
            if (ar) al.artists.push_back({j_id(ar, "id"), j_str(ar, "name")});
            al.artwork_url = j_str(a, "cover_medium");
            sr.albums.push_back(al);
        }
        cJSON_Delete(j);
    }
    if (cJSON* j = cJSON_Parse(rest_get("/search/playlist?q=" + enc + "&limit=20").c_str())) {
        const cJSON* p = nullptr;
        cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(j, "data")) {
            Playlist pl;
            pl.id = j_id(p, "id");
            pl.name = j_str(p, "title");
            const cJSON* u = cJSON_GetObjectItemCaseSensitive(p, "user");
            if (u) pl.owner = j_str(u, "name");
            const cJSON* nb = cJSON_GetObjectItemCaseSensitive(p, "nb_tracks");
            pl.track_count = cJSON_IsNumber(nb) ? nb->valueint : 0;
            pl.artwork_url = j_str(p, "picture_medium");
            sr.playlists.push_back(pl);
        }
        cJSON_Delete(j);
    }
    return sr;
}

std::optional<std::vector<Track>> DeezerClient::album_tracks(const std::string& id) {
    std::vector<Track> out;
    if (cJSON* j = cJSON_Parse(rest_get("/album/" + id + "/tracks?limit=100").c_str())) {
        const cJSON* t = nullptr;
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(j, "data")) out.push_back(rest_track(t));
        cJSON_Delete(j);
    }
    return out;
}

std::optional<std::vector<Track>> DeezerClient::playlist_tracks(const std::string& id) {
    if (!logged_in()) return std::nullopt;
    std::string body = "{\"playlist_id\":\"" + id + "\",\"nb\":500,\"start\":0}";
    std::vector<Track> out;
    if (cJSON* j = cJSON_Parse(gw("playlist.getSongs", body).c_str())) {
        const cJSON* res = cJSON_GetObjectItemCaseSensitive(j, "results");
        const cJSON* data = res ? cJSON_GetObjectItemCaseSensitive(res, "data") : nullptr;
        const cJSON* t = nullptr;
        cJSON_ArrayForEach(t, data) out.push_back(gw_track(t));
        cJSON_Delete(j);
    }
    return out;
}

std::optional<std::vector<Track>> DeezerClient::favorites() {
    if (!logged_in()) return std::nullopt;
    std::string body = "{\"user_id\":\"" + user_id_ + "\",\"nb\":100,\"start\":0}";
    std::vector<Track> out;
    if (cJSON* j = cJSON_Parse(gw("favorite_song.getList", body).c_str())) {
        const cJSON* res = cJSON_GetObjectItemCaseSensitive(j, "results");
        const cJSON* data = res ? cJSON_GetObjectItemCaseSensitive(res, "data") : nullptr;
        const cJSON* t = nullptr;
        cJSON_ArrayForEach(t, data) out.push_back(gw_track(t));
        cJSON_Delete(j);
    }
    return out;
}

std::optional<std::vector<Playlist>> DeezerClient::playlists() {
    if (!logged_in()) return std::nullopt;
    std::string body = "{\"user_id\":\"" + user_id_ + "\",\"tab\":\"playlists\",\"nb\":100}";
    std::vector<Playlist> out;
    if (cJSON* j = cJSON_Parse(gw("deezer.pageProfile", body).c_str())) {
        const cJSON* res = cJSON_GetObjectItemCaseSensitive(j, "results");
        const cJSON* tab = res ? cJSON_GetObjectItemCaseSensitive(res, "TAB") : nullptr;
        const cJSON* pls = tab ? cJSON_GetObjectItemCaseSensitive(tab, "playlists") : nullptr;
        const cJSON* data = pls ? cJSON_GetObjectItemCaseSensitive(pls, "data") : nullptr;
        const cJSON* p = nullptr;
        cJSON_ArrayForEach(p, data) {
            Playlist pl;
            pl.id = j_id(p, "PLAYLIST_ID");
            pl.name = j_str(p, "TITLE");
            pl.track_count = (int)(j_dur_ms(p, "NB_SONG") / 1000); // reuse number coercion
            pl.artwork_url = gw_cover(j_str(p, "PLAYLIST_PICTURE"));
            out.push_back(pl);
        }
        cJSON_Delete(j);
    }
    return out;
}

std::string DeezerClient::track_token(const std::string& track_id) {
    std::string body = "{\"sng_id\":\"" + track_id + "\"}";
    std::string tok;
    if (cJSON* j = cJSON_Parse(gw("song.getData", body).c_str())) {
        const cJSON* res = cJSON_GetObjectItemCaseSensitive(j, "results");
        if (res) tok = j_str(res, "TRACK_TOKEN");
        cJSON_Delete(j);
    }
    return tok;
}

bool DeezerClient::media_url(const std::string& track_token, std::string& url, std::string& format) {
    if (license_token_.empty() || track_token.empty()) return false;
    // Prefer MP3_128 (fits the Wii U's bandwidth), fall back to MP3_320.
    std::string body =
        "{\"license_token\":\"" + license_token_ +
        "\",\"media\":[{\"type\":\"FULL\",\"formats\":["
        "{\"cipher\":\"BF_CBC_STRIPE\",\"format\":\"MP3_128\"},"
        "{\"cipher\":\"BF_CBC_STRIPE\",\"format\":\"MP3_320\"}]}],"
        "\"track_tokens\":[\"" + track_token + "\"]}";

    CURL* c = curl_easy_init();
    if (!c) return false;
    std::string resp;
    struct curl_slist* h = nullptr;
    h = curl_slist_append(h, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, MEDIA);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s_);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "DiizerU-WiiU/0.1");
    DIIZERU_SET_CA(c);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return false;

    bool ok = false;
    if (cJSON* j = cJSON_Parse(resp.c_str())) {
        const cJSON* data = cJSON_GetObjectItemCaseSensitive(j, "data");
        const cJSON* d0 = data ? cJSON_GetArrayItem(data, 0) : nullptr;
        const cJSON* media = d0 ? cJSON_GetObjectItemCaseSensitive(d0, "media") : nullptr;
        const cJSON* m0 = media ? cJSON_GetArrayItem(media, 0) : nullptr;
        if (m0) {
            format = j_str(m0, "format");
            std::printf("[deezer] media format = %s\n", format.c_str());
#ifdef __WIIU__
            if (FILE* lf = std::fopen("fs:/vol/external01/diizeru/audio_fmt.txt", "w")) {
                std::fprintf(lf, "%s\n", format.c_str());
                std::fclose(lf);
            }
#endif
            const cJSON* srcs = cJSON_GetObjectItemCaseSensitive(m0, "sources");
            const cJSON* s0 = srcs ? cJSON_GetArrayItem(srcs, 0) : nullptr;
            if (s0) { url = j_str(s0, "url"); ok = !url.empty(); }
        }
        cJSON_Delete(j);
    }
    return ok;
}

std::optional<StreamPlan> DeezerClient::prepare_stream(const std::string& uri) {
    if (!logged_in()) return std::nullopt;
    std::string id = track_id_of(uri);
    if (id.empty()) return std::nullopt;
    std::string tok = track_token(id);
    std::string url, format;
    if (!media_url(tok, url, format)) return std::nullopt;
    StreamPlan p;
    p.native = true;
    p.cdn_url = url;
    p.track_id = id;
    return p;
}

} // namespace core
