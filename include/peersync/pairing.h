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

enum class PairingRole {
    Initiator,
    Responder
};

enum class PairingState {
    Initial,
    WaitingForResponse,
    WaitingForResult,
    Authenticated,
    Failed
};

class PairingSession {
public:
    PairingSession(PairingRole role, const std::string& pin);
    ~PairingSession() = default;

    // Initiator starts by generating a random challenge nonce and enqueuing PairChallengeMessage.
    void start();

    // Feeds an incoming serialized message into the state machine, advancing state and generating responses.
    void processMessage(const std::vector<uint8_t>& serializedMessage);

    // Outgoing serialized message queue for transport over pipes/sockets.
    bool hasOutgoingMessage() const;
    std::vector<uint8_t> popOutgoingMessage();

    // State query methods
    PairingState getState() const;
    bool isAuthenticated() const;
    bool isFinished() const;
    bool isFailed() const;

    // Returns derived session key if isAuthenticated() == true, otherwise returns empty vector.
    std::vector<uint8_t> getSessionKey() const;
    std::string getErrorMessage() const;

    // Helper for testing replay protection across independent test runs
    static void clearSeenNonces();

private:
    PairingRole role_;
    PairingState state_;
    std::string pin_;
    std::vector<uint8_t> sessionKey_;
    std::vector<uint8_t> currentNonce_;
    std::string errorMessage_;
    std::vector<std::vector<uint8_t>> outgoingQueue_;

    void fail(const std::string& reason);
};

} // namespace pairing

// Make primary pairing functions and classes available directly in peersync namespace
using pairing::generatePin;
using pairing::deriveSessionKey;
using pairing::PairingRole;
using pairing::PairingState;
using pairing::PairingSession;

} // namespace peersync

#endif // PEERSYNC_PAIRING_H
