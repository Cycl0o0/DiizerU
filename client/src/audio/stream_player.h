// audio/StreamPlayer — pulls the relay's chunked PCM (GET /v1/stream) on a
// worker thread and feeds an IAudioBackend. Throttles to the backend's buffer so
// memory stays bounded; the relay already paces ~real time. UI-agnostic and
// host-testable (a mock backend stands in for SDL on the host).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "adpcm.h"
#include "deezer_decrypt.h"
#include "iaudio_backend.h"
#include "../../third_party/minimp3/minimp3.h"

namespace audio {

class StreamPlayer {
public:
    explicit StreamPlayer(IAudioBackend& backend) : backend_(backend) {}
    ~StreamPlayer();

    // Relay path: pull the relay's chunked PCM/ADPCM. base_url is /v1; token is
    // the relay session bearer.
    void start_relay(const std::string& base_url, const std::string& token,
                     const std::string& fmt = "pcm_s16le");
    // Native path: pull the encrypted Deezer track from the CDN, Blowfish-decrypt
    // the stripes and MP3-decode on the console. Backend must be 44100 Hz stereo.
    void start_deezer(const std::string& cdn_url, const std::string& track_id);
    void stop();

    bool running() const { return running_.load(); }
    // 32-bit counter (PPC has no native 64-bit atomic); wraps after ~4GB.
    unsigned long bytes_received() const { return bytes_.load(); }
    const std::string& last_error() const { return error_; }

    // internal (used by the libcurl callbacks)
    IAudioBackend& backend() { return backend_; }
    bool should_stop() const { return stop_.load(); }
    void add_bytes(size_t n) { bytes_.fetch_add((uint32_t)n); }
    bool adpcm() const { return adpcm_; }
    bool deezer() const { return deezer_; }
    // Download throttle target. Native output is 22050 Hz stereo s16 (the
    // proven-good relay rate on this hardware); cap ~1.5s. Relay path ~8s.
    size_t max_buffered() const { return deezer_ ? (size_t)22050 * 4 * 3 / 2 : (size_t)44100 * 4 * 8; }
    AdpcmDecoder& decoder() { return decoder_; }
    std::vector<uint8_t>& pcm_scratch() { return pcm_scratch_; }
    // Decrypt + MP3-decode a network chunk into the backend. false -> abort.
    bool feed_deezer(const uint8_t* data, size_t len);

private:
    void run(std::string url, std::string token);

    IAudioBackend& backend_;
    bool adpcm_ = false;
    AdpcmDecoder decoder_;
    std::vector<uint8_t> pcm_scratch_;
    // native Deezer decode state
    bool deezer_ = false;
    DeezerStripeDecryptor dz_;
    mp3dec_t mp3_;
    std::vector<uint8_t> mp3in_; // decrypted, not-yet-decoded MP3 bytes (rolling)
    // wall-clock pacing (native): feed audio at ~real time so the Wii U never
    // gets a backlog to flush fast at the start of a track.
    bool pace_started_ = false;
    std::chrono::steady_clock::time_point pace_start_;
    uint64_t pace_frames_ = 0; // total output (22050) frames queued
    int decim_phase_ = 0;      // 2:1 decimation 44100 -> 22050 (keep every other)
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> bytes_{0};
    std::string error_;
};

} // namespace audio
