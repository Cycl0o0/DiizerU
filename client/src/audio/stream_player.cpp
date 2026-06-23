#include "stream_player.h"

#include <curl/curl.h>

#include <chrono>

#ifdef __WIIU__
#include "cacert_pem.h"
#endif

namespace audio {

// Throttle target comes from StreamPlayer::max_buffered() (per path).

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* sp = static_cast<StreamPlayer*>(userdata);
    size_t len = size * nmemb;
    if (sp->should_stop()) return 0; // abort transfer

    // Throttle: wait while the backend buffer is full. Keeps the queue shallow so
    // playback self-paces (native) and memory stays bounded (relay).
    while (!sp->should_stop() && sp->backend().queued_bytes() > sp->max_buffered()) {
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
    if (sp->deezer()) sp->log_throughput();
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
    if (tlog_) { std::fclose(tlog_); tlog_ = nullptr; }
    tlog_t0_ = {};
    tlog_last_ = {};
    tlog_last_bytes_ = 0;
    running_.store(true);
    thread_ = std::thread(&StreamPlayer::run, this, cdn_url, std::string());
}

// Append network throughput (avg + instantaneous KB/s) and ring level to SD every
// ~500ms. Lets us tell network starvation from decode starvation for a real track.
void StreamPlayer::log_throughput() {
#ifdef __WIIU__
    auto now = std::chrono::steady_clock::now();
    if (tlog_t0_.time_since_epoch().count() == 0) {
        tlog_t0_ = tlog_last_ = now;
        tlog_ = std::fopen("fs:/vol/external01/diizeru/audio_throughput.txt", "w");
        if (tlog_) std::fprintf(tlog_, "ms\tavg_KBps\tinst_KBps\tring_s\n");
        return;
    }
    double since_last = (double)std::chrono::duration_cast<std::chrono::milliseconds>(now - tlog_last_).count();
    if (since_last < 500.0) return;
    double elapsed = (double)std::chrono::duration_cast<std::chrono::milliseconds>(now - tlog_t0_).count();
    unsigned long b = bytes_received();
    double avg = elapsed > 0 ? (double)b / (elapsed / 1000.0) / 1024.0 : 0.0;
    double inst = since_last > 0 ? (double)(b - tlog_last_bytes_) / (since_last / 1000.0) / 1024.0 : 0.0;
    double ring_s = (double)backend_.queued_bytes() / (44100.0 * 4.0);
    if (tlog_) {
        std::fprintf(tlog_, "%.0f\t%.1f\t%.1f\t%.2f\n", elapsed, avg, inst, ring_s);
        std::fflush(tlog_);
    }
    tlog_last_ = now;
    tlog_last_bytes_ = b;
#endif
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
        // Emit interleaved s16 *little-endian* at the native 44100 Hz + upmix mono
        // -> stereo. minimp3 returns native-endian shorts; the Wii U is big-endian
        // and the backend expects s16le, so write the bytes explicitly. No rate
        // conversion here: the pull-model backend drains at real time, so full
        // 44100 plays at correct speed (SDL upsamples to the AX rate on demand).
        const int ch = info.channels;
        out.clear();
        out.reserve((size_t)samples * 4);
        for (int i = 0; i < samples; ++i) {
            short L = (ch == 2) ? pcm[i * 2] : pcm[i];
            short R = (ch == 2) ? pcm[i * 2 + 1] : pcm[i];
            out.push_back((uint8_t)(L & 0xff)); out.push_back((uint8_t)((L >> 8) & 0xff));
            out.push_back((uint8_t)(R & 0xff)); out.push_back((uint8_t)((R >> 8) & 0xff));
        }
        // Backpressure: one curl chunk decodes to many seconds of audio, but the
        // ring drains at real time. Wait for room before queueing each frame so we
        // never overflow the ring (this also throttles curl -> TCP, pacing the
        // whole pipeline to real time without a separate clock).
        while (!should_stop() && backend_.queued_bytes() + out.size() > max_buffered())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (should_stop()) return false;
        if (!out.empty() && !backend_.queue(out.data(), out.size())) return false;
    }
    if (pos > 0) mp3in_.erase(mp3in_.begin(), mp3in_.begin() + pos);
    return true;
}

void StreamPlayer::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    if (tlog_) { std::fclose(tlog_); tlog_ = nullptr; }
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
