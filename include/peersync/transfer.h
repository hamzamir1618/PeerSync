#ifndef PEERSYNC_TRANSFER_H
#define PEERSYNC_TRANSFER_H

#include <peersync/socket.h>
#include <peersync/protocol.h>
#include <peersync/delta.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace peersync {

class TransferSession {
public:
    struct Config {
        size_t blockSize = 1024;                // Block size for delta signatures and chunking
        size_t literalThreshold = 1024;         // Send literal bytes > threshold as BlockData messages
        size_t maxInstructionsPerMessage = 500; // Max instructions in a single DeltaInstructions message
        bool allowResume = true;                // Whether automatic transfer resumption is permitted
        std::function<void(uint64_t bytesSent, uint64_t fileBytesProcessed, uint64_t totalFileSize)> progressCallback = nullptr;
        std::function<void(const std::string& relPath, bool isResuming, uint64_t resumedBytes, uint64_t totalFileSize)> onResumeDetected = nullptr;
        std::function<bool()> isCancelled = nullptr; // Returns true if transfer should be aborted immediately
    };

    explicit TransferSession(TcpSocket& socket);
    TransferSession(TcpSocket& socket, Config config);

    // Sender role: initiate transfer of localFile to peer, named relativePath on the receiver
    bool sendFile(const std::filesystem::path& localFile, const std::string& relativePath);

    // Receiver role: listen for incoming transfer request and save/reconstruct file inside localDir
    bool receiveFile(const std::filesystem::path& localDir);

    // Statistics & verification getters
    uint64_t getBytesSent() const { return m_bytesSent; }
    uint64_t getBytesReceived() const { return m_bytesReceived; }
    uint64_t getInstructionsApplied() const { return m_instructionsApplied; }
    std::string getFinalHash() const { return m_finalHash; }

    // Computes hex-encoded xxHash64 of a file
    static std::string computeFileHash(const std::filesystem::path& file);

private:
    void sendMsg(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> recvMsg();

    TcpSocket& m_socket;
    Config m_config;
    uint64_t m_bytesSent;
    uint64_t m_bytesReceived;
    uint64_t m_instructionsApplied;
    std::string m_finalHash;
};

} // namespace peersync

#endif // PEERSYNC_TRANSFER_H
