#include "stream_player.h"

#include <curl/curl.h>

#include <chrono>

#ifdef __WIIU__
#include "cacert_pem.h"
#endif

namespace audio {

namespace {
// Keep up to ~8s of 44.1k stereo s16 buffered, then throttle the download. The
// relay path stays near real time (server-paced); the native path pulls a finite
// file over bursty Wi-Fi, so a deep cushion is what rides out the dips.
constexpr size_t kMaxBufferedBytes = 44100 * 4 * 8;
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
    if (sp->deezer()) {
        // Blowfish-decrypt the stripes + MP3-decode on the console.
        if (!sp->feed_deezer(bytes, len)) return 0;
    } else if (sp->adpcm()) {
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

void StreamPlayer::start_relay(const std::string& base_url, const std::string& token,
                               const std::string& fmt) {
    stop();
    stop_.store(false);
    bytes_.store(0);
    error_.clear();
    adpcm_ = (fmt == "adpcm_ima");
    deezer_ = false;
    decoder_.reset();
    std::string url = base_url + "/stream?fmt=" + fmt;
    running_.store(true);
    thread_ = std::thread(&StreamPlayer::run, this, url, std::string(token));
}

void StreamPlayer::start_deezer(const std::string& cdn_url, const std::string& track_id) {
    stop();
    stop_.store(false);
    bytes_.store(0);
    error_.clear();
    adpcm_ = false;
    deezer_ = true;
    dz_.init(track_id);
    mp3dec_init(&mp3_);
    mp3in_.clear();
    running_.store(true);
    thread_ = std::thread(&StreamPlayer::run, this, cdn_url, std::string());
}

// Decrypt the network chunk, then MP3-decode as many whole frames as are
// buffered, queueing s16 stereo PCM to the backend (mono is upmixed).
bool StreamPlayer::feed_deezer(const uint8_t* data, size_t len) {
    dz_.feed(data, len, mp3in_); // append decrypted bytes to the rolling buffer
    size_t pos = 0;
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    auto& out = pcm_scratch_; // reused byte buffer
    while (mp3in_.size() - pos > 0) {
        mp3dec_frame_info_t info;
        // `samples` is per channel; pcm holds samples*channels interleaved shorts.
        int samples = mp3dec_decode_frame(
            &mp3_, mp3in_.data() + pos, (int)(mp3in_.size() - pos), pcm, &info);
        if (info.frame_bytes == 0) break; // need more data
        pos += (size_t)info.frame_bytes;
        if (samples <= 0) continue; // header/skip frame
        // Emit interleaved s16 *little-endian* + upmix mono -> stereo. minimp3
        // returns native-endian shorts; the Wii U is big-endian and the backend
        // expects s16le (AUDIO_S16LSB), so write the bytes explicitly.
        out.clear();
        out.resize((size_t)samples * 2 /*stereo*/ * 2 /*bytes*/);
        size_t o = 0;
        if (info.channels == 2) {
            for (int i = 0; i < samples * 2; ++i) {
                out[o++] = (uint8_t)(pcm[i] & 0xff);
                out[o++] = (uint8_t)((pcm[i] >> 8) & 0xff);
            }
        } else {
            for (int i = 0; i < samples; ++i) {
                uint8_t lo = (uint8_t)(pcm[i] & 0xff), hi = (uint8_t)((pcm[i] >> 8) & 0xff);
                out[o++] = lo; out[o++] = hi; // L
                out[o++] = lo; out[o++] = hi; // R
            }
        }
        if (!backend_.queue(out.data(), o)) return false;
    }
    if (pos > 0) mp3in_.erase(mp3in_.begin(), mp3in_.begin() + pos);
    return true;
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
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); // Deezer CDN may redirect
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
