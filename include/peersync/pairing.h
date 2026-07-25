#ifndef PEERSYNC_PAIRING_H
#define PEERSYNC_PAIRING_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace peersync {
namespace pairing {

// Generates a random 6-digit numeric PIN (e.g., "042918") using a cryptographically
// reasonable random source (std::random_device / std::mt19937).
std::string generatePin();

// Derives a fixed-length session key (default 32 bytes / 256 bits) from a PIN and salt
// using PBKDF2-HMAC-SHA256 with the specified iteration count (default 4096).
std::vector<uint8_t> deriveSessionKey(const std::string& pin,
                                      const std::string& salt,
                                      size_t iterations = 4096,
                                      size_t keyLength = 32);

// Standalone cryptographic helper functions (RFC 6234, RFC 2104, RFC 2898)
// Exposed for verification testing and protocol use.
std::vector<uint8_t> sha256(const uint8_t* data, size_t len);
std::vector<uint8_t> sha256(const std::string& data);
std::vector<uint8_t> sha256(const std::vector<uint8_t>& data);

std::vector<uint8_t> hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen);
std::vector<uint8_t> hmacSha256(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data);
std::vector<uint8_t> hmacSha256(const std::string& key, const std::string& data);

std::vector<uint8_t> pbkdf2HmacSha256(const uint8_t* pass, size_t passLen,
                                      const uint8_t* salt, size_t saltLen,
                                      size_t iterations,
                                      size_t keyLength);

} // namespace pairing

// Make primary pairing functions available directly in peersync namespace
using pairing::generatePin;
using pairing::deriveSessionKey;

} // namespace peersync

#endif // PEERSYNC_PAIRING_H
