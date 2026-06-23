// Host test: StreamPlayer pulls PCM from a running relay into a mock backend.
// Verifies the relay->client audio path end-to-end (throughput ~= 44100*2*2 B/s)
// WITHOUT SDL or a Wii U. Run the relay with DEV_SEED_TOKEN first.
//
//   g++ -std=gnu++17 audio_test.cpp ../src/audio/stream_player.cpp \
//       -I../src -lcurl -pthread -o audio_test
//   ./audio_test http://127.0.0.1:8099/v1 devtoken

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "audio/iaudio_backend.h"
#include "audio/stream_player.h"

// Mock sink: counts bytes, pretends to drain instantly (no throttle in test).
class CountingBackend : public audio::IAudioBackend {
public:
    bool init(const audio::AudioFormat&) override { return true; }
    void shutdown() override {}
    bool queue(const uint8_t*, size_t len) override {
        total_.fetch_add(len);
        return true;
    }
    size_t queued_bytes() override { return 0; }
    void pause(bool) override {}
    void clear() override {}
    unsigned long long total() const { return total_.load(); }

private:
    std::atomic<unsigned long long> total_{0};
};

int main(int argc, char** argv) {
    std::string base = argc > 1 ? argv[1] : "http://127.0.0.1:8099/v1";
    std::string token = argc > 2 ? argv[2] : "devtoken";

    CountingBackend backend;
    audio::StreamPlayer player(backend);

    std::printf("streaming for ~2s from %s ...\n", base.c_str());
    player.start(base, token);
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    player.stop();

    unsigned long long got = player.bytes_received();
    // expected ~ 44100 * 2ch * 2B * 2s = 352800; allow generous tolerance for
    // startup + scheduling.
    const double expected = 352800.0;
    double ratio = got / expected;
    std::printf("bytes_received=%llu (mock sink saw=%llu) ratio=%.2f\n", got, backend.total(),
                ratio);
    if (!player.last_error().empty())
        std::printf("error: %s\n", player.last_error().c_str());

    bool ok = got > 100000 && ratio > 0.5 && ratio < 1.6 && player.last_error().empty();
    std::printf("%s\n", ok ? "AUDIO E2E PASS" : "AUDIO E2E FAIL");
    return ok ? 0 : 1;
}
