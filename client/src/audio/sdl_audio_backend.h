// audio/SdlAudioBackend — IAudioBackend over SDL2 audio using the PULL
// (callback) model backed by our own ring buffer.
//
// Why pull, not SDL_QueueAudio: the Wii U SDL2 port drains a queued backlog
// FASTER than real time to "catch up", which made a freshly-filled queue play
// ultra-fast for the first seconds of a track (the native path fills the queue
// at CDN speed, far above real time, so it always had a backlog). With a
// callback device SDL pulls exactly one period of audio per real-time interval,
// so a backlog simply waits in the ring — it can never be rushed. Underruns
// become silence (zero-fill), never a speed change.
#pragma once

#include <SDL2/SDL.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include "iaudio_backend.h"

namespace audio {

class SdlAudioBackend : public IAudioBackend {
public:
    bool init(const AudioFormat& fmt) override;
    void shutdown() override;
    bool queue(const uint8_t* data, size_t len) override; // producer (curl thread)
    size_t queued_bytes() override;                       // ring bytes not yet played
    void pause(bool paused) override;
    void clear() override;

    // Obtained device spec (diagnostics; note allowed_changes=0 forces have==want,
    // so these echo the request — they do NOT reveal the real AX hardware rate).
    int obtained_freq() const { return have_.freq; }
    int obtained_format() const { return have_.format; }
    int obtained_samples() const { return have_.samples; }
    int obtained_channels() const { return have_.channels; }

private:
    static void audio_cb(void* userdata, Uint8* stream, int len); // SDL audio thread
    void fill(Uint8* stream, int len);                            // consumer

    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioSpec have_{};
    int frame_bytes_ = 4; // channels * bytes_per_sample (s16 stereo = 4)

    // Ring buffer (single producer = curl thread, single consumer = audio cb),
    // guarded by m_. Holds the raw s16le byte stream; the SDL callback always
    // asks for whole-frame-multiple lengths, so byte-granular storage stays
    // frame-aligned as long as we never drop a partial frame.
    std::mutex m_;
    std::vector<uint8_t> ring_;
    size_t cap_ = 0;        // ring capacity in bytes
    size_t head_ = 0;       // read offset
    size_t tail_ = 0;       // write offset
    size_t avail_ = 0;      // bytes available to play
    size_t prebuffer_ = 0;  // bytes to accumulate before real output begins
    bool playing_ = false;  // false => callback emits silence until prebuffer met

    // Real-time output governor. The Wii U port pulls buffered audio FASTER than
    // real time (it burns the ring/AX backlog at startup), which plays the first
    // seconds sped-up. The callback therefore releases real samples only up to the
    // wall-clock budget (rate_ frames/sec) and pads any over-pull with silence, so
    // audio always occupies correct real-time duration no matter how greedily the
    // port pulls. A deep ring still rides out Wi-Fi jitter.
    int rate_ = 44100;             // output frames per second
    size_t credit_frames_ = 2048;  // allow up to ~one period ahead of real time
    std::chrono::steady_clock::time_point t0_; // playback start (per track)
    uint64_t out_frames_ = 0;      // real frames released since t0_
};

} // namespace audio
