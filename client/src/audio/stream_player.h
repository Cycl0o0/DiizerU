// audio/StreamPlayer — pulls the relay's chunked PCM (GET /v1/stream) on a
// worker thread and feeds an IAudioBackend. Throttles to the backend's buffer so
// memory stays bounded; the relay already paces ~real time. UI-agnostic and
// host-testable (a mock backend stands in for SDL on the host).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "adpcm.h"
#include "iaudio_backend.h"

namespace audio {

class StreamPlayer {
public:
    explicit StreamPlayer(IAudioBackend& backend) : backend_(backend) {}
    ~StreamPlayer();

    // base_url is the relay /v1 root; token is the relay session bearer.
    void start(const std::string& base_url, const std::string& token,
               const std::string& fmt = "pcm_s16le");
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
    AdpcmDecoder& decoder() { return decoder_; }
    std::vector<uint8_t>& pcm_scratch() { return pcm_scratch_; }

private:
    void run(std::string url, std::string token);

    IAudioBackend& backend_;
    bool adpcm_ = false;
    AdpcmDecoder decoder_;
    std::vector<uint8_t> pcm_scratch_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> bytes_{0};
    std::string error_;
};

} // namespace audio
