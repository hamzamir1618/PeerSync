#include <gtest/gtest.h>
#include <peersync/transfer.h>
#include <peersync/socket.h>
#include <peersync/exceptions.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <string>
#include <random>

namespace {

class TransferTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::random_device rd;
        testDir = std::filesystem::temp_directory_path() / ("peersync_test_transfer_" + std::to_string(rd()));
        senderDir = testDir / "sender";
        receiverDir = testDir / "receiver";
        std::filesystem::create_directories(senderDir);
        std::filesystem::create_directories(receiverDir);
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

    std::vector<uint8_t> readFileBytes(const std::filesystem::path& path) {
        std::ifstream ifs(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    std::filesystem::path testDir;
    std::filesystem::path senderDir;
    std::filesystem::path receiverDir;
};

} // anonymous namespace

TEST_F(TransferTest, SyncNewFileToEmptyPeer) {
    std::filesystem::path senderFile = senderDir / "new_file.dat";
    createTestFile(senderFile, 15000, 42); // 15 KB file

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

    ASSERT_TRUE(client.isValid());
    ASSERT_TRUE(acceptedSocket.isValid());

    bool receiverSuccess = false;
    std::string receiverError;
    std::string receiverHash;

    std::thread receiverThread([&]() {
        try {
            peersync::TransferSession session(acceptedSocket);
            receiverSuccess = session.receiveFile(receiverDir);
            receiverHash = session.getFinalHash();
        } catch (const std::exception& e) {
            receiverSuccess = false;
            receiverError = e.what();
        }
        acceptedSocket.close();
    });

    peersync::TransferSession senderSession(client);
    bool senderSuccess = false;
    try {
        senderSuccess = senderSession.sendFile(senderFile, "new_file.dat");
    } catch (const std::exception& e) {
        client.close();
        FAIL() << "Sender exception: " << e.what();
    }
    client.close();

    if (receiverThread.joinable()) {
        receiverThread.join();
    }

    EXPECT_TRUE(senderSuccess) << "Sender should complete transfer successfully";
    EXPECT_TRUE(receiverSuccess) << "Receiver should complete transfer successfully: " << receiverError;

    std::filesystem::path targetFile = receiverDir / "new_file.dat";
    ASSERT_TRUE(std::filesystem::exists(targetFile));
    EXPECT_EQ(std::filesystem::file_size(targetFile), 15000u);

    auto senderBytes = readFileBytes(senderFile);
    auto receiverBytes = readFileBytes(targetFile);
    EXPECT_EQ(senderBytes, receiverBytes) << "Reconstructed file must be byte-for-byte identical";
    EXPECT_EQ(senderSession.getFinalHash(), receiverHash);
}

TEST_F(TransferTest, SyncUpdatedFileToPartialPeerTransmitsMeaningfullyLessData) {
    std::filesystem::path senderFile = senderDir / "large_doc.bin";
    std::filesystem::path receiverFile = receiverDir / "large_doc.bin";

    // 100 KB file identical on both sides initially
    createTestFile(senderFile, 100000, 10);
    createTestFile(receiverFile, 100000, 10);

    // Modify only 50 bytes in the middle of senderFile
    {
        std::fstream fs(senderFile, std::ios::in | std::ios::out | std::ios::binary);
        fs.seekp(50000, std::ios::beg);
        for (int i = 0; i < 50; ++i) {
            fs.put(static_cast<char>(0xFF));
        }
    }

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

    ASSERT_TRUE(client.isValid());
    ASSERT_TRUE(acceptedSocket.isValid());

    bool receiverSuccess = false;
    std::string receiverError;

    std::thread receiverThread([&]() {
        try {
            peersync::TransferSession session(acceptedSocket);
            receiverSuccess = session.receiveFile(receiverDir);
        } catch (const std::exception& e) {
            receiverSuccess = false;
            receiverError = e.what();
        }
    });

    peersync::TransferSession senderSession(client);
    bool senderSuccess = false;
    try {
        senderSuccess = senderSession.sendFile(senderFile, "large_doc.bin");
    } catch (const std::exception& e) {
        FAIL() << "Sender exception: " << e.what();
    }

    if (receiverThread.joinable()) {
        receiverThread.join();
    }

    EXPECT_TRUE(senderSuccess);
    EXPECT_TRUE(receiverSuccess) << "Receiver error: " << receiverError;

    auto senderBytes = readFileBytes(senderFile);
    auto receiverBytes = readFileBytes(receiverFile);
    EXPECT_EQ(senderBytes, receiverBytes);

    // Crucial requirement: prove delta sync actually transmitted meaningfully less data over the network
    uint64_t bytesSent = senderSession.getBytesSent();
    EXPECT_LT(bytesSent, 20000u) << "Delta sync of a 100 KB file with 50 changed bytes must transmit well below the file size (actual: " << bytesSent << " bytes)";
}

TEST_F(TransferTest, SyncWithLargeLiteralChunkingAsBlockData) {
    std::filesystem::path senderFile = senderDir / "chunked.bin";
    createTestFile(senderFile, 5000, 99); // 5 KB file

    peersync::TransferSession::Config config;
    config.blockSize = 512;
    config.literalThreshold = 1000; // Any literal > 1000 bytes sent as BlockData message
    config.maxInstructionsPerMessage = 10; // Force multi-message chunking of instruction list

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

    bool receiverSuccess = false;
    std::string receiverError;

    std::thread receiverThread([&]() {
        try {
            peersync::TransferSession session(acceptedSocket, config);
            receiverSuccess = session.receiveFile(receiverDir);
        } catch (const std::exception& e) {
            receiverSuccess = false;
            receiverError = e.what();
        }
    });

    peersync::TransferSession senderSession(client, config);
    bool senderSuccess = false;
    try {
        senderSuccess = senderSession.sendFile(senderFile, "chunked.bin");
    } catch (const std::exception& e) {
        FAIL() << "Sender exception: " << e.what();
    }

    if (receiverThread.joinable()) {
        receiverThread.join();
    }

    EXPECT_TRUE(senderSuccess);
    EXPECT_TRUE(receiverSuccess) << "Receiver error: " << receiverError;

    std::filesystem::path targetFile = receiverDir / "chunked.bin";
    EXPECT_EQ(readFileBytes(senderFile), readFileBytes(targetFile));
}

TEST_F(TransferTest, SyncEmptyFile) {
    std::filesystem::path senderFile = senderDir / "empty.dat";
    std::ofstream(senderFile, std::ios::binary).close(); // 0-byte file

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

    bool receiverSuccess = false;
    std::thread receiverThread([&]() {
        peersync::TransferSession session(acceptedSocket);
        receiverSuccess = session.receiveFile(receiverDir);
    });

    peersync::TransferSession senderSession(client);
    bool senderSuccess = senderSession.sendFile(senderFile, "empty.dat");

    if (receiverThread.joinable()) {
        receiverThread.join();
    }

    EXPECT_TRUE(senderSuccess);
    EXPECT_TRUE(receiverSuccess);

    std::filesystem::path targetFile = receiverDir / "empty.dat";
    ASSERT_TRUE(std::filesystem::exists(targetFile));
    EXPECT_EQ(std::filesystem::file_size(targetFile), 0u);
}
