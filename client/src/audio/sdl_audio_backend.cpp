#include "sdl_audio_backend.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace audio {

// One-shot probe: open the device letting SDL report the REAL hardware spec
// (allowed_changes=0 on the playback device hides it). Writes the true rate so
// a later build can match it exactly and skip SDL's internal resampler.
static void probe_hw_rate(int want_freq) {
    SDL_AudioSpec want{};
    want.freq = want_freq;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 2048;
    want.callback = nullptr;
    SDL_AudioSpec have{};
    SDL_AudioDeviceID d = SDL_OpenAudioDevice(nullptr, 0, &want, &have,
                                              SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (d == 0) {
        std::printf("[audio] probe open failed: %s\n", SDL_GetError());
        return;
    }
    std::printf("[audio] PROBE real device: freq=%d ch=%d fmt=%04x samples=%d\n",
                have.freq, have.channels, have.format, have.samples);
#ifdef __WIIU__
    if (FILE* f = std::fopen("fs:/vol/external01/diizeru/audio_probe.txt", "w")) {
        std::fprintf(f, "real freq=%d ch=%d fmt=%04x samples=%d\n",
                     have.freq, have.channels, have.format, have.samples);
        std::fclose(f);
    }
#endif
    SDL_CloseAudioDevice(d);
}

bool SdlAudioBackend::init(const AudioFormat& fmt) {
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::printf("[audio] SDL audio init failed: %s\n", SDL_GetError());
        return false;
    }

    probe_hw_rate(fmt.sample_rate); // diagnostic only

    frame_bytes_ = fmt.channels * fmt.bytes_per_sample; // s16 stereo = 4

    SDL_AudioSpec want{};
    want.freq = fmt.sample_rate;
    want.format = AUDIO_S16LSB; // s16le
    want.channels = (Uint8)fmt.channels;
    want.samples = 2048;        // callback period (~46ms @22050, ~46ms @44100)
    want.callback = &SdlAudioBackend::audio_cb;
    want.userdata = this;

    SDL_AudioSpec have{};
    // allowed_changes=0: SDL guarantees have==want, inserting an internal
    // resampler if the hardware rate differs. Pull model means that resampler is
    // demand-driven at real time, so a backlog can never be flushed fast.
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev_ == 0) {
        std::printf("[audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }
    have_ = have;
    std::printf("[audio] want freq=%d ch=%d fmt=%04x | have freq=%d ch=%d fmt=%04x samples=%d\n",
                fmt.sample_rate, fmt.channels, AUDIO_S16LSB, have.freq, have.channels,
                have.format, have.samples);

    int pre_ms = fmt.prebuffer_ms > 0 ? fmt.prebuffer_ms : 500;
    prebuffer_ = (size_t)fmt.sample_rate * frame_bytes_ * pre_ms / 1000;

    // Ring sized to ~10s; the curl write callback throttles on queued_bytes()
    // well below this, so queue() never has to drop.
    cap_ = (size_t)fmt.sample_rate * frame_bytes_ * 10;
    ring_.assign(cap_, 0);
    head_ = tail_ = avail_ = 0;
    playing_ = false;

    // Run the device immediately; the callback emits silence until the prebuffer
    // cushion is reached, so there is no paused->unpaused backlog to rush.
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void SdlAudioBackend::shutdown() {
    if (dev_) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
}

// SDL audio thread. Pulls exactly `len` bytes of real-time audio per period.
void SdlAudioBackend::audio_cb(void* userdata, Uint8* stream, int len) {
    static_cast<SdlAudioBackend*>(userdata)->fill(stream, len);
}

void SdlAudioBackend::fill(Uint8* stream, int len) {
    std::lock_guard<std::mutex> lk(m_);
    // Hold silence until the cushion is built (per track; clear() re-arms this).
    if (!playing_) {
        if (avail_ >= prebuffer_ && prebuffer_ > 0) playing_ = true;
        else { std::memset(stream, 0, (size_t)len); return; }
    }
    size_t n = std::min((size_t)len, avail_);
    size_t first = std::min(n, cap_ - head_);
    std::memcpy(stream, ring_.data() + head_, first);
    if (n > first) std::memcpy(stream + first, ring_.data(), n - first);
    head_ = (head_ + n) % cap_;
    avail_ -= n;
    if ((size_t)len > n) std::memset(stream + n, 0, (size_t)len - n); // underrun -> silence
}

bool SdlAudioBackend::queue(const uint8_t* data, size_t len) {
    if (!dev_) return false;
    std::lock_guard<std::mutex> lk(m_);
    size_t space = cap_ - avail_;
    size_t n = std::min(len, space);
    // On the rare overflow (throttle lagging) drop whole frames to stay aligned.
    n -= n % (size_t)frame_bytes_;
    size_t first = std::min(n, cap_ - tail_);
    std::memcpy(ring_.data() + tail_, data, first);
    if (n > first) std::memcpy(ring_.data(), data + first, n - first);
    tail_ = (tail_ + n) % cap_;
    avail_ += n;
    return true;
}

size_t SdlAudioBackend::queued_bytes() {
    std::lock_guard<std::mutex> lk(m_);
    return avail_;
}

void SdlAudioBackend::pause(bool paused) {
    if (dev_) SDL_PauseAudioDevice(dev_, paused ? 1 : 0);
}

void SdlAudioBackend::clear() {
    std::lock_guard<std::mutex> lk(m_);
    head_ = tail_ = avail_ = 0;
    playing_ = false; // re-arm the prebuffer gate for the next track
}

} // namespace audio
