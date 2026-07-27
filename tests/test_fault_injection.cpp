#include <gtest/gtest.h>
#include <peersync/socket.h>
#include <peersync/message_framing.h>
#include <peersync/protocol.h>
#include <peersync/transfer.h>
#include <peersync/conflict_resolution.h>
#include <peersync/exceptions.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <chrono>
#include <random>

namespace {

class FaultInjectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::random_device rd;
        testDir = std::filesystem::temp_directory_path() / ("peersync_fault_injection_" + std::to_string(rd()));
        std::filesystem::create_directories(testDir);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }

    void createTestFile(const std::filesystem::path& path, size_t size, uint8_t startByte = 0) {
        std::ofstream ofs(path, std::ios::binary);
        for (size_t i = 0; i < size; ++i) {
            uint8_t b = static_cast<uint8_t>((startByte + i) % 256);
            ofs.put(static_cast<char>(b));
        }
    }

    std::filesystem::path testDir;
};

} // anonymous namespace

TEST_F(FaultInjectionTest, PartialSendReadTimeout) {
    peersync::TcpSocket server = peersync::TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    peersync::TcpSocket acceptedSocket;
    std::thread acceptThread([&]() {
        acceptedSocket = server.accept();
        acceptedSocket.setRecvTimeout(300); // 300ms read timeout
    });

    peersync::TcpSocket client = peersync::TcpSocket::connect("127.0.0.1", port);
    if (acceptThread.joinable()) {
        acceptThread.join();
    }
    ASSERT_TRUE(acceptedSocket.isValid());

    // Send valid 4-byte frame length indicating 1000 bytes, but send only 10 bytes of payload
    uint8_t netLen[4] = { (1000 >> 24) & 0xFF, (1000 >> 16) & 0xFF, (1000 >> 8) & 0xFF, 1000 & 0xFF };
    client.send(netLen, 4);
    std::vector<uint8_t> partialData(10, 'X');
    client.send(partialData.data(), partialData.size());

    auto start = std::chrono::steady_clock::now();
    EXPECT_THROW({
        peersync::recvFramedMessage(acceptedSocket);
    }, peersync::PeerSyncNetworkException);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(elapsed, 2500); // Confirms timeout triggered without hanging indefinitely
}

TEST_F(FaultInjectionTest, UnexpectedMessageAfterPairingHandshake) {
    std::filesystem::path senderFile = testDir / "test.txt";
    createTestFile(senderFile, 100, 42);

    peersync::TcpSocket server = peersync::TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    peersync::TcpSocket acceptedSocket;
    std::thread acceptThread([&]() {
        acceptedSocket = server.accept();
    });
    peersync::TcpSocket client = peersync::TcpSocket::connect("127.0.0.1", port);
    if (acceptThread.joinable()) {
        acceptThread.join();
    }

    // Step 1: Simulate sender expecting ManifestResponse after sending ManifestRequest,
    // but receiver sends an unexpected BlockDataMessage instead.
    std::thread senderThread([&]() {
        peersync::TransferSession::Config config;
        config.allowResume = false;
        peersync::TransferSession session(client, config);
        EXPECT_THROW({
            session.sendFile(senderFile, "test.txt");
        }, peersync::PeerSyncProtocolException);
    });

    // Receive ManifestRequest sent by sendFile()
    auto reqPayload = peersync::recvFramedMessage(acceptedSocket);
    EXPECT_EQ(peersync::getMessageType(reqPayload), peersync::MessageType::ManifestRequest);

    // Send unexpected BlockDataMessage instead of ManifestResponse
    peersync::BlockDataMessage unexpectedMsg{"test.txt", 0, {1, 2, 3}};
    peersync::sendMessage(acceptedSocket, unexpectedMsg);

    if (senderThread.joinable()) {
        senderThread.join();
    }
}

TEST_F(FaultInjectionTest, SimultaneousSyncConflictResolution) {
    peersync::PeerIdentifier peerA{"Laptop-A", "192.168.1.10", 8000};
    peersync::PeerIdentifier peerB{"Desktop-B", "192.168.1.20", 8000};

    // Both peers independently evaluate roles for simultaneous sync initiation
    auto roleA = peersync::resolveSimultaneousSyncRole(peerA, peerB);
    auto roleB = peersync::resolveSimultaneousSyncRole(peerB, peerA);

    EXPECT_EQ(roleA, peersync::SimultaneousSyncRole::Receiver); // "Desktop-B" < "Laptop-A", so B is Sender, A is Receiver
    EXPECT_EQ(roleB, peersync::SimultaneousSyncRole::Sender);

    // Verify tie-breaking when names and IPs are identical (different ports)
    peersync::PeerIdentifier peerC{"Node-1", "10.0.0.1", 5001};
    peersync::PeerIdentifier peerD{"Node-1", "10.0.0.1", 5002};
    EXPECT_EQ(peersync::resolveSimultaneousSyncRole(peerC, peerD), peersync::SimultaneousSyncRole::Sender);
    EXPECT_EQ(peersync::resolveSimultaneousSyncRole(peerD, peerC), peersync::SimultaneousSyncRole::Receiver);

    // Verify end-to-end sync behavior when roles are resolved
    std::filesystem::path fileToSync = testDir / "simultaneous.dat";
    createTestFile(fileToSync, 1024, 99);
    std::filesystem::path targetDir = testDir / "target";
    std::filesystem::create_directories(targetDir);

    peersync::TcpSocket server = peersync::TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    peersync::TcpSocket acceptedSocket;
    std::thread acceptThread([&]() {
        acceptedSocket = server.accept();
    });
    peersync::TcpSocket client = peersync::TcpSocket::connect("127.0.0.1", port);
    if (acceptThread.joinable()) {
        acceptThread.join();
    }

    // Since B is Sender and A is Receiver:
    std::thread bThread([&]() {
        if (roleB == peersync::SimultaneousSyncRole::Sender) {
            peersync::TransferSession::Config config;
            config.allowResume = false;
            peersync::TransferSession session(client, config);
            session.sendFile(fileToSync, "simultaneous.dat");
        }
    });

    std::thread aThread([&]() {
        if (roleA == peersync::SimultaneousSyncRole::Receiver) {
            peersync::TransferSession::Config config;
            config.allowResume = false;
            peersync::TransferSession session(acceptedSocket, config);
            session.receiveFile(targetDir);
        }
    });

    if (bThread.joinable()) bThread.join();
    if (aThread.joinable()) aThread.join();

    EXPECT_TRUE(std::filesystem::exists(targetDir / "simultaneous.dat"));
    EXPECT_EQ(std::filesystem::file_size(targetDir / "simultaneous.dat"), 1024);
}
