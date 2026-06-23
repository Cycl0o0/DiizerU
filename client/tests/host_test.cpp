// Host-side test for core/ against a running relay. Builds with system g++ +
// libcurl (NOT devkitPro) — proves the relay API client + pairing + session
// store logic without a Wii U. See tests/run.sh.
//
//   g++ -std=gnu++17 host_test.cpp ../src/core/*.cpp ../third_party/cjson/cJSON.c \
//       -I../src -lcurl -o host_test
//   ./host_test http://127.0.0.1:8099/v1 devtoken

#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/pairing.h"
#include "core/relay_client.h"
#include "core/session_store.h"

static int failures = 0;
#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) {                                                 \
            std::printf("  PASS: %s\n", msg);                       \
        } else {                                                    \
            std::printf("  FAIL: %s\n", msg);                       \
            ++failures;                                             \
        }                                                           \
    } while (0)

int main(int argc, char** argv) {
    std::string base = argc > 1 ? argv[1] : "http://127.0.0.1:8099/v1";
    std::string dev_token = argc > 2 ? argv[2] : "";

    core::RelayClient client(base);

    std::printf("[1] capabilities\n");
    auto cap = client.capabilities();
    CHECK(cap.has_value(), "capabilities reachable");
    if (cap) {
        std::printf("    api=%s mode=%s rate=%d ch=%d fmts=%zu\n",
                    cap->api_version.c_str(), cap->relay_mode.c_str(),
                    cap->sample_rate, cap->channels, cap->audio_formats.size());
        CHECK(cap->sample_rate == 44100 && cap->channels == 2, "44.1k stereo");
        CHECK(!cap->audio_formats.empty(), "advertises an audio format");
    }

    std::printf("[2] pair/start\n");
    auto ps = client.pair_start("Host Test");
    CHECK(ps.has_value(), "pair_start ok");
    if (ps) {
        std::printf("    user_code=%s verify=%s interval=%d\n",
                    ps->user_code.c_str(), ps->verify_url.c_str(), ps->interval);
        CHECK(!ps->device_code.empty() && !ps->user_code.empty(), "got codes");

        std::printf("[3] pair/poll (expect pending)\n");
        auto pp = client.pair_poll(ps->device_code);
        CHECK(pp.has_value(), "pair_poll ok");
        CHECK(pp && pp->status == core::PairStatus::Pending, "status pending");
    }

    std::printf("[4] session store roundtrip\n");
    core::SessionStore store("/tmp/diizeru_test");
    CHECK(store.save_relay_url(base), "save relay url");
    auto u = store.load_relay_url();
    CHECK(u && *u == base, "load relay url matches");
    CHECK(store.save_token("tok-abc"), "save token");
    auto t = store.load_token();
    CHECK(t && *t == "tok-abc", "load token matches");
    store.clear_token();
    CHECK(!store.load_token().has_value(), "clear token");

    if (!dev_token.empty()) {
        std::printf("[5] bearer auth path (dev token)\n");
        client.set_bearer(dev_token);
        auto m = client.me(); // dev-seed user has no profile record -> 401 -> nullopt
        std::printf("    me() present=%d (dev-seed expected empty)\n", m.has_value());
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
