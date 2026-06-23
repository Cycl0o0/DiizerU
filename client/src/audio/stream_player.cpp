#include "stream_player.h"

#include <curl/curl.h>

#include <chrono>

#ifdef __WIIU__
#include "cacert_pem.h"
#endif

namespace audio {

namespace {
// Keep up to ~2s buffered on the backend; throttle the download to that. The
// relay runs up to ~3s ahead, so this cushion stays full and rides out jitter.
constexpr size_t kMaxBufferedBytes = 44100 * 2 * 2 * 2;
} // namespace

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* sp = static_cast<StreamPlayer*>(userdata);
    size_t len = size * nmemb;
    if (sp->should_stop()) return 0; // abort transfer

    // Throttle: wait while the backend buffer is full (relay paces ~real time).
    while (!sp->should_stop() && sp->backend().queued_bytes() > kMaxBufferedBytes) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (sp->should_stop()) return 0;

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(ptr);
    if (sp->adpcm()) {
        // Decode ADPCM -> s16 PCM, then queue the PCM.
        auto& pcm = sp->pcm_scratch();
        pcm.clear();
        sp->decoder().decode(bytes, len, pcm);
        if (!pcm.empty() && !sp->backend().queue(pcm.data(), pcm.size())) return 0;
    } else {
        if (!sp->backend().queue(bytes, len)) return 0; // backend error -> abort
    }
    sp->add_bytes(len); // count network bytes (real throughput)
    return len;
}

static int xfer_cb(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* sp = static_cast<StreamPlayer*>(userdata);
    return sp->should_stop() ? 1 : 0; // nonzero aborts
}

StreamPlayer::~StreamPlayer() {
    stop();
}

void StreamPlayer::start(const std::string& base_url, const std::string& token,
                         const std::string& fmt) {
    stop();
    stop_.store(false);
    bytes_.store(0);
    error_.clear();
    adpcm_ = (fmt == "adpcm_ima");
    decoder_.reset();
    std::string url = base_url + "/stream?fmt=" + fmt;
    running_.store(true);
    thread_ = std::thread(&StreamPlayer::run, this, url, token);
}

void StreamPlayer::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

void StreamPlayer::run(std::string url, std::string token) {
    CURL* c = curl_easy_init();
    if (!c) {
        error_ = "curl init failed";
        running_.store(false);
        return;
    }
    struct curl_slist* hdrs = nullptr;
    if (!token.empty())
        hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + token).c_str());

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "DiizerU-WiiU/0.1");
    // Throughput tuning (Wii U pulled only ~50 KB/s with defaults).
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 262144L);
    curl_easy_setopt(c, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
#ifdef __WIIU__
    static const struct curl_blob ca_blob = {(void*)cacert_pem, (size_t)cacert_pem_size,
                                             CURL_BLOB_NOCOPY};
    curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca_blob);
#endif
    // no overall timeout: this is an endless stream

    CURLcode rc = curl_easy_perform(c);
    // CURLE_ABORTED_BY_CALLBACK / WRITE_ERROR are expected on stop()
    if (rc != CURLE_OK && rc != CURLE_ABORTED_BY_CALLBACK && rc != CURLE_WRITE_ERROR &&
        !stop_.load()) {
        error_ = curl_easy_strerror(rc);
    }
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    running_.store(false);
}

} // namespace audio
