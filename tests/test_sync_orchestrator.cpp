#include <gtest/gtest.h>
#include <peersync/sync_orchestrator.h>
#include <peersync/socket.h>
#include <peersync/exceptions.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <string>
#include <random>
#include <chrono>

namespace {

class SyncOrchestratorTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::random_device rd;
        testDir = std::filesystem::temp_directory_path() / ("peersync_test_sync_" + std::to_string(rd()));
        dirA = testDir / "dirA";
        dirB = testDir / "dirB";
        std::filesystem::create_directories(dirA);
        std::filesystem::create_directories(dirB);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }

    void createTestFile(const std::filesystem::path& path, size_t size, uint8_t startByte = 0) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream ofs(path, std::ios::binary);
        for (size_t i = 0; i < size; ++i) {
            uint8_t b = static_cast<uint8_t>((startByte + i) % 256);
            ofs.put(static_cast<char>(b));
        }
    }

    std::vector<uint8_t> readFileBytes(const std::filesystem::path& path) {
        std::ifstream ifs(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    std::filesystem::path testDir;
    std::filesystem::path dirA;
    std::filesystem::path dirB;
};

TEST_F(SyncOrchestratorTest, NonOverlappingDirectories) {
    createTestFile(dirA / "file1.txt", 1000, 10);
    createTestFile(dirA / "sub" / "file2.txt", 2000, 20);
    createTestFile(dirB / "file3.txt", 1500, 30);

    peersync::TcpSocket server = peersync::TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    peersync::TcpSocket acceptedSocket;
    std::thread acceptThread([&]() {
        acceptedSocket = server.accept();
    });

    peersync::TcpSocket client = peersync::TcpSocket::connect("127.0.0.1", port);
    if (acceptThread.joinable()) acceptThread.join();

    ASSERT_TRUE(client.isValid());
    ASSERT_TRUE(acceptedSocket.isValid());

    peersync::SyncPolicy policy;
    policy.direction = peersync::SyncPolicy::Direction::Bidirectional;
    policy.maxConcurrency = 2;

    bool responderSuccess = false;
    std::thread responderThread([&]() {
        peersync::SyncOrchestrator responder(acceptedSocket, peersync::SyncOrchestrator::Role::Responder, policy);
        responderSuccess = responder.syncDirectory(dirB);
        acceptedSocket.close();
    });

    peersync::SyncOrchestrator initiator(client, peersync::SyncOrchestrator::Role::Initiator, policy);
    bool initiatorSuccess = initiator.syncDirectory(dirA);
    client.close();

    if (responderThread.joinable()) responderThread.join();

    EXPECT_TRUE(initiatorSuccess);
    EXPECT_TRUE(responderSuccess);

    EXPECT_TRUE(std::filesystem::exists(dirA / "file3.txt"));
    EXPECT_TRUE(std::filesystem::exists(dirB / "file1.txt"));
    EXPECT_TRUE(std::filesystem::exists(dirB / "sub" / "file2.txt"));

    EXPECT_EQ(readFileBytes(dirA / "file3.txt"), readFileBytes(dirB / "file3.txt"));
    EXPECT_EQ(readFileBytes(dirB / "file1.txt"), readFileBytes(dirA / "file1.txt"));
    EXPECT_EQ(readFileBytes(dirB / "sub" / "file2.txt"), readFileBytes(dirA / "sub" / "file2.txt"));
}

TEST_F(SyncOrchestratorTest, IdenticalFilesSkipped) {
    createTestFile(dirA / "same.txt", 5000, 42);
    createTestFile(dirB / "same.txt", 5000, 42);

    auto ftime = std::filesystem::last_write_time(dirA / "same.txt");
    std::filesystem::last_write_time(dirB / "same.txt", ftime);

    peersync::TcpSocket server = peersync::TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    peersync::TcpSocket acceptedSocket;
    std::thread acceptThread([&]() {
        acceptedSocket = server.accept();
    });

    peersync::TcpSocket client = peersync::TcpSocket::connect("127.0.0.1", port);
    if (acceptThread.joinable()) acceptThread.join();

    peersync::SyncPolicy policy;
    policy.direction = peersync::SyncPolicy::Direction::Bidirectional;

    bool responderSuccess = false;
    size_t responderSkipped = 0;
    size_t responderSent = 0;
    size_t responderReceived = 0;

    std::thread responderThread([&]() {
        peersync::SyncOrchestrator responder(acceptedSocket, peersync::SyncOrchestrator::Role::Responder, policy);
        responderSuccess = responder.syncDirectory(dirB);
        responderSkipped = responder.getFilesSkippedCount();
        responderSent = responder.getFilesSentCount();
        responderReceived = responder.getFilesReceivedCount();
        acceptedSocket.close();
    });

    peersync::SyncOrchestrator initiator(client, peersync::SyncOrchestrator::Role::Initiator, policy);
    bool initiatorSuccess = initiator.syncDirectory(dirA);
    size_t initiatorSkipped = initiator.getFilesSkippedCount();
    size_t initiatorSent = initiator.getFilesSentCount();
    size_t initiatorReceived = initiator.getFilesReceivedCount();
    client.close();

    if (responderThread.joinable()) responderThread.join();

    EXPECT_TRUE(initiatorSuccess);
    EXPECT_TRUE(responderSuccess);

    EXPECT_EQ(initiatorSkipped, 1u);
    EXPECT_EQ(initiatorSent, 0u);
    EXPECT_EQ(initiatorReceived, 0u);

    EXPECT_EQ(responderSkipped, 1u);
    EXPECT_EQ(responderSent, 0u);
    EXPECT_EQ(responderReceived, 0u);
}

TEST_F(SyncOrchestratorTest, DifferingContentTriggersDeltaTransfer) {
    createTestFile(dirA / "doc.txt", 10000, 10);
    createTestFile(dirB / "doc.txt", 10000, 20); // Different content (startByte = 20)

    auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(dirA / "doc.txt", now - std::chrono::hours(2));
    std::filesystem::last_write_time(dirB / "doc.txt", now); // dirB is newer

    peersync::TcpSocket server = peersync::TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    peersync::TcpSocket acceptedSocket;
    std::thread acceptThread([&]() {
        acceptedSocket = server.accept();
    });

    peersync::TcpSocket client = peersync::TcpSocket::connect("127.0.0.1", port);
    if (acceptThread.joinable()) acceptThread.join();

    peersync::SyncPolicy policy;
    policy.direction = peersync::SyncPolicy::Direction::Bidirectional;

    bool responderSuccess = false;
    size_t responderSent = 0;
    size_t responderReceived = 0;

    std::thread responderThread([&]() {
        peersync::SyncOrchestrator responder(acceptedSocket, peersync::SyncOrchestrator::Role::Responder, policy);
        responderSuccess = responder.syncDirectory(dirB);
        responderSent = responder.getFilesSentCount();
        responderReceived = responder.getFilesReceivedCount();
        acceptedSocket.close();
    });

    peersync::SyncOrchestrator initiator(client, peersync::SyncOrchestrator::Role::Initiator, policy);
    bool initiatorSuccess = initiator.syncDirectory(dirA);
    size_t initiatorSent = initiator.getFilesSentCount();
    size_t initiatorReceived = initiator.getFilesReceivedCount();
    client.close();

    if (responderThread.joinable()) responderThread.join();

    EXPECT_TRUE(initiatorSuccess);
    EXPECT_TRUE(responderSuccess);

    // Because dirB was newer, dirB should send its version to dirA
    EXPECT_EQ(initiatorReceived, 1u);
    EXPECT_EQ(initiatorSent, 0u);
    EXPECT_EQ(responderSent, 1u);
    EXPECT_EQ(responderReceived, 0u);

    EXPECT_EQ(readFileBytes(dirA / "doc.txt"), readFileBytes(dirB / "doc.txt"));
}

TEST_F(SyncOrchestratorTest, RespectsConcurrencyCap) {
    const size_t numFiles = 15;
    for (size_t i = 0; i < numFiles; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "file_%02zu.dat", i);
        createTestFile(dirA / buf, 8000, static_cast<uint8_t>(i));
    }

    peersync::TcpSocket server = peersync::TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    peersync::TcpSocket acceptedSocket;
    std::thread acceptThread([&]() {
        acceptedSocket = server.accept();
    });

    peersync::TcpSocket client = peersync::TcpSocket::connect("127.0.0.1", port);
    if (acceptThread.joinable()) acceptThread.join();

    peersync::SyncPolicy policy;
    policy.direction = peersync::SyncPolicy::Direction::Bidirectional;
    policy.maxConcurrency = 3; // Concurrency cap = 3

    bool responderSuccess = false;
    size_t responderMaxConcurrency = 0;

    std::thread responderThread([&]() {
        peersync::SyncOrchestrator responder(acceptedSocket, peersync::SyncOrchestrator::Role::Responder, policy);
        responderSuccess = responder.syncDirectory(dirB);
        responderMaxConcurrency = responder.getMaxObservedConcurrency();
        acceptedSocket.close();
    });

    peersync::SyncOrchestrator initiator(client, peersync::SyncOrchestrator::Role::Initiator, policy);
    bool initiatorSuccess = initiator.syncDirectory(dirA);
    size_t initiatorMaxConcurrency = initiator.getMaxObservedConcurrency();
    client.close();

    if (responderThread.joinable()) responderThread.join();

    EXPECT_TRUE(initiatorSuccess);
    EXPECT_TRUE(responderSuccess);

    EXPECT_LE(initiatorMaxConcurrency, 3u);
    EXPECT_LE(responderMaxConcurrency, 3u);

    // Verify all files were synced
    for (size_t i = 0; i < numFiles; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "file_%02zu.dat", i);
        EXPECT_TRUE(std::filesystem::exists(dirB / buf));
        EXPECT_EQ(readFileBytes(dirA / buf), readFileBytes(dirB / buf));
    }
}

} // anonymous namespace
