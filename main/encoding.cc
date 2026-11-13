#include "encoding.h"

#include <cstring>

std::string Base64Encode(const std::string& in) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t n = (uint8_t)in[i] << 16;
        if (i + 1 < in.size()) n |= (uint8_t)in[i + 1] << 8;
        if (i + 2 < in.size()) n |= (uint8_t)in[i + 2];
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += i + 1 < in.size() ? kTable[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kTable[n & 63] : '=';
    }
    return out;
}

std::string UrlEncode(const std::string& in) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

// FIPS 180-4
namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t Rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

void Compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
               (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + kRoundConstants[i] + w[i];
        uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

}  // namespace

std::array<uint8_t, 32> Sha256(const std::string& data) {
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t size = data.size();
    size_t full = size / 64 * 64;
    for (size_t i = 0; i < full; i += 64) {
        Compress(state, p + i);
    }

    // Padding: 0x80, zeros, then the bit length as a 64-bit big-endian number
    uint8_t tail[128] = {0};
    size_t rest = size - full;
    memcpy(tail, p + full, rest);
    tail[rest] = 0x80;
    size_t tail_size = rest + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)size * 8;
    for (int i = 0; i < 8; i++) {
        tail[tail_size - 1 - i] = bits >> (i * 8);
    }
    Compress(state, tail);
    if (tail_size == 128) {
        Compress(state, tail + 64);
    }

    std::array<uint8_t, 32> digest;
    for (int i = 0; i < 8; i++) {
        digest[i * 4] = state[i] >> 24;
        digest[i * 4 + 1] = state[i] >> 16;
        digest[i * 4 + 2] = state[i] >> 8;
        digest[i * 4 + 3] = state[i];
    }
    return digest;
}

// RFC 2104
std::array<uint8_t, 32> HmacSha256(const std::string& key, const std::string& message) {
    constexpr size_t kBlockSize = 64;
    std::string block_key = key;
    if (block_key.size() > kBlockSize) {
        auto digest = Sha256(block_key);
        block_key.assign(reinterpret_cast<const char*>(digest.data()), digest.size());
    }
    block_key.resize(kBlockSize, '\0');

    std::string inner(kBlockSize, '\0');
    std::string outer(kBlockSize, '\0');
    for (size_t i = 0; i < kBlockSize; i++) {
        inner[i] = block_key[i] ^ 0x36;
        outer[i] = block_key[i] ^ 0x5c;
    }
    auto inner_digest = Sha256(inner + message);
    outer.append(reinterpret_cast<const char*>(inner_digest.data()), inner_digest.size());
    return Sha256(outer);
}
