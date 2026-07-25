#ifndef PEERSYNC_PROTOCOL_H
#define PEERSYNC_PROTOCOL_H

#include <cstdint>
#include <string>
#include <vector>
#include <peersync/socket.h>
#include <peersync/delta.h>

namespace peersync {

enum class MessageType : uint8_t {
    Hello = 1,
    PairChallenge = 2,
    PairResponse = 3,
    PairResult = 4,
    ManifestRequest = 5,
    ManifestResponse = 6,
    DeltaInstructions = 7,
    BlockData = 8,
    TransferAck = 9,
    ResumeRequest = 10,
    ResumeResponse = 11,
    TransferComplete = 12,
    ErrorMessage = 13
};

struct HelloMessage {
    std::string deviceName;
    std::string protocolVersion;
};

struct PairChallengeMessage {
    std::vector<uint8_t> challengeBytes;
};

struct PairResponseMessage {
    std::vector<uint8_t> responseBytes;
};

struct PairResultMessage {
    bool success;
    std::string errorMessage;
};

struct ManifestRequestMessage {
    std::string path;
};

struct FileEntry {
    std::string relativePath;
    uint64_t fileSize;
    std::string sha256Hash;
    uint64_t lastModified;
};

struct ManifestResponseMessage {
    std::vector<FileEntry> files;
    std::vector<BlockSignature> signatures;
};


struct DeltaInstructionsMessage {
    std::string relativePath;
    uint64_t targetFileSize;
    uint32_t blockSize;
    std::vector<DeltaInstruction> instructions;
};

struct BlockDataMessage {
    std::string relativePath;
    uint64_t offset;
    std::vector<uint8_t> data;
};

struct TransferAckMessage {
    std::string relativePath;
    uint64_t bytesReceived;
};

struct ResumeRequestMessage {
    std::string relativePath;
    std::string fileHash;
    uint64_t lastOffset;
    std::vector<BlockSignature> signatures;
};

struct ResumeResponseMessage {
    std::string relativePath;
    bool canResume;
    uint64_t resumeOffset;
};

struct TransferCompleteMessage {
    std::string relativePath;
    bool success;
    std::string finalHash;
};

struct ErrorMessageMessage {
    uint32_t errorCode;
    std::string errorMessage;
};

// Inspection and validation
MessageType getMessageType(const std::vector<uint8_t>& payload);

// Serialization functions
std::vector<uint8_t> serializeMessage(const HelloMessage& msg);
std::vector<uint8_t> serializeMessage(const PairChallengeMessage& msg);
std::vector<uint8_t> serializeMessage(const PairResponseMessage& msg);
std::vector<uint8_t> serializeMessage(const PairResultMessage& msg);
std::vector<uint8_t> serializeMessage(const ManifestRequestMessage& msg);
std::vector<uint8_t> serializeMessage(const ManifestResponseMessage& msg);
std::vector<uint8_t> serializeMessage(const DeltaInstructionsMessage& msg);
std::vector<uint8_t> serializeMessage(const BlockDataMessage& msg);
std::vector<uint8_t> serializeMessage(const TransferAckMessage& msg);
std::vector<uint8_t> serializeMessage(const ResumeRequestMessage& msg);
std::vector<uint8_t> serializeMessage(const ResumeResponseMessage& msg);
std::vector<uint8_t> serializeMessage(const TransferCompleteMessage& msg);
std::vector<uint8_t> serializeMessage(const ErrorMessageMessage& msg);

// Deserialization functions
HelloMessage deserializeHelloMessage(const std::vector<uint8_t>& payload);
PairChallengeMessage deserializePairChallengeMessage(const std::vector<uint8_t>& payload);
PairResponseMessage deserializePairResponseMessage(const std::vector<uint8_t>& payload);
PairResultMessage deserializePairResultMessage(const std::vector<uint8_t>& payload);
ManifestRequestMessage deserializeManifestRequestMessage(const std::vector<uint8_t>& payload);
ManifestResponseMessage deserializeManifestResponseMessage(const std::vector<uint8_t>& payload);
DeltaInstructionsMessage deserializeDeltaInstructionsMessage(const std::vector<uint8_t>& payload);
BlockDataMessage deserializeBlockDataMessage(const std::vector<uint8_t>& payload);
TransferAckMessage deserializeTransferAckMessage(const std::vector<uint8_t>& payload);
ResumeRequestMessage deserializeResumeRequestMessage(const std::vector<uint8_t>& payload);
ResumeResponseMessage deserializeResumeResponseMessage(const std::vector<uint8_t>& payload);
TransferCompleteMessage deserializeTransferCompleteMessage(const std::vector<uint8_t>& payload);
ErrorMessageMessage deserializeErrorMessageMessage(const std::vector<uint8_t>& payload);

// Typed socket messaging layer
void sendMessage(TcpSocket& sock, const HelloMessage& msg);
void sendMessage(TcpSocket& sock, const PairChallengeMessage& msg);
void sendMessage(TcpSocket& sock, const PairResponseMessage& msg);
void sendMessage(TcpSocket& sock, const PairResultMessage& msg);
void sendMessage(TcpSocket& sock, const ManifestRequestMessage& msg);
void sendMessage(TcpSocket& sock, const ManifestResponseMessage& msg);
void sendMessage(TcpSocket& sock, const DeltaInstructionsMessage& msg);
void sendMessage(TcpSocket& sock, const BlockDataMessage& msg);
void sendMessage(TcpSocket& sock, const TransferAckMessage& msg);
void sendMessage(TcpSocket& sock, const ResumeRequestMessage& msg);
void sendMessage(TcpSocket& sock, const ResumeResponseMessage& msg);
void sendMessage(TcpSocket& sock, const TransferCompleteMessage& msg);
void sendMessage(TcpSocket& sock, const ErrorMessageMessage& msg);

MessageType peekNextMessageType(TcpSocket& sock, std::vector<uint8_t>& outRawPayload);

} // namespace peersync

#endif // PEERSYNC_PROTOCOL_H
