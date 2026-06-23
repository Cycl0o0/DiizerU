#include "deezer_decrypt.h"

#include <cstring>

#include <mbedtls/md5.h>

namespace audio {

namespace {
const unsigned char SECRET[16] = {'g', '4', 'e', 'l', '5', '8', 'w', 'c',
                                  '0', 'z', 'v', 'f', '9', 'n', 'a', '1'};
const unsigned char IV[8] = {0, 1, 2, 3, 4, 5, 6, 7};
const size_t CHUNK = 2048;

void md5_hex(const std::string& s, unsigned char hex[32]) {
    unsigned char digest[16];
    // mbedTLS 2.x non-deprecated one-shot; returns 0 on success.
    mbedtls_md5_ret(reinterpret_cast<const unsigned char*>(s.data()), s.size(), digest);
    static const char* H = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        hex[i * 2] = (unsigned char)H[(digest[i] >> 4) & 0x0f];
        hex[i * 2 + 1] = (unsigned char)H[digest[i] & 0x0f];
    }
}
} // namespace

void deezer_blowfish_key(const std::string& track_id, unsigned char key[16]) {
    unsigned char hex[32];
    md5_hex(track_id, hex);
    for (int i = 0; i < 16; ++i) key[i] = hex[i] ^ hex[i + 16] ^ SECRET[i];
}

DeezerStripeDecryptor::DeezerStripeDecryptor() {
    mbedtls_blowfish_init(&ctx_);
    std::memset(key_, 0, sizeof(key_));
}

DeezerStripeDecryptor::~DeezerStripeDecryptor() {
    mbedtls_blowfish_free(&ctx_);
}

void DeezerStripeDecryptor::init(const std::string& track_id) {
    deezer_blowfish_key(track_id, key_);
    mbedtls_blowfish_setkey(&ctx_, key_, 128);
    buf_.clear();
    chunk_index_ = 0;
}

// CBC over a full 2048-byte chunk with a fixed IV reset per chunk, matching
// OpenSSL BF-CBC / the relay. ECB-decrypt each 8-byte block then XOR the
// previous ciphertext block (IV for the first).
void DeezerStripeDecryptor::decrypt_chunk(const unsigned char* in, unsigned char* out) {
    unsigned char prev[8];
    std::memcpy(prev, IV, 8);
    for (size_t off = 0; off + 8 <= CHUNK; off += 8) {
        unsigned char ct[8], pt[8];
        std::memcpy(ct, in + off, 8);
        mbedtls_blowfish_crypt_ecb(&ctx_, MBEDTLS_BLOWFISH_DECRYPT, ct, pt);
        for (int k = 0; k < 8; ++k) out[off + k] = pt[k] ^ prev[k];
        std::memcpy(prev, ct, 8);
    }
}

void DeezerStripeDecryptor::feed(const unsigned char* data, size_t n,
                                 std::vector<unsigned char>& out) {
    size_t i = 0;
    // If a chunk is mid-fill, top it up first.
    while (n - i > 0) {
        size_t need = CHUNK - buf_.size();
        size_t take = (n - i < need) ? (n - i) : need;
        buf_.insert(buf_.end(), data + i, data + i + take);
        i += take;
        if (buf_.size() == CHUNK) {
            if (chunk_index_ % 3 == 0) {
                unsigned char dec[2048];
                decrypt_chunk(buf_.data(), dec);
                out.insert(out.end(), dec, dec + CHUNK);
            } else {
                out.insert(out.end(), buf_.begin(), buf_.end());
            }
            ++chunk_index_;
            buf_.clear();
        }
    }
}

void DeezerStripeDecryptor::finish(std::vector<unsigned char>& out) {
    // A trailing partial chunk (< 2048) is always plaintext.
    if (!buf_.empty()) {
        out.insert(out.end(), buf_.begin(), buf_.end());
        buf_.clear();
    }
}

std::vector<unsigned char> deezer_decrypt_track(const std::string& track_id,
                                                const unsigned char* data, size_t n) {
    DeezerStripeDecryptor d;
    d.init(track_id);
    std::vector<unsigned char> out;
    out.reserve(n);
    d.feed(data, n, out);
    d.finish(out);
    return out;
}

bool deezer_decrypt_selftest() {
    // 1) key derivation vs known vector (track 3135556).
    static const unsigned char EXPECT[16] = {108, 108, 102, 107, 57, 102, 44, 55,
                                             101, 37, 117, 96, 60, 100, 52, 57};
    unsigned char k[16];
    deezer_blowfish_key("3135556", k);
    if (std::memcmp(k, EXPECT, 16) != 0) return false;

    // 2) chunks 1 and 2 (index % 3 != 0) pass through verbatim; chunk 0 changes.
    std::vector<unsigned char> data(CHUNK * 3);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (unsigned char)(i % 251);
    auto out = deezer_decrypt_track("3135556", data.data(), data.size());
    if (out.size() != data.size()) return false;
    if (std::memcmp(&out[CHUNK], &data[CHUNK], CHUNK * 2) != 0) return false; // 1,2 unchanged
    if (std::memcmp(&out[0], &data[0], CHUNK) == 0) return false;            // 0 changed
    return true;
}

} // namespace audio
