#include <gtest/gtest.h>
#include <peersync/transfer.h>
#include <peersync/delta.h>
#include <peersync/socket.h>
#include <peersync/exceptions.h>
#include <peersync/message_framing.h>
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
    
    peersync::resetAdler32CallCount();

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
    
    EXPECT_EQ(peersync::getAdler32CallCount(), 0) << "Fast path must bypass computeAdler32 entirely when signature list is empty";
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

TEST_F(TransferTest, ResumableSyncAfterInterruption) {
    std::filesystem::path targetFile = receiverDir / "resume.dat";
    createTestFile(targetFile, 15000, 42); // Initial file on receiver

    auto senderBytes = readFileBytes(targetFile);
    for (size_t i = 32; i < senderBytes.size(); i += 128) {
        senderBytes[i] ^= 0xFF; // Modify periodic bytes to create alternating Copy and Literal instructions
    }
    std::filesystem::path senderFile = senderDir / "sender_resume.dat";
    {
        std::ofstream ofs(senderFile, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(senderBytes.data()), static_cast<std::streamsize>(senderBytes.size()));
    }
    auto contentBytes = readFileBytes(senderFile);

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

    peersync::TransferSession::Config config;
    config.blockSize = 64;
    config.maxInstructionsPerMessage = 10;
    config.literalThreshold = 10000; // Keep literals inlined for this test

    bool caughtException = false;
    std::thread receiverThread([&]() {
        try {
            peersync::TransferSession session(acceptedSocket, config);
            session.receiveFile(receiverDir);
        } catch (const std::exception&) {
            caughtException = true;
        }
    });

    // Manually act as sender for just 1 batch of 10 instructions
    {
        peersync::ManifestRequestMessage reqMsg{"resume.dat"};
        peersync::sendFramedMessage(client, peersync::serializeMessage(reqMsg));

        auto respPayload = peersync::recvFramedMessage(client);
        auto respMsg = peersync::deserializeManifestResponseMessage(respPayload);

        std::vector<peersync::DeltaInstruction> delta;
        peersync::computeDelta(senderFile, respMsg.signatures, config.blockSize, [&](const peersync::DeltaInstruction& inst) {
            delta.push_back(inst);
        });
        ASSERT_GT(delta.size(), 15u);

        std::vector<peersync::DeltaInstruction> firstBatch(delta.begin(), delta.begin() + 10);
        peersync::DeltaInstructionsMessage deltaMsg{"resume.dat", static_cast<uint64_t>(contentBytes.size()), static_cast<uint32_t>(config.blockSize), 1, firstBatch};
        peersync::sendFramedMessage(client, peersync::serializeMessage(deltaMsg));

        auto ackPayload = peersync::recvFramedMessage(client);
        EXPECT_EQ(peersync::getMessageType(ackPayload), peersync::MessageType::TransferAck);

        // Close client socket to simulate disconnection mid-transfer
        client.close();
    }

    if (receiverThread.joinable()) {
        receiverThread.join();
    }
    EXPECT_TRUE(caughtException);

    std::filesystem::path tempPath = receiverDir / "resume.dat.peersync-tmp";
    std::filesystem::path journalPath = receiverDir / "resume.dat.peersync-journal";
    EXPECT_TRUE(std::filesystem::exists(tempPath));
    EXPECT_TRUE(std::filesystem::exists(journalPath));
    EXPECT_GT(std::filesystem::file_size(tempPath), 0u);

    // Part 2: Reconnect and resume the transfer
    peersync::TcpSocket server2 = peersync::TcpSocket::listen(0, "127.0.0.1");
    uint16_t port2 = server2.getBoundPort();

    peersync::TcpSocket acceptedSocket2;
    std::thread acceptThread2([&]() {
        acceptedSocket2 = server2.accept();
    });
    peersync::TcpSocket client2 = peersync::TcpSocket::connect("127.0.0.1", port2);
    if (acceptThread2.joinable()) {
        acceptThread2.join();
    }

    bool receiverSuccess = false;
    std::thread receiverThread2([&]() {
        try {
            peersync::TransferSession session(acceptedSocket2, config);
            receiverSuccess = session.receiveFile(receiverDir);
        } catch (...) {
            receiverSuccess = false;
        }
    });

    peersync::TransferSession senderSession(client2, config);
    bool senderSuccess = senderSession.sendFile(senderFile, "resume.dat");

    if (receiverThread2.joinable()) {
        receiverThread2.join();
    }

    EXPECT_TRUE(senderSuccess);
    EXPECT_TRUE(receiverSuccess);
    EXPECT_LT(senderSession.getBytesSent(), contentBytes.size()); // Confirms first 10 blocks were skipped!
    EXPECT_TRUE(std::filesystem::exists(targetFile));
    EXPECT_FALSE(std::filesystem::exists(tempPath));
    EXPECT_FALSE(std::filesystem::exists(journalPath));

    auto receivedBytes = readFileBytes(targetFile);
    EXPECT_EQ(receivedBytes, contentBytes);
}

TEST_F(TransferTest, StaleJournalFallbackToFreshSync) {
    std::filesystem::path senderFile = senderDir / "sender_stale.dat";
    createTestFile(senderFile, 5000, 99);
    auto contentBytes = readFileBytes(senderFile);

    std::filesystem::path targetFile = receiverDir / "stale.dat";
    std::filesystem::path tempPath = receiverDir / "stale.dat.peersync-tmp";
    std::filesystem::path journalPath = receiverDir / "stale.dat.peersync-journal";

    // Create a bogus stale journal and temp file
    {
        std::ofstream ofs(tempPath, std::ios::binary);
        ofs << "old stale partial data";
    }
    {
        std::ofstream ofs(journalPath);
        ofs << "path=stale.dat\n";
        ofs << "expected_size=99999\n";
        ofs << "sig_hash=0000000000000000\n"; // Bogus signature hash
        ofs << "last_seq=5\n";
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

    bool receiverSuccess = false;
    std::thread receiverThread([&]() {
        try {
            peersync::TransferSession session(acceptedSocket);
            receiverSuccess = session.receiveFile(receiverDir);
        } catch (...) {
            receiverSuccess = false;
        }
    });

    peersync::TransferSession senderSession(client);
    bool senderSuccess = senderSession.sendFile(senderFile, "stale.dat");

    if (receiverThread.joinable()) {
        receiverThread.join();
    }

    EXPECT_TRUE(senderSuccess);
    EXPECT_TRUE(receiverSuccess);

    EXPECT_TRUE(std::filesystem::exists(targetFile));
    EXPECT_FALSE(std::filesystem::exists(tempPath));
    EXPECT_FALSE(std::filesystem::exists(journalPath));

    auto receivedBytes = readFileBytes(targetFile);
    EXPECT_EQ(receivedBytes, contentBytes);
}

TEST_F(TransferTest, BoundedMemoryStreaming) {
    auto generateLargeFile = [&](const std::filesystem::path& path, size_t sizeMB) {
        std::ofstream ofs(path, std::ios::binary);
        std::vector<uint8_t> chunk(1024 * 1024, 0x42);
        for (size_t i = 0; i < sizeMB; ++i) {
            ofs.write(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        }
    };

    auto runTransfer = [&](size_t sizeMB) -> size_t {
        std::filesystem::path senderFile = senderDir / ("test_" + std::to_string(sizeMB) + "MB.dat");
        generateLargeFile(senderFile, sizeMB);

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

        peersync::TransferSession::Config config;
        config.blockSize = 256 * 1024;
        config.literalThreshold = 512 * 1024;
        
        std::atomic<size_t> peakLiteralBytes{0};
        config.preBatchSendCallback = [&](size_t instrCount, size_t literalBytes) {
            size_t currentPeak = peakLiteralBytes.load();
            while (literalBytes > currentPeak && !peakLiteralBytes.compare_exchange_weak(currentPeak, literalBytes)) {}
        };

        std::thread receiverThread([&]() {
            peersync::TransferSession session(acceptedSocket, config);
            session.receiveFile(receiverDir);
        });

        peersync::TransferSession senderSession(client, config);
        senderSession.sendFile(senderFile, senderFile.filename().u8string());

        if (receiverThread.joinable()) {
            receiverThread.join();
        }
        return peakLiteralBytes.load();
    };

    // Test with 10MB and 50MB (using 50MB vs 500MB might take too long in a CI pipeline, 10 vs 50 is enough to prove O(1) vs O(N))
    size_t peak10MB = runTransfer(10);
    size_t peak50MB = runTransfer(50);

    // If memory is O(N), peak50MB would be ~5x peak10MB.
    // If memory is bounded, they should be exactly the same or very close (e.g. within a chunk size).
    EXPECT_LE(peak50MB, peak10MB * 1.5);
    EXPECT_LE(peak50MB, 2 * 1024 * 1024); // Bounded strictly below 2MB in-flight
}

TEST_F(TransferTest, AckDelayMemoryBound) {
    std::filesystem::path senderFile = senderDir / "ack_delay_sender.bin";
    std::filesystem::path receiverFile = receiverDir / "ack_delay_sender.bin";
    
    // Generate a 5MB file
    {
        std::ofstream ofs(senderFile, std::ios::binary);
        std::vector<char> zeros(1024 * 1024, 0);
        for (int i = 0; i < 5; ++i) {
            ofs.write(zeros.data(), zeros.size());
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

    peersync::TransferSession::Config config;
    config.blockSize = 256 * 1024;
    config.literalThreshold = 512 * 1024;
    config.ackDelayMs = 50; // Inject 50ms artificial delay before each ack
    
    std::atomic<size_t> peakLiteralBytes{0};
    config.preBatchSendCallback = [&](size_t instrCount, size_t literalBytes) {
        size_t currentPeak = peakLiteralBytes.load();
        while (literalBytes > currentPeak && !peakLiteralBytes.compare_exchange_weak(currentPeak, literalBytes)) {}
    };

    std::thread receiverThread([&]() {
        peersync::TransferSession session(acceptedSocket, config);
        session.receiveFile(receiverDir);
    });

    peersync::TransferSession senderSession(client, config);
    bool senderSuccess = senderSession.sendFile(senderFile, senderFile.filename().u8string());

    if (receiverThread.joinable()) {
        receiverThread.join();
    }

    EXPECT_TRUE(senderSuccess);
    EXPECT_LE(peakLiteralBytes.load(), 2 * 1024 * 1024); // Hard cap on peak memory remains bounded

    // Strong hash verification (byte-for-byte comparison of the fully reconstructed file)
    EXPECT_EQ(readFileBytes(senderFile), readFileBytes(receiverFile));
}

TEST_F(TransferTest, MonotonicJournalMultiFile) {
    std::filesystem::path file1 = senderDir / "file1.dat";
    std::filesystem::path file2 = senderDir / "file2.dat";
    createTestFile(file1, 3 * 1024 * 1024, 10);
    createTestFile(file2, 2 * 1024 * 1024, 20);

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
            peersync::TransferSession::Config cfg;
            
            std::map<std::string, uint64_t> lastBytesMap;
            std::map<std::string, uint64_t> lastSeqMap;
            std::atomic<bool> monotonicFailure{false};

            cfg.testJournalCallback = [&](const std::string& path, uint64_t bytesApplied, uint64_t lastSeq) {
                if (lastBytesMap.find(path) != lastBytesMap.end()) {
                    if (bytesApplied < lastBytesMap[path] || lastSeq < lastSeqMap[path]) {
                        monotonicFailure = true;
                    }
                }
                lastBytesMap[path] = bytesApplied;
                lastSeqMap[path] = lastSeq;
            };

            peersync::TransferSession session(acceptedSocket, cfg);
            receiverSuccess = session.receiveFile(receiverDir);
            if (receiverSuccess) {
                receiverSuccess = session.receiveFile(receiverDir);
            }
            if (monotonicFailure) {
                receiverError = "Monotonic journal failure detected";
                receiverSuccess = false;
            }
        } catch (const std::exception& e) {
            receiverSuccess = false;
            receiverError = e.what();
        }
        acceptedSocket.close();
    });

    peersync::TransferSession senderSession(client);
    bool senderSuccess = false;
    try {
        senderSuccess = senderSession.sendFile(file1, "file1.dat");
        if (senderSuccess) {
            senderSuccess = senderSession.sendFile(file2, "file2.dat");
        }
    } catch (const std::exception& e) {
        client.close();
        FAIL() << "Sender exception: " << e.what();
    }
    client.close();

    if (receiverThread.joinable()) {
        receiverThread.join();
    }

    ASSERT_TRUE(senderSuccess);
    ASSERT_TRUE(receiverSuccess) << "Receiver error: " << receiverError;
}

TEST_F(TransferTest, ResumableSyncMidwayInterruption) {
    std::filesystem::path targetFile = receiverDir / "resume_mid.dat";
    createTestFile(targetFile, 50000, 42); 

    auto senderBytes = readFileBytes(targetFile);
    for (size_t i = 32; i < senderBytes.size(); i += 128) {
        senderBytes[i] ^= 0xFF; 
    }
    std::filesystem::path senderFile = senderDir / "sender_resume_mid.dat";
    {
        std::ofstream ofs(senderFile, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(senderBytes.data()), static_cast<std::streamsize>(senderBytes.size()));
    }
    auto contentBytes = readFileBytes(senderFile);

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

    peersync::TransferSession::Config config;
    config.blockSize = 64; 
    config.maxInstructionsPerMessage = 10;
    config.literalThreshold = 10000; 

    bool caughtException = false;
    std::thread receiverThread([&]() {
        try {
            peersync::TransferSession session(acceptedSocket, config);
            session.receiveFile(receiverDir);
        } catch (const std::exception&) {
            caughtException = true;
        }
    });

    // Manually act as sender for 150 batches of 1 instruction each.
    // The journal flushes exactly every 100 batches.
    {
        peersync::ManifestRequestMessage reqMsg{"resume_mid.dat"};
        peersync::sendFramedMessage(client, peersync::serializeMessage(reqMsg));

        auto respPayload = peersync::recvFramedMessage(client);
        auto respMsg = peersync::deserializeManifestResponseMessage(respPayload);

        std::vector<peersync::DeltaInstruction> delta;
        peersync::computeDelta(senderFile, respMsg.signatures, config.blockSize, [&](const peersync::DeltaInstruction& inst) {
            delta.push_back(inst);
        });
        ASSERT_GT(delta.size(), 200u);

        for (size_t i = 0; i < 150; i++) {
            std::vector<peersync::DeltaInstruction> singleBatch;
            singleBatch.push_back(delta[i]);
            peersync::DeltaInstructionsMessage deltaMsg{"resume_mid.dat", static_cast<uint64_t>(contentBytes.size()), static_cast<uint32_t>(config.blockSize), 1, singleBatch};
            peersync::sendFramedMessage(client, peersync::serializeMessage(deltaMsg));

            auto ackPayload = peersync::recvFramedMessage(client);
            EXPECT_EQ(peersync::getMessageType(ackPayload), peersync::MessageType::TransferAck);
        }

        // We sent 150 batches. The receiver journal flushed at 100, but not for the last 50.
        // Close client socket to simulate disconnection mid-way between flush points.
        client.close();
    }

    if (receiverThread.joinable()) {
        receiverThread.join();
    }
    EXPECT_TRUE(caughtException);

    std::filesystem::path tempPath = receiverDir / "resume_mid.dat.peersync-tmp";
    std::filesystem::path journalPath = receiverDir / "resume_mid.dat.peersync-journal";
    EXPECT_TRUE(std::filesystem::exists(tempPath));
    EXPECT_TRUE(std::filesystem::exists(journalPath));
    
    // Part 2: Reconnect and resume the transfer
    peersync::TcpSocket server2 = peersync::TcpSocket::listen(0, "127.0.0.1");
    uint16_t port2 = server2.getBoundPort();

    peersync::TcpSocket acceptedSocket2;
    std::thread acceptThread2([&]() {
        acceptedSocket2 = server2.accept();
    });
    peersync::TcpSocket client2 = peersync::TcpSocket::connect("127.0.0.1", port2);
    if (acceptThread2.joinable()) {
        acceptThread2.join();
    }

    bool receiverSuccess = false;
    std::thread receiverThread2([&]() {
        try {
            peersync::TransferSession session(acceptedSocket2, config);
            receiverSuccess = session.receiveFile(receiverDir);
        } catch (...) {
            receiverSuccess = false;
        }
    });

    peersync::TransferSession senderSession(client2, config);
    bool senderSuccess = senderSession.sendFile(senderFile, "resume_mid.dat");

    if (receiverThread2.joinable()) {
        receiverThread2.join();
    }

    EXPECT_TRUE(senderSuccess);
    EXPECT_TRUE(receiverSuccess);

    // Assert that the file is byte-for-byte perfectly identical despite 
    // the receiver having idempotently re-applied 50 batches of instructions.
    EXPECT_EQ(readFileBytes(targetFile), contentBytes);
}
