#include <peersync/pairing.h>
#include <random>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace peersync {
namespace pairing {

namespace {

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}
inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}
inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}
inline uint32_t ep0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}
inline uint32_t ep1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}
inline uint32_t sig0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}
inline uint32_t sig1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

void processSha256Block(uint32_t* state, const uint8_t* block) {
    uint32_t W[64];
    for (int t = 0; t < 16; ++t) {
        W[t] = (static_cast<uint32_t>(block[t * 4]) << 24) |
               (static_cast<uint32_t>(block[t * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[t * 4 + 2]) << 8) |
               (static_cast<uint32_t>(block[t * 4 + 3]));
    }
    for (int t = 16; t < 64; ++t) {
        W[t] = sig1(W[t - 2]) + W[t - 7] + sig0(W[t - 15]) + W[t - 16];
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (int t = 0; t < 64; ++t) {
        uint32_t t1 = h + ep1(e) + ch(e, f, g) + K[t] + W[t];
        uint32_t t2 = ep0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // anonymous namespace

std::string generatePin() {
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 999999);
    int val = dist(gen);
    char buf[16];
    snprintf(buf, sizeof(buf), "%06d", val);
    return std::string(buf);
}

std::vector<uint8_t> sha256(const uint8_t* data, size_t len) {
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    std::vector<uint8_t> padded(data, data + len);
    padded.push_back(0x80);
    while ((padded.size() % 64) != 56) {
        padded.push_back(0x00);
    }
    uint64_t bitLen = static_cast<uint64_t>(len) * 8;
    for (int i = 7; i >= 0; --i) {
        padded.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFF));
    }

    for (size_t offset = 0; offset < padded.size(); offset += 64) {
        processSha256Block(state, &padded[offset]);
    }

    std::vector<uint8_t> hash(32);
    for (int i = 0; i < 8; ++i) {
        hash[i * 4]     = static_cast<uint8_t>((state[i] >> 24) & 0xFF);
        hash[i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16) & 0xFF);
        hash[i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8) & 0xFF);
        hash[i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xFF);
    }
    return hash;
}

std::vector<uint8_t> sha256(const std::string& data) {
    return sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::vector<uint8_t> sha256(const std::vector<uint8_t>& data) {
    return sha256(data.data(), data.size());
}

std::vector<uint8_t> hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen) {
    std::vector<uint8_t> k(key, key + keyLen);
    if (k.size() > 64) {
        k = sha256(k.data(), k.size());
    }
    while (k.size() < 64) {
        k.push_back(0);
    }

    std::vector<uint8_t> i_key_pad(64);
    std::vector<uint8_t> o_key_pad(64);
    for (size_t i = 0; i < 64; ++i) {
        i_key_pad[i] = k[i] ^ 0x36;
        o_key_pad[i] = k[i] ^ 0x5c;
    }

    std::vector<uint8_t> inner_msg = i_key_pad;
    inner_msg.insert(inner_msg.end(), data, data + dataLen);
    std::vector<uint8_t> inner_hash = sha256(inner_msg.data(), inner_msg.size());

    std::vector<uint8_t> outer_msg = o_key_pad;
    outer_msg.insert(outer_msg.end(), inner_hash.begin(), inner_hash.end());
    return sha256(outer_msg.data(), outer_msg.size());
}

std::vector<uint8_t> hmacSha256(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data) {
    return hmacSha256(key.data(), key.size(), data.data(), data.size());
}

std::vector<uint8_t> hmacSha256(const std::string& key, const std::string& data) {
    return hmacSha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                      reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::vector<uint8_t> pbkdf2HmacSha256(const uint8_t* pass, size_t passLen,
                                      const uint8_t* salt, size_t saltLen,
                                      size_t iterations,
                                      size_t keyLength) {
    std::vector<uint8_t> result;
    result.reserve(keyLength);

    uint32_t blockIndex = 1;
    while (result.size() < keyLength) {
        std::vector<uint8_t> salt_and_index(salt, salt + saltLen);
        salt_and_index.push_back(static_cast<uint8_t>((blockIndex >> 24) & 0xFF));
        salt_and_index.push_back(static_cast<uint8_t>((blockIndex >> 16) & 0xFF));
        salt_and_index.push_back(static_cast<uint8_t>((blockIndex >> 8) & 0xFF));
        salt_and_index.push_back(static_cast<uint8_t>(blockIndex & 0xFF));

        std::vector<uint8_t> u = hmacSha256(pass, passLen, salt_and_index.data(), salt_and_index.size());
        std::vector<uint8_t> t = u;

        for (size_t iter = 1; iter < iterations; ++iter) {
            u = hmacSha256(pass, passLen, u.data(), u.size());
            for (size_t j = 0; j < 32; ++j) {
                t[j] ^= u[j];
            }
        }

        size_t needed = keyLength - result.size();
        size_t toCopy = (needed < 32) ? needed : 32;
        result.insert(result.end(), t.begin(), t.begin() + toCopy);
        blockIndex++;
    }

    return result;
}

std::vector<uint8_t> deriveSessionKey(const std::string& pin,
                                      const std::string& salt,
                                      size_t iterations,
                                      size_t keyLength) {
    return pbkdf2HmacSha256(reinterpret_cast<const uint8_t*>(pin.data()), pin.size(),
                            reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
                            iterations, keyLength);
}

} // namespace pairing
} // namespace peersync
