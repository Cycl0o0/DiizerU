#include "stream_player.h"

#include <curl/curl.h>

#include <chrono>

#ifdef __WIIU__
#include <coreinit/thread.h>
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
        if (samples <= 0) continue; // header/skip frame; `samples` is per channel
        // minimp3 returns native-endian interleaved shorts. The native device is
        // opened AUDIO_S16SYS (platform-native byte order), so the samples go out
        // as-is: stereo passes straight through (no packing), mono is upmixed to
        // stereo. No byte-swap and no SDL conversion on the audio thread — that
        // hidden per-period conversion was the deadline miss behind the skips.
        const uint8_t* pcm_bytes;
        size_t nbytes;
        if (info.channels == 2) {
            pcm_bytes = reinterpret_cast<const uint8_t*>(pcm);
            nbytes = (size_t)samples * 2 * sizeof(short); // samples * 2ch * 2 bytes
        } else {
            out.resize((size_t)samples * 2 * sizeof(short));
            int16_t* o = reinterpret_cast<int16_t*>(out.data());
            for (int i = 0; i < samples; ++i) { o[i * 2] = pcm[i]; o[i * 2 + 1] = pcm[i]; }
            pcm_bytes = out.data();
            nbytes = out.size();
        }
        // Backpressure: one curl chunk decodes to many seconds of audio, but the
        // ring drains at real time. Wait for room before queueing so we never
        // overflow the ring (this also throttles curl -> TCP, pacing the whole
        // pipeline to real time without a separate clock).
        while (!should_stop() && backend_.queued_bytes() + nbytes > max_buffered())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (should_stop()) return false;
        if (nbytes && !backend_.queue(pcm_bytes, nbytes)) return false;
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
#ifdef __WIIU__
    // Keep the network + decrypt + MP3 decode burst off the AX audio core (CPU1)
    // so it can never preempt the audio callback (that preemption was causing
    // skips even with a full ring). CPU2 is otherwise idle for homebrew.
    OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU2);
#endif
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
