#include <peersync/protocol.h>
#include <peersync/exceptions.h>
#include <peersync/message_framing.h>

namespace peersync {

namespace {

void writeU8(std::vector<uint8_t>& buf, uint8_t val) {
    buf.push_back(val);
}

void writeU16(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

void writeU32(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

void writeU64(std::vector<uint8_t>& buf, uint64_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 56) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 48) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 40) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 32) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

void writeBool(std::vector<uint8_t>& buf, bool val) {
    writeU8(buf, val ? 1 : 0);
}

void writeString(std::vector<uint8_t>& buf, const std::string& str) {
    writeU32(buf, static_cast<uint32_t>(str.size()));
    buf.insert(buf.end(), str.begin(), str.end());
}

void writeBytes(std::vector<uint8_t>& buf, const std::vector<uint8_t>& bytes) {
    writeU32(buf, static_cast<uint32_t>(bytes.size()));
    buf.insert(buf.end(), bytes.begin(), bytes.end());
}

class BinaryReader {
public:
    explicit BinaryReader(const std::vector<uint8_t>& data) : m_data(data), m_offset(0) {}

    uint8_t readU8() {
        if (m_offset >= m_data.size()) throw PeerSyncProtocolException("Unexpected EOF while reading uint8_t");
        return m_data[m_offset++];
    }

    uint16_t readU16() {
        if (m_offset + 2 > m_data.size()) throw PeerSyncProtocolException("Unexpected EOF while reading uint16_t");
        uint16_t val = (static_cast<uint16_t>(m_data[m_offset]) << 8) |
                       (static_cast<uint16_t>(m_data[m_offset + 1]));
        m_offset += 2;
        return val;
    }

    uint32_t readU32() {
        if (m_offset + 4 > m_data.size()) throw PeerSyncProtocolException("Unexpected EOF while reading uint32_t");
        uint32_t val = (static_cast<uint32_t>(m_data[m_offset]) << 24) |
                       (static_cast<uint32_t>(m_data[m_offset + 1]) << 16) |
                       (static_cast<uint32_t>(m_data[m_offset + 2]) << 8) |
                       (static_cast<uint32_t>(m_data[m_offset + 3]));
        m_offset += 4;
        return val;
    }

    uint64_t readU64() {
        if (m_offset + 8 > m_data.size()) throw PeerSyncProtocolException("Unexpected EOF while reading uint64_t");
        uint64_t val = (static_cast<uint64_t>(m_data[m_offset]) << 56) |
                       (static_cast<uint64_t>(m_data[m_offset + 1]) << 48) |
                       (static_cast<uint64_t>(m_data[m_offset + 2]) << 40) |
                       (static_cast<uint64_t>(m_data[m_offset + 3]) << 32) |
                       (static_cast<uint64_t>(m_data[m_offset + 4]) << 24) |
                       (static_cast<uint64_t>(m_data[m_offset + 5]) << 16) |
                       (static_cast<uint64_t>(m_data[m_offset + 6]) << 8) |
                       (static_cast<uint64_t>(m_data[m_offset + 7]));
        m_offset += 8;
        return val;
    }

    bool readBool() {
        return readU8() != 0;
    }

    std::string readString() {
        uint32_t len = readU32();
        if (m_offset + len > m_data.size()) throw PeerSyncProtocolException("Unexpected EOF while reading string of length " + std::to_string(len));
        std::string str(reinterpret_cast<const char*>(m_data.data() + m_offset), len);
        m_offset += len;
        return str;
    }

    std::vector<uint8_t> readBytes() {
        uint32_t len = readU32();
        if (m_offset + len > m_data.size()) throw PeerSyncProtocolException("Unexpected EOF while reading byte vector of length " + std::to_string(len));
        std::vector<uint8_t> bytes(m_data.begin() + m_offset, m_data.begin() + m_offset + len);
        m_offset += len;
        return bytes;
    }

    void expectEOF() {
        if (m_offset != m_data.size()) {
            throw PeerSyncProtocolException("Trailing unparsed bytes in payload (" + std::to_string(m_data.size() - m_offset) + " bytes remaining)");
        }
    }

    void expectMessageType(MessageType expectedType) {
        uint8_t tag = readU8();
        if (tag < 1 || tag > 13) {
            throw PeerSyncProtocolException("Unknown MessageType tag: " + std::to_string(tag));
        }
        MessageType actualType = static_cast<MessageType>(tag);
        if (actualType != expectedType) {
            throw PeerSyncProtocolException("MessageType tag mismatch: expected " + std::to_string(static_cast<int>(expectedType)) +
                                            ", got " + std::to_string(tag));
        }
    }

private:
    const std::vector<uint8_t>& m_data;
    size_t m_offset;
};

} // anonymous namespace

MessageType getMessageType(const std::vector<uint8_t>& payload) {
    if (payload.empty()) {
        throw PeerSyncProtocolException("Cannot read MessageType from empty payload");
    }
    uint8_t tag = payload[0];
    if (tag < 1 || tag > 13) {
        throw PeerSyncProtocolException("Unknown MessageType tag: " + std::to_string(tag));
    }
    return static_cast<MessageType>(tag);
}

// HelloMessage
std::vector<uint8_t> serializeMessage(const HelloMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::Hello));
    writeString(buf, msg.deviceName);
    writeString(buf, msg.protocolVersion);
    return buf;
}

HelloMessage deserializeHelloMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::Hello);
    HelloMessage msg;
    msg.deviceName = reader.readString();
    msg.protocolVersion = reader.readString();
    reader.expectEOF();
    return msg;
}

// PairChallengeMessage
std::vector<uint8_t> serializeMessage(const PairChallengeMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::PairChallenge));
    writeBytes(buf, msg.challengeBytes);
    return buf;
}

PairChallengeMessage deserializePairChallengeMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::PairChallenge);
    PairChallengeMessage msg;
    msg.challengeBytes = reader.readBytes();
    reader.expectEOF();
    return msg;
}

// PairResponseMessage
std::vector<uint8_t> serializeMessage(const PairResponseMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::PairResponse));
    writeBytes(buf, msg.responseBytes);
    return buf;
}

PairResponseMessage deserializePairResponseMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::PairResponse);
    PairResponseMessage msg;
    msg.responseBytes = reader.readBytes();
    reader.expectEOF();
    return msg;
}

// PairResultMessage
std::vector<uint8_t> serializeMessage(const PairResultMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::PairResult));
    writeBool(buf, msg.success);
    writeString(buf, msg.errorMessage);
    return buf;
}

PairResultMessage deserializePairResultMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::PairResult);
    PairResultMessage msg;
    msg.success = reader.readBool();
    msg.errorMessage = reader.readString();
    reader.expectEOF();
    return msg;
}

// ManifestRequestMessage
std::vector<uint8_t> serializeMessage(const ManifestRequestMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::ManifestRequest));
    writeString(buf, msg.path);
    return buf;
}

ManifestRequestMessage deserializeManifestRequestMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::ManifestRequest);
    ManifestRequestMessage msg;
    msg.path = reader.readString();
    reader.expectEOF();
    return msg;
}

// ManifestResponseMessage
std::vector<uint8_t> serializeMessage(const ManifestResponseMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::ManifestResponse));
    writeU32(buf, static_cast<uint32_t>(msg.files.size()));
    for (const auto& file : msg.files) {
        writeString(buf, file.relativePath);
        writeU64(buf, file.fileSize);
        writeString(buf, file.sha256Hash);
        writeU64(buf, file.lastModified);
    }
    return buf;
}

ManifestResponseMessage deserializeManifestResponseMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::ManifestResponse);
    ManifestResponseMessage msg;
    uint32_t count = reader.readU32();
    msg.files.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        FileEntry file;
        file.relativePath = reader.readString();
        file.fileSize = reader.readU64();
        file.sha256Hash = reader.readString();
        file.lastModified = reader.readU64();
        msg.files.push_back(file);
    }
    reader.expectEOF();
    return msg;
}

// DeltaInstructionsMessage
std::vector<uint8_t> serializeMessage(const DeltaInstructionsMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::DeltaInstructions));
    writeString(buf, msg.relativePath);
    writeU64(buf, msg.targetFileSize);
    writeU32(buf, msg.blockSize);
    writeU32(buf, static_cast<uint32_t>(msg.instructions.size()));
    for (const auto& inst : msg.instructions) {
        writeU64(buf, inst.blockIndex);
        writeU64(buf, inst.offset);
        writeU64(buf, inst.length);
        writeString(buf, inst.checksum);
    }
    return buf;
}

DeltaInstructionsMessage deserializeDeltaInstructionsMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::DeltaInstructions);
    DeltaInstructionsMessage msg;
    msg.relativePath = reader.readString();
    msg.targetFileSize = reader.readU64();
    msg.blockSize = reader.readU32();
    uint32_t count = reader.readU32();
    msg.instructions.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        DeltaInstruction inst;
        inst.blockIndex = reader.readU64();
        inst.offset = reader.readU64();
        inst.length = reader.readU64();
        inst.checksum = reader.readString();
        msg.instructions.push_back(inst);
    }
    reader.expectEOF();
    return msg;
}

// BlockDataMessage
std::vector<uint8_t> serializeMessage(const BlockDataMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::BlockData));
    writeString(buf, msg.relativePath);
    writeU64(buf, msg.offset);
    writeBytes(buf, msg.data);
    return buf;
}

BlockDataMessage deserializeBlockDataMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::BlockData);
    BlockDataMessage msg;
    msg.relativePath = reader.readString();
    msg.offset = reader.readU64();
    msg.data = reader.readBytes();
    reader.expectEOF();
    return msg;
}

// TransferAckMessage
std::vector<uint8_t> serializeMessage(const TransferAckMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::TransferAck));
    writeString(buf, msg.relativePath);
    writeU64(buf, msg.bytesReceived);
    return buf;
}

TransferAckMessage deserializeTransferAckMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::TransferAck);
    TransferAckMessage msg;
    msg.relativePath = reader.readString();
    msg.bytesReceived = reader.readU64();
    reader.expectEOF();
    return msg;
}

// ResumeRequestMessage
std::vector<uint8_t> serializeMessage(const ResumeRequestMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::ResumeRequest));
    writeString(buf, msg.relativePath);
    writeString(buf, msg.fileHash);
    writeU64(buf, msg.lastOffset);
    return buf;
}

ResumeRequestMessage deserializeResumeRequestMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::ResumeRequest);
    ResumeRequestMessage msg;
    msg.relativePath = reader.readString();
    msg.fileHash = reader.readString();
    msg.lastOffset = reader.readU64();
    reader.expectEOF();
    return msg;
}

// ResumeResponseMessage
std::vector<uint8_t> serializeMessage(const ResumeResponseMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::ResumeResponse));
    writeString(buf, msg.relativePath);
    writeBool(buf, msg.canResume);
    writeU64(buf, msg.resumeOffset);
    return buf;
}

ResumeResponseMessage deserializeResumeResponseMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::ResumeResponse);
    ResumeResponseMessage msg;
    msg.relativePath = reader.readString();
    msg.canResume = reader.readBool();
    msg.resumeOffset = reader.readU64();
    reader.expectEOF();
    return msg;
}

// TransferCompleteMessage
std::vector<uint8_t> serializeMessage(const TransferCompleteMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::TransferComplete));
    writeString(buf, msg.relativePath);
    writeBool(buf, msg.success);
    writeString(buf, msg.finalHash);
    return buf;
}

TransferCompleteMessage deserializeTransferCompleteMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::TransferComplete);
    TransferCompleteMessage msg;
    msg.relativePath = reader.readString();
    msg.success = reader.readBool();
    msg.finalHash = reader.readString();
    reader.expectEOF();
    return msg;
}

// ErrorMessageMessage
std::vector<uint8_t> serializeMessage(const ErrorMessageMessage& msg) {
    std::vector<uint8_t> buf;
    writeU8(buf, static_cast<uint8_t>(MessageType::ErrorMessage));
    writeU32(buf, msg.errorCode);
    writeString(buf, msg.errorMessage);
    return buf;
}

ErrorMessageMessage deserializeErrorMessageMessage(const std::vector<uint8_t>& payload) {
    BinaryReader reader(payload);
    reader.expectMessageType(MessageType::ErrorMessage);
    ErrorMessageMessage msg;
    msg.errorCode = reader.readU32();
    msg.errorMessage = reader.readString();
    reader.expectEOF();
    return msg;
}

namespace {

template <typename T>
void sendTypedMessage(TcpSocket& sock, const T& msg) {
    sendFramedMessage(sock, serializeMessage(msg));
}

} // anonymous namespace

void sendMessage(TcpSocket& sock, const HelloMessage& msg) { sendTypedMessage(sock, msg); }
void sendMessage(TcpSocket& sock, const PairChallengeMessage& msg) { sendTypedMessage(sock, msg); }
void sendMessage(TcpSocket& sock, const PairResponseMessage& msg) { sendTypedMessage(sock, msg); }
void sendMessage(TcpSocket& sock, const PairResultMessage& msg) { sendTypedMessage(sock, msg); }
void sendMessage(TcpSocket& sock, const ManifestRequestMessage& msg) { sendTypedMessage(sock, msg); }
void sendMessage(TcpSocket& sock, const ManifestResponseMessage& msg) { sendTypedMessage(sock, msg); }
void sendMessage(TcpSocket& sock, const DeltaInstructionsMessage& msg) { sendTypedMessage(sock, msg); }
void sendMessage(TcpSocket& sock, const BlockDataMessage& msg) { sendTypedMessage(sock, msg); }
void sendMessage(TcpSocket& sock, const TransferAckMessage& msg) { sendTypedMessage(sock, msg); }
void sendMessage(TcpSocket& sock, const ResumeRequestMessage& msg) { sendTypedMessage(sock, msg); }
void sendMessage(TcpSocket& sock, const ResumeResponseMessage& msg) { sendTypedMessage(sock, msg); }
void sendMessage(TcpSocket& sock, const TransferCompleteMessage& msg) { sendTypedMessage(sock, msg); }
void sendMessage(TcpSocket& sock, const ErrorMessageMessage& msg) { sendTypedMessage(sock, msg); }

MessageType peekNextMessageType(TcpSocket& sock, std::vector<uint8_t>& outRawPayload) {
    outRawPayload = recvFramedMessage(sock);
    return getMessageType(outRawPayload);
}

} // namespace peersync
