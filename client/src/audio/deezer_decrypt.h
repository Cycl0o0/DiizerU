// audio/deezer_decrypt — Deezer track decryption on the console.
//
// Deezer ships tracks with every 3rd 2048-byte chunk Blowfish-CBC encrypted
// (the rest plaintext), keyed off the track id. This is a faithful C++ port of
// the relay's deezer/crypto.rs, using mbedTLS for MD5 + Blowfish so the ARL and
// the decrypt stay entirely on the Wii U (relay-optional path).
//
// Streaming: feed() accepts bytes as they arrive from the CDN and appends the
// decrypted/plaintext output; finish() flushes the trailing partial chunk.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <mbedtls/blowfish.h>

namespace audio {

// Derive the 16-byte Blowfish key for a track id (Deezer scheme):
//   hex = lowercase md5(track_id ascii) as 32 hex chars
//   key[i] = hex[i] ^ hex[i+16] ^ SECRET[i]
void deezer_blowfish_key(const std::string& track_id, unsigned char key[16]);

class DeezerStripeDecryptor {
public:
    DeezerStripeDecryptor();
    ~DeezerStripeDecryptor();

    // Set the track id (derives the key, resets chunk position).
    void init(const std::string& track_id);

    // Append decrypted/plaintext output for the raw bytes in [data, data+n).
    void feed(const unsigned char* data, size_t n, std::vector<unsigned char>& out);
    // Flush the final partial chunk (< 2048 bytes are always plaintext).
    void finish(std::vector<unsigned char>& out);

private:
    void decrypt_chunk(const unsigned char* in, unsigned char* out); // 2048 bytes

    mbedtls_blowfish_context ctx_;
    unsigned char key_[16];
    std::vector<unsigned char> buf_; // accumulates up to one 2048 chunk
    size_t chunk_index_ = 0;
};

// One-shot (used by the host self-test). Equivalent to feed-all + finish.
std::vector<unsigned char> deezer_decrypt_track(const std::string& track_id,
                                                const unsigned char* data, size_t n);

// Host self-test: validates the key derivation against a known vector and the
// chunk passthrough rule. Returns true on pass. Compiled out on the console.
bool deezer_decrypt_selftest();

} // namespace audio
