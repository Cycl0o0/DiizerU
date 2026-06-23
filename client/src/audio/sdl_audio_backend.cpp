#include "sdl_audio_backend.h"

#include <cstdio>
#include <cstring>

namespace audio {

bool SdlAudioBackend::init(const AudioFormat& fmt) {
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::printf("[audio] SDL audio init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{};
    want.freq = fmt.sample_rate;
    want.format = AUDIO_S16LSB; // s16le, matches relay PCM
    want.channels = (Uint8)fmt.channels;
    want.samples = 4096; // device buffer (frames) ~93ms of slack
    want.callback = nullptr; // push model via SDL_QueueAudio

    SDL_AudioSpec have{};
    // Allow the device to report its native rate/format; we still queue s16le
    // 44.1k and SDL converts. Storing `have` lets us diagnose mismatches.
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev_ == 0) {
        std::printf("[audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }
    have_ = have;
    frame_bytes_ = fmt.channels * fmt.bytes_per_sample;     // s16 stereo = 4
    prebuffer_ = (size_t)fmt.sample_rate * frame_bytes_; // ~1.0s cushion before play
    rem_len_ = 0;
    started_ = false;
    // Start PAUSED: avoid underruns by filling a cushion before playback begins.
    SDL_PauseAudioDevice(dev_, 1);
    return true;
}

void SdlAudioBackend::shutdown() {
    if (dev_) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
}

bool SdlAudioBackend::queue(const uint8_t* data, size_t len) {
    if (!dev_) return false;
    // Queue only whole audio frames; carry any partial frame to the next call.
    // libcurl hands us arbitrary-sized buffers — misaligned bytes would shift
    // every following sample and produce strident noise.
    const int fb = frame_bytes_;

    // 1) complete a carried partial frame first
    if (rem_len_ > 0) {
        size_t need = (size_t)fb - rem_len_;
        size_t take = need < len ? need : len;
        std::memcpy(rem_ + rem_len_, data, take);
        rem_len_ += take;
        data += take;
        len -= take;
        if (rem_len_ == (size_t)fb) {
            if (SDL_QueueAudio(dev_, rem_, (Uint32)fb) != 0) return false;
            rem_len_ = 0;
        } else {
            return true; // still incomplete
        }
    }

    // 2) bulk-queue the aligned remainder
    size_t aligned = len - (len % fb);
    if (aligned && SDL_QueueAudio(dev_, data, (Uint32)aligned) != 0) return false;

    // 3) stash the tail (0..fb-1 bytes) for next time
    size_t tail = len - aligned;
    if (tail) {
        std::memcpy(rem_, data + aligned, tail);
        rem_len_ = tail;
    }

    // Begin playback once the prebuffer cushion is filled.
    if (!started_ && SDL_GetQueuedAudioSize(dev_) >= prebuffer_) {
        SDL_PauseAudioDevice(dev_, 0);
        started_ = true;
    }
    return true;
}

size_t SdlAudioBackend::queued_bytes() {
    return dev_ ? SDL_GetQueuedAudioSize(dev_) : 0;
}

void SdlAudioBackend::pause(bool paused) {
    if (dev_) SDL_PauseAudioDevice(dev_, paused ? 1 : 0);
}

void SdlAudioBackend::clear() {
    rem_len_ = 0;
    started_ = false;
    if (dev_) {
        SDL_ClearQueuedAudio(dev_);
        SDL_PauseAudioDevice(dev_, 1); // re-prebuffer on next stream
    }
}

} // namespace audio
