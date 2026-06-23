// audio/adpcm — IMA ADPCM decoder (mirrors the relay's ImaAdpcmEncoder).
// Decodes a continuous 4-bit nibble stream (L,R,L,R...) to interleaved s16le.
// Stateful across chunks; reliable (TCP) transport keeps it in sync from byte 0.
// No SDL/platform deps -> unit-testable.
#pragma once

#include <cstdint>
#include <vector>

namespace audio {

class AdpcmDecoder {
public:
    void reset() {
        ch_[0] = Ch{};
        ch_[1] = Ch{};
        parity_ = 0;
    }

    // Decode `len` ADPCM bytes; append interleaved s16le PCM to `out`.
    void decode(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
        out.reserve(out.size() + len * 4); // 1 byte -> 2 samples -> 4 PCM bytes
        for (size_t i = 0; i < len; ++i) {
            uint8_t b = data[i];
            push_sample(b & 0x0f, out);
            push_sample((b >> 4) & 0x0f, out);
        }
    }

private:
    struct Ch {
        int32_t pred = 0;
        int32_t idx = 0;
    };

    void push_sample(uint8_t code, std::vector<uint8_t>& out) {
        int16_t s = decode_nibble(ch_[parity_], code);
        out.push_back((uint8_t)(s & 0xff));
        out.push_back((uint8_t)((s >> 8) & 0xff));
        parity_ ^= 1; // L,R,L,R...
    }

    static int16_t decode_nibble(Ch& c, uint8_t code) {
        int step = kStep[c.idx];
        int vp = step >> 3;
        if (code & 4) vp += step;
        if (code & 2) vp += step >> 1;
        if (code & 1) vp += step >> 2;
        if (code & 8)
            c.pred -= vp;
        else
            c.pred += vp;
        if (c.pred > 32767) c.pred = 32767;
        if (c.pred < -32768) c.pred = -32768;
        c.idx += kIndex[code];
        if (c.idx < 0) c.idx = 0;
        if (c.idx > 88) c.idx = 88;
        return (int16_t)c.pred;
    }

    Ch ch_[2];
    int parity_ = 0;

    static constexpr int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                       -1, -1, -1, -1, 2, 4, 6, 8};
    static constexpr int kStep[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
        66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
        408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
        2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
        8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
        29794, 32767};
};

} // namespace audio
