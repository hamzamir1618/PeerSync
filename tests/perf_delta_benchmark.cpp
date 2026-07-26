#include <gtest/gtest.h>
#include <peersync/delta.h>
#include <peersync/transfer.h>
#include <peersync/socket.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <random>
#include <iostream>
#include <thread>
#include <cstdlib>
#include <cstring>

namespace {

class PerfDeltaTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::random_device rd;
        testDir = std::filesystem::temp_directory_path() / ("peersync_perf_" + std::to_string(rd()));
        std::filesystem::create_directories(testDir);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }

    size_t getBenchmarkFileSize() const {
        const char* envSize = std::getenv("PEERSYNC_PERF_SIZE_MB");
        if (envSize && strlen(envSize) > 0) {
            try {
                unsigned long mb = std::stoul(envSize);
                if (mb > 0) {
                    return static_cast<size_t>(mb) * 1024ULL * 1024ULL;
                }
            } catch (...) {}
        }
        return 250ULL * 1024ULL * 1024ULL; // Default 250 MB for rapid local iteration
    }

    void createLargeSyntheticFile(const std::filesystem::path& path, size_t size, uint32_t seed = 42) {
        std::ofstream ofs(path, std::ios::binary);
        std::mt19937 rng(seed);
        const size_t chunkSize = 65536;
        std::vector<char> buffer(chunkSize);
        size_t remaining = size;
        while (remaining > 0) {
            size_t toWrite = std::min(remaining, chunkSize);
            for (size_t i = 0; i < toWrite; i += 4) {
                uint32_t val = rng();
                size_t copyBytes = std::min(static_cast<size_t>(4), toWrite - i);
                std::memcpy(&buffer[i], &val, copyBytes);
            }
            ofs.write(buffer.data(), toWrite);
            remaining -= toWrite;
        }
    }

    void createModifiedCopy(const std::filesystem::path& origPath, const std::filesystem::path& modPath) {
        std::ifstream ifs(origPath, std::ios::binary);
        std::ofstream ofs(modPath, std::ios::binary);
        std::vector<char> buffer(65536);
        size_t offset = 0;
        bool inserted = false;
        while (ifs.read(buffer.data(), buffer.size()) || ifs.gcount() > 0) {
            size_t count = static_cast<size_t>(ifs.gcount());
            // In every 10 MB block, modify ~50 bytes
            if ((offset % (10ULL * 1024ULL * 1024ULL)) == 0 && count >= 100) {
                for (size_t i = 10; i < 60; ++i) {
                    buffer[i] = static_cast<char>(buffer[i] ^ 0xFF);
                }
            }
            ofs.write(buffer.data(), count);
            // At ~20 MB mark, insert 1000 new bytes (length-changing insertion)
            if (!inserted && offset >= 20ULL * 1024ULL * 1024ULL) {
                std::vector<char> insertion(1000, 'X');
                ofs.write(insertion.data(), insertion.size());
                inserted = true;
            }
            offset += count;
        }
    }

    bool compareFiles(const std::filesystem::path& p1, const std::filesystem::path& p2) {
        if (std::filesystem::file_size(p1) != std::filesystem::file_size(p2)) return false;
        std::ifstream f1(p1, std::ios::binary);
        std::ifstream f2(p2, std::ios::binary);
        std::vector<char> b1(65536), b2(65536);
        while (true) {
            f1.read(b1.data(), b1.size());
            f2.read(b2.data(), b2.size());
            std::streamsize c1 = f1.gcount();
            std::streamsize c2 = f2.gcount();
            if (c1 != c2) return false;
            if (c1 == 0) break;
            if (std::memcmp(b1.data(), b2.data(), static_cast<size_t>(c1)) != 0) return false;
        }
        return true;
    }

    std::filesystem::path testDir;
};

TEST_F(PerfDeltaTest, InMemoryDeltaEngineBenchmark) {
    const size_t origSize = getBenchmarkFileSize();
    const size_t blockSize = 65536; // 64 KB block size for large files

    std::filesystem::path origPath = testDir / "orig_data.bin";
    std::filesystem::path modPath = testDir / "mod_data.bin";
    std::filesystem::path reconPath = testDir / "recon_data.bin";

    std::cout << "\n[PerfDeltaTest] Generating " << (origSize / (1024.0 * 1024.0)) << " MB synthetic file..." << std::endl;
    createLargeSyntheticFile(origPath, origSize, 12345);

    std::cout << "[PerfDeltaTest] Creating modified copy with edits and length-changing insertion..." << std::endl;
    createModifiedCopy(origPath, modPath);

    const size_t modSize = std::filesystem::file_size(modPath);
    ASSERT_GT(modSize, 0);

    using clock = std::chrono::high_resolution_clock;

    std::cout << "[PerfDeltaTest] Running computeSignatures..." << std::endl;
    auto t0 = clock::now();
    auto sigs = peersync::computeSignatures(origPath, blockSize);
    auto t1 = clock::now();

    std::cout << "[PerfDeltaTest] Running computeDelta..." << std::endl;
    auto deltas = peersync::computeDelta(modPath, sigs, blockSize);
    auto t2 = clock::now();

    std::cout << "[PerfDeltaTest] Running reconstructFile..." << std::endl;
    peersync::reconstructFile(origPath, deltas, reconPath, blockSize);
    auto t3 = clock::now();

    double sigSec = std::chrono::duration<double>(t1 - t0).count();
    double deltaSec = std::chrono::duration<double>(t2 - t1).count();
    double reconSec = std::chrono::duration<double>(t3 - t2).count();
    double totalSec = std::chrono::duration<double>(t3 - t0).count();

    uint64_t literalBytes = 0;
    for (const auto& instr : deltas) {
        if (instr.type == peersync::DeltaInstructionType::Literal) {
            literalBytes += instr.bytes.size();
        }
    }

    double literalPct = (modSize > 0) ? (100.0 * static_cast<double>(literalBytes) / static_cast<double>(modSize)) : 0.0;

    std::cout << "\n=========================================================\n";
    std::cout << " [PERFORMANCE BENCHMARK 1] In-Memory / Local Delta Engine\n";
    std::cout << "=========================================================\n";
    std::cout << " Original File Size:      " << (origSize / (1024.0 * 1024.0)) << " MB\n";
    std::cout << " Modified File Size:      " << (modSize / (1024.0 * 1024.0)) << " MB\n";
    std::cout << " Block Size:              " << (blockSize / 1024) << " KB\n";
    std::cout << " --------------------------------------------------------\n";
    std::cout << " computeSignatures Time:  " << sigSec << " s (" << ((origSize / (1024.0 * 1024.0)) / sigSec) << " MB/s)\n";
    std::cout << " computeDelta Time:       " << deltaSec << " s (" << ((modSize / (1024.0 * 1024.0)) / deltaSec) << " MB/s)\n";
    std::cout << " reconstructFile Time:    " << reconSec << " s (" << ((modSize / (1024.0 * 1024.0)) / reconSec) << " MB/s)\n";
    std::cout << " Total End-to-End Time:   " << totalSec << " s (" << ((modSize / (1024.0 * 1024.0)) / totalSec) << " MB/s)\n";
    std::cout << " --------------------------------------------------------\n";
    std::cout << " Literal Bytes Equivalent: " << literalBytes << " bytes\n";
    std::cout << " Bandwidth % of Full File: " << literalPct << "%\n";
    std::cout << " Bandwidth Savings:        " << (100.0 - literalPct) << "%\n";
    std::cout << "=========================================================\n\n";

    EXPECT_LT(literalPct, 5.0) << "Literal percentage must stay well below 100% (expected < 5% for small edits + insertion)";
    EXPECT_EQ(std::filesystem::file_size(reconPath), modSize);
    EXPECT_TRUE(compareFiles(modPath, reconPath)) << "Reconstructed file must match modified file exactly";
}

TEST_F(PerfDeltaTest, LoopbackTransferSessionBenchmark) {
    const size_t origSize = getBenchmarkFileSize();
    const size_t blockSize = 65536; // 64 KB block size for large files

    std::filesystem::path senderDir = testDir / "sender";
    std::filesystem::path receiverDir = testDir / "receiver";
    std::filesystem::create_directories(senderDir);
    std::filesystem::create_directories(receiverDir);

    std::filesystem::path origPath = senderDir / "data.bin";
    std::filesystem::path modPath = senderDir / "data_mod.bin";

    std::cout << "\n[PerfDeltaTest] Generating " << (origSize / (1024.0 * 1024.0)) << " MB file for loopback transfer..." << std::endl;
    createLargeSyntheticFile(origPath, origSize, 54321);
    createModifiedCopy(origPath, modPath);

    const size_t modSize = std::filesystem::file_size(modPath);

    // Pre-populate receiver directory with origPath as "sync_target.bin" so that receiveFile performs a delta sync!
    std::filesystem::path receiverTarget = receiverDir / "sync_target.bin";
    std::filesystem::copy_file(origPath, receiverTarget, std::filesystem::copy_options::overwrite_existing);

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

    peersync::TransferSession::Config config;
    config.blockSize = blockSize;
    config.literalThreshold = blockSize;
    config.allowResume = false;

    bool receiverSuccess = false;
    std::string receiverError;

    using clock = std::chrono::high_resolution_clock;
    auto t0 = clock::now();

    std::thread receiverThread([&]() {
        try {
            peersync::TransferSession session(acceptedSocket, config);
            receiverSuccess = session.receiveFile(receiverDir);
        } catch (const std::exception& e) {
            receiverSuccess = false;
            receiverError = e.what();
        }
        acceptedSocket.close();
    });

    peersync::TransferSession senderSession(client, config);
    bool senderSuccess = false;
    try {
        senderSuccess = senderSession.sendFile(modPath, "sync_target.bin");
    } catch (const std::exception& e) {
        client.close();
        FAIL() << "Sender exception: " << e.what();
    }
    client.close();

    if (receiverThread.joinable()) {
        receiverThread.join();
    }
    auto t1 = clock::now();

    double wallSec = std::chrono::duration<double>(t1 - t0).count();
    uint64_t bytesSent = senderSession.getBytesSent();
    double netPct = (modSize > 0) ? (100.0 * static_cast<double>(bytesSent) / static_cast<double>(modSize)) : 0.0;

    std::cout << "\n=========================================================\n";
    std::cout << " [PERFORMANCE BENCHMARK 2] Loopback TransferSession Delta\n";
    std::cout << "=========================================================\n";
    std::cout << " Target File Size:        " << (modSize / (1024.0 * 1024.0)) << " MB\n";
    std::cout << " Wall-Clock Transfer Time: " << wallSec << " s\n";
    std::cout << " Effective Throughput:    " << ((modSize / (1024.0 * 1024.0)) / wallSec) << " MB/s\n";
    std::cout << " --------------------------------------------------------\n";
    std::cout << " Actual Wire Bytes Sent:  " << bytesSent << " bytes (" << (bytesSent / (1024.0 * 1024.0)) << " MB)\n";
    std::cout << " Network Bandwidth %:     " << netPct << "%\n";
    std::cout << " Bandwidth Saved vs Full: " << (100.0 - netPct) << "%\n";
    std::cout << "=========================================================\n\n";

    EXPECT_TRUE(senderSuccess);
    EXPECT_TRUE(receiverSuccess) << "Receiver error: " << receiverError;
    EXPECT_LT(netPct, 5.0) << "Network bytes sent should be a tiny fraction of full file size";
    EXPECT_EQ(std::filesystem::file_size(receiverTarget), modSize);
    EXPECT_TRUE(compareFiles(modPath, receiverTarget));
}

} // anonymous namespace
