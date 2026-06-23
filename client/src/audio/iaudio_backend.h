// audio/IAudioBackend — abstracts PCM sink so the streaming player doesn't know
// about AX/sndcore2 vs SDL2 (ARCHITECTURE §"Audio"). The relay sends raw PCM
// s16le 44.1k stereo; backends just queue and play it. No Deezer crypto/decode
// on the console.
#pragma once

#include <cstddef>
#include <cstdint>

namespace audio {

struct AudioFormat {
    int sample_rate = 44100;
    int channels = 2;
    // s16le == 2 bytes/sample
    int bytes_per_sample = 2;
    // Audio buffered before playback starts. The native (Deezer) path pulls a
    // finite file over the Wii U's bursty Wi-Fi, so it wants a deeper cushion
    // than the relay path (which is paced ~real time server-side).
    int prebuffer_ms = 1000;
};

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    virtual bool init(const AudioFormat& fmt) = 0;
    virtual void shutdown() = 0;

    // Queue interleaved PCM bytes for playback. Returns false on error.
    virtual bool queue(const uint8_t* data, size_t len) = 0;

    // Bytes currently buffered but not yet played (for the player's throttle).
    virtual size_t queued_bytes() = 0;

    virtual void pause(bool paused) = 0;
    virtual void clear() = 0; // drop buffered audio (e.g. on track skip)
};

} // namespace audio
