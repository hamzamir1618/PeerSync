#include <gtest/gtest.h>
#include <peersync/protocol.h>
#include <peersync/exceptions.h>
#include <vector>
#include <string>
#include <thread>
#include <peersync/socket.h>

using namespace peersync;

TEST(ProtocolTest, HelloMessageRoundTrip) {
    HelloMessage msg{"TestDevice-X", "2.0.0-beta"};
    auto payload = serializeMessage(msg);
    EXPECT_EQ(getMessageType(payload), MessageType::Hello);

    auto decoded = deserializeHelloMessage(payload);
    EXPECT_EQ(decoded.deviceName, msg.deviceName);
    EXPECT_EQ(decoded.protocolVersion, msg.protocolVersion);

    // Edge case: empty strings
    HelloMessage emptyMsg{"", ""};
    auto decodedEmpty = deserializeHelloMessage(serializeMessage(emptyMsg));
    EXPECT_EQ(decodedEmpty.deviceName, "");
    EXPECT_EQ(decodedEmpty.protocolVersion, "");
}

TEST(ProtocolTest, PairChallengeMessageRoundTrip) {
    PairChallengeMessage msg{{0x01, 0x02, 0xFF, 0x00, 0x7F}};
    auto payload = serializeMessage(msg);
    EXPECT_EQ(getMessageType(payload), MessageType::PairChallenge);

    auto decoded = deserializePairChallengeMessage(payload);
    EXPECT_EQ(decoded.challengeBytes, msg.challengeBytes);

    // Edge case: empty challenge vector
    PairChallengeMessage emptyMsg{{}};
    auto decodedEmpty = deserializePairChallengeMessage(serializeMessage(emptyMsg));
    EXPECT_TRUE(decodedEmpty.challengeBytes.empty());
}

TEST(ProtocolTest, PairResponseMessageRoundTrip) {
    PairResponseMessage msg{{0xAA, 0xBB, 0xCC, 0xDD}};
    auto payload = serializeMessage(msg);
    EXPECT_EQ(getMessageType(payload), MessageType::PairResponse);

    auto decoded = deserializePairResponseMessage(payload);
    EXPECT_EQ(decoded.responseBytes, msg.responseBytes);
}

TEST(ProtocolTest, PairResultMessageRoundTrip) {
    PairResultMessage msgTrue{true, ""};
    auto decodedTrue = deserializePairResultMessage(serializeMessage(msgTrue));
    EXPECT_TRUE(decodedTrue.success);
    EXPECT_EQ(decodedTrue.errorMessage, "");

    PairResultMessage msgFalse{false, "PIN verification failed: Invalid response"};
    auto decodedFalse = deserializePairResultMessage(serializeMessage(msgFalse));
    EXPECT_FALSE(decodedFalse.success);
    EXPECT_EQ(decodedFalse.errorMessage, "PIN verification failed: Invalid response");
}

TEST(ProtocolTest, ManifestRequestMessageRoundTrip) {
    ManifestRequestMessage msg{"/projects/offline-file-sync/data"};
    auto decoded = deserializeManifestRequestMessage(serializeMessage(msg));
    EXPECT_EQ(decoded.path, msg.path);
}

TEST(ProtocolTest, ManifestResponseMessageRoundTrip) {
    ManifestResponseMessage msg;
    msg.files.push_back({"file1.txt", 1024, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 1689000000});
    msg.files.push_back({"dir/subdir/file2.bin", UINT64_MAX, "a591a6d40bf420404a011733cfb7b190d62c65bf0bcda32b57b277d9ad9f146e", 0});

    auto payload = serializeMessage(msg);
    EXPECT_EQ(getMessageType(payload), MessageType::ManifestResponse);

    auto decoded = deserializeManifestResponseMessage(payload);
    ASSERT_EQ(decoded.files.size(), 2u);
    EXPECT_EQ(decoded.files[0].relativePath, "file1.txt");
    EXPECT_EQ(decoded.files[0].fileSize, 1024u);
    EXPECT_EQ(decoded.files[0].sha256Hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(decoded.files[0].lastModified, 1689000000u);

    EXPECT_EQ(decoded.files[1].relativePath, "dir/subdir/file2.bin");
    EXPECT_EQ(decoded.files[1].fileSize, UINT64_MAX); // test max plausible u64 value
    EXPECT_EQ(decoded.files[1].sha256Hash, "a591a6d40bf420404a011733cfb7b190d62c65bf0bcda32b57b277d9ad9f146e");
    EXPECT_EQ(decoded.files[1].lastModified, 0u);

    // Edge case: empty manifest
    ManifestResponseMessage emptyMsg;
    auto decodedEmpty = deserializeManifestResponseMessage(serializeMessage(emptyMsg));
    EXPECT_TRUE(decodedEmpty.files.empty());
}

TEST(ProtocolTest, DeltaInstructionsMessageRoundTrip) {
    DeltaInstructionsMessage msg;
    msg.relativePath = "huge_database.db";
    msg.targetFileSize = 10000000000ULL;
    msg.blockSize = 65536;
    msg.instructions.push_back({0, 0, 65536, "checksum0"});
    msg.instructions.push_back({99999, 655350000, 32768, "checksum99999"});

    auto payload = serializeMessage(msg);
    EXPECT_EQ(getMessageType(payload), MessageType::DeltaInstructions);

    auto decoded = deserializeDeltaInstructionsMessage(payload);
    EXPECT_EQ(decoded.relativePath, msg.relativePath);
    EXPECT_EQ(decoded.targetFileSize, msg.targetFileSize);
    EXPECT_EQ(decoded.blockSize, msg.blockSize);
    ASSERT_EQ(decoded.instructions.size(), 2u);
    EXPECT_EQ(decoded.instructions[1].blockIndex, 99999u);
    EXPECT_EQ(decoded.instructions[1].offset, 655350000u);
    EXPECT_EQ(decoded.instructions[1].length, 32768u);
    EXPECT_EQ(decoded.instructions[1].checksum, "checksum99999");
}

TEST(ProtocolTest, BlockDataMessageRoundTrip) {
    BlockDataMessage msg{"video.mp4", 1048576, {0x10, 0x20, 0x30, 0x40, 0x50}};
    auto payload = serializeMessage(msg);
    EXPECT_EQ(getMessageType(payload), MessageType::BlockData);

    auto decoded = deserializeBlockDataMessage(payload);
    EXPECT_EQ(decoded.relativePath, msg.relativePath);
    EXPECT_EQ(decoded.offset, msg.offset);
    EXPECT_EQ(decoded.data, msg.data);

    // Edge case: zero-length byte vector for BlockData
    BlockDataMessage emptyBlock{"empty.bin", 0, {}};
    auto decodedEmpty = deserializeBlockDataMessage(serializeMessage(emptyBlock));
    EXPECT_EQ(decodedEmpty.relativePath, "empty.bin");
    EXPECT_EQ(decodedEmpty.offset, 0u);
    EXPECT_TRUE(decodedEmpty.data.empty());
}

TEST(ProtocolTest, TransferAckMessageRoundTrip) {
    TransferAckMessage msg{"document.pdf", 524288};
    auto decoded = deserializeTransferAckMessage(serializeMessage(msg));
    EXPECT_EQ(decoded.relativePath, msg.relativePath);
    EXPECT_EQ(decoded.bytesReceived, msg.bytesReceived);
}

TEST(ProtocolTest, ResumeRequestMessageRoundTrip) {
    ResumeRequestMessage msg{"archive.zip", "abc123hash", 999999999};
    auto decoded = deserializeResumeRequestMessage(serializeMessage(msg));
    EXPECT_EQ(decoded.relativePath, msg.relativePath);
    EXPECT_EQ(decoded.fileHash, msg.fileHash);
    EXPECT_EQ(decoded.lastOffset, msg.lastOffset);
}

TEST(ProtocolTest, ResumeResponseMessageRoundTrip) {
    ResumeResponseMessage msgTrue{"archive.zip", true, 999999999};
    auto decodedTrue = deserializeResumeResponseMessage(serializeMessage(msgTrue));
    EXPECT_EQ(decodedTrue.relativePath, "archive.zip");
    EXPECT_TRUE(decodedTrue.canResume);
    EXPECT_EQ(decodedTrue.resumeOffset, 999999999u);

    ResumeResponseMessage msgFalse{"archive.zip", false, 0};
    auto decodedFalse = deserializeResumeResponseMessage(serializeMessage(msgFalse));
    EXPECT_FALSE(decodedFalse.canResume);
    EXPECT_EQ(decodedFalse.resumeOffset, 0u);
}

TEST(ProtocolTest, TransferCompleteMessageRoundTrip) {
    TransferCompleteMessage msg{"image.png", true, "final_sha256_hash_value"};
    auto decoded = deserializeTransferCompleteMessage(serializeMessage(msg));
    EXPECT_EQ(decoded.relativePath, msg.relativePath);
    EXPECT_TRUE(decoded.success);
    EXPECT_EQ(decoded.finalHash, msg.finalHash);
}

TEST(ProtocolTest, ErrorMessageMessageRoundTrip) {
    ErrorMessageMessage msg{404, "File not found on remote peer"};
    auto decoded = deserializeErrorMessageMessage(serializeMessage(msg));
    EXPECT_EQ(decoded.errorCode, 404u);
    EXPECT_EQ(decoded.errorMessage, msg.errorMessage);
}

TEST(ProtocolTest, InvalidMessageTypeThrowsException) {
    // 0 is not a valid MessageType enum tag
    std::vector<uint8_t> invalidZero = {0x00, 0x01, 0x02};
    EXPECT_THROW(getMessageType(invalidZero), PeerSyncProtocolException);
    EXPECT_THROW(deserializeHelloMessage(invalidZero), PeerSyncProtocolException);

    // 99 is not a valid MessageType enum tag
    std::vector<uint8_t> invalid99 = {99, 0x00, 0x00, 0x00, 0x00};
    EXPECT_THROW(getMessageType(invalid99), PeerSyncProtocolException);
    EXPECT_THROW(deserializeHelloMessage(invalid99), PeerSyncProtocolException);

    // Mismatched tag (try to deserialize a HelloMessage payload as a ManifestRequestMessage)
    HelloMessage hello{"Dev", "1.0"};
    auto helloPayload = serializeMessage(hello);
    EXPECT_THROW(deserializeManifestRequestMessage(helloPayload), PeerSyncProtocolException);

    // Empty payload
    std::vector<uint8_t> emptyPayload;
    EXPECT_THROW(getMessageType(emptyPayload), PeerSyncProtocolException);
}

TEST(ProtocolTest, SocketTypedMessagingRoundTrip) {
    TcpSocket server = TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    TcpSocket acceptedSocket;
    std::thread serverThread([&]() {
        acceptedSocket = server.accept();
    });

    TcpSocket client = TcpSocket::connect("127.0.0.1", port);
    if (serverThread.joinable()) {
        serverThread.join();
    }
    ASSERT_TRUE(client.isValid());
    ASSERT_TRUE(acceptedSocket.isValid());

    // 1. Send HelloMessage from client -> server
    HelloMessage hello{"MyLaptop", "v1.0"};
    sendMessage(client, hello);

    std::vector<uint8_t> rawPayload;
    MessageType tag = peekNextMessageType(acceptedSocket, rawPayload);
    EXPECT_EQ(tag, MessageType::Hello);
    auto decodedHello = deserializeHelloMessage(rawPayload);
    EXPECT_EQ(decodedHello.deviceName, "MyLaptop");
    EXPECT_EQ(decodedHello.protocolVersion, "v1.0");

    // 2. Send ManifestResponseMessage from server -> client
    ManifestResponseMessage manifest;
    manifest.files.push_back({"test.doc", 2048, "hash_abc", 1000});
    sendMessage(acceptedSocket, manifest);

    tag = peekNextMessageType(client, rawPayload);
    EXPECT_EQ(tag, MessageType::ManifestResponse);
    auto decodedManifest = deserializeManifestResponseMessage(rawPayload);
    ASSERT_EQ(decodedManifest.files.size(), 1u);
    EXPECT_EQ(decodedManifest.files[0].relativePath, "test.doc");
    EXPECT_EQ(decodedManifest.files[0].fileSize, 2048u);

    // 3. Send BlockDataMessage from client -> server
    BlockDataMessage block{"test.doc", 0, {0xAA, 0xBB, 0xCC}};
    sendMessage(client, block);

    tag = peekNextMessageType(acceptedSocket, rawPayload);
    EXPECT_EQ(tag, MessageType::BlockData);
    auto decodedBlock = deserializeBlockDataMessage(rawPayload);
    EXPECT_EQ(decodedBlock.relativePath, "test.doc");
    EXPECT_EQ(decodedBlock.offset, 0u);
    EXPECT_EQ(decodedBlock.data, block.data);

    // 4. Send ErrorMessageMessage from server -> client
    ErrorMessageMessage err{500, "Internal Sync Error"};
    sendMessage(acceptedSocket, err);

    tag = peekNextMessageType(client, rawPayload);
    EXPECT_EQ(tag, MessageType::ErrorMessage);
    auto decodedErr = deserializeErrorMessageMessage(rawPayload);
    EXPECT_EQ(decodedErr.errorCode, 500u);
    EXPECT_EQ(decodedErr.errorMessage, "Internal Sync Error");
}
