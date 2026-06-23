// audio/SdlAudioBackend — IAudioBackend over SDL2 audio (push model via
// SDL_QueueAudio). Works on the Wii U (SDL2 -> AX) and the host. A future
// AxAudioBackend can implement the same interface for lower-latency native
// output without touching the streaming player.
#pragma once

#include <SDL2/SDL.h>

#include "iaudio_backend.h"

namespace audio {

class SdlAudioBackend : public IAudioBackend {
public:
    bool init(const AudioFormat& fmt) override;
    void shutdown() override;
    bool queue(const uint8_t* data, size_t len) override;
    size_t queued_bytes() override;
    void pause(bool paused) override;
    void clear() override;

    // Obtained device spec (for diagnostics: detect rate/format mismatch).
    int obtained_freq() const { return have_.freq; }
    int obtained_format() const { return have_.format; }
    int obtained_samples() const { return have_.samples; }
    int obtained_channels() const { return have_.channels; }

private:
    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioSpec have_{};
    int frame_bytes_ = 4;     // channels * bytes_per_sample (s16 stereo = 4)
    uint8_t rem_[8] = {0};    // carry partial frame across queue() calls
    size_t rem_len_ = 0;
    size_t prebuffer_ = 0;    // bytes to accumulate before starting playback
    bool started_ = false;    // playback unpaused once prebuffer reached
};

} // namespace audio
