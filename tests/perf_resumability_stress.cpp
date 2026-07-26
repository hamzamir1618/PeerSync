#include <gtest/gtest.h>
#include <peersync/sync_orchestrator.h>
#include <peersync/socket.h>
#include <peersync/transfer.h>
#include <peersync/exceptions.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <string>
#include <random>
#include <chrono>
#include <atomic>
#include <iostream>
#include <cstring>


namespace {

class ResumabilityStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::random_device rd;
        testDir = std::filesystem::temp_directory_path() / ("peersync_stress_" + std::to_string(rd()));
        dirA = testDir / "source";
        dirB = testDir / "destination";
        std::filesystem::create_directories(dirA);
        std::filesystem::create_directories(dirB);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }

    void createTestFile(const std::filesystem::path& path, size_t size, uint32_t seed) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        std::mt19937 rng(seed);
        std::vector<char> buffer(4096);
        size_t written = 0;
        while (written < size) {
            size_t toWrite = std::min(buffer.size(), size - written);
            for (size_t i = 0; i < toWrite; ++i) {
                buffer[i] = static_cast<char>(rng() & 0xFF);
            }
            ofs.write(buffer.data(), static_cast<std::streamsize>(toWrite));
            written += toWrite;
        }
    }

    void generateSourceDirectory() {
        // Subdirectory small/
        createTestFile(dirA / "small" / "empty.dat", 0, 101);
        createTestFile(dirA / "small" / "note.txt", 120, 102);
        createTestFile(dirA / "small" / "config.json", 1500, 103);
        createTestFile(dirA / "small" / "icon.png", 8192, 104);
        createTestFile(dirA / "small" / "script.py", 25000, 105);

        // Subdirectory medium/
        createTestFile(dirA / "medium" / "data1.bin", 150 * 1024, 201);
        createTestFile(dirA / "medium" / "data2.bin", 350 * 1024, 202);
        createTestFile(dirA / "medium" / "data3.bin", 600 * 1024, 203);

        // Subdirectory large/
        createTestFile(dirA / "large" / "archive1.zip", 1500 * 1024, 301);
        createTestFile(dirA / "large" / "video1.mp4", 3500 * 1024, 302);
    }

    bool verifyDirectoryIdentical(const std::filesystem::path& srcDir, const std::filesystem::path& dstDir, std::string& err) {
        std::error_code ec;
        if (!std::filesystem::exists(dstDir, ec) || !std::filesystem::is_directory(dstDir, ec)) {
            err = "Destination directory does not exist or is not a directory.";
            return false;
        }

        // Check no leftover .peersync-tmp or .peersync-journal files
        for (auto it = std::filesystem::recursive_directory_iterator(dstDir, ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (ec) break;
            const auto& entry = *it;
            if (entry.is_directory(ec)) continue;
            std::string filename = entry.path().filename().string();
            if (filename.find(".peersync-tmp") != std::string::npos ||
                filename.find(".peersync-journal") != std::string::npos) {
                err = "Found leftover temporary/journal file: " + entry.path().string();
                return false;
            }
        }

        // Check every file in srcDir exists in dstDir and matches byte-for-byte
        for (auto it = std::filesystem::recursive_directory_iterator(srcDir, ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (ec) break;
            const auto& srcEntry = *it;
            if (srcEntry.is_directory(ec)) continue;

            auto relPath = std::filesystem::relative(srcEntry.path(), srcDir, ec);
            if (ec) {
                err = "Failed to compute relative path for: " + srcEntry.path().string();
                return false;
            }

            std::filesystem::path dstPath = dstDir / relPath;
            if (!std::filesystem::exists(dstPath, ec)) {
                err = "Missing file in destination: " + relPath.generic_string();
                return false;
            }

            uint64_t srcSize = std::filesystem::file_size(srcEntry.path(), ec);
            uint64_t dstSize = std::filesystem::file_size(dstPath, ec);
            if (srcSize != dstSize) {
                err = "Size mismatch for " + relPath.generic_string() + ": src=" + std::to_string(srcSize) + ", dst=" + std::to_string(dstSize);
                return false;
            }

            std::ifstream srcIfs(srcEntry.path(), std::ios::binary);
            std::ifstream dstIfs(dstPath, std::ios::binary);
            std::vector<char> srcBuf(4096), dstBuf(4096);
            uint64_t checked = 0;
            while (checked < srcSize) {
                srcIfs.read(srcBuf.data(), srcBuf.size());
                dstIfs.read(dstBuf.data(), dstBuf.size());
                if (srcIfs.gcount() != dstIfs.gcount() ||
                    std::memcmp(srcBuf.data(), dstBuf.data(), static_cast<size_t>(srcIfs.gcount())) != 0) {
                    err = "Content byte mismatch for " + relPath.generic_string() + " at offset " + std::to_string(checked);
                    return false;
                }
                checked += srcIfs.gcount();
            }
        }

        return true;
    }

    std::filesystem::path testDir;
    std::filesystem::path dirA;
    std::filesystem::path dirB;
};

TEST_F(ResumabilityStressTest, MultiFileDirectorySyncWithRandomInterruptions) {
    std::cout << "\n[ResumabilityStressTest] Generating multi-file source directory (~6.1 MB across 10 files)...\n";
    generateSourceDirectory();

    const int numTrials = 3;
    for (int trial = 1; trial <= numTrials; ++trial) {
        std::cout << "\n=========================================================\n";
        std::cout << " [TRIAL " << trial << " OF " << numTrials << "] Starting Random Interruption Sequence\n";
        std::cout << "=========================================================\n";

        std::error_code ec;
        std::filesystem::remove_all(dirB, ec);
        std::filesystem::create_directories(dirB, ec);

        std::mt19937 rng(static_cast<uint32_t>(1337 + trial * 999));
        std::uniform_int_distribution<uint64_t> distBytes(150 * 1024, 1000 * 1024); // Interrupt after 150 KB to 1 MB

        bool trialSucceeded = false;
        int totalAttempts = 0;
        const int maxAttempts = 30;

        for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
            totalAttempts = attempt;
            std::string verificationErr;
            if (verifyDirectoryIdentical(dirA, dirB, verificationErr)) {
                std::cout << "[Trial " << trial << "] Directory byte-identical! Sync complete in " << (attempt - 1) << " attempts.\n";
                trialSucceeded = true;
                break;
            }

            uint64_t killThreshold = distBytes(rng);
            // Every 4th attempt or after attempt 12, let it run to completion without interruption
            if (attempt % 4 == 0 || attempt >= 12) {
                killThreshold = UINT64_MAX;
            }

            if (killThreshold == UINT64_MAX) {
                std::cout << "[Trial " << trial << ", Attempt " << attempt << "] Running without interruption threshold...\n";
            } else {
                std::cout << "[Trial " << trial << ", Attempt " << attempt << "] Will interrupt after ~" << (killThreshold / 1024) << " KB transferred...\n";
            }

            peersync::TcpSocket server = peersync::TcpSocket::listen(0, "127.0.0.1");
            uint16_t port = server.getBoundPort();

            peersync::TcpSocket acceptedSocket;
            std::thread acceptThread([&]() {
                try {
                    acceptedSocket = server.accept();
                } catch (...) {}
            });

            peersync::TcpSocket client;
            try {
                client = peersync::TcpSocket::connect("127.0.0.1", port);
            } catch (...) {
                if (acceptThread.joinable()) acceptThread.join();
                continue;
            }
            if (acceptThread.joinable()) acceptThread.join();

            if (!client.isValid() || !acceptedSocket.isValid()) {
                continue;
            }

            peersync::SyncPolicy policy;
            policy.direction = peersync::SyncPolicy::Direction::Bidirectional;
            policy.maxConcurrency = 2; // Exercise concurrent multi-socket interruption and resumption
            policy.allowResume = true;
            policy.transferConfig.blockSize = 1024;
            policy.transferConfig.maxInstructionsPerMessage = 40; // Ensure frequent checkpointing

            std::atomic<uint64_t> bytesThisAttempt{0};
            std::atomic<bool> killTriggered{false};
            std::atomic<bool> threadsRunning{true};

            auto checkKill = [&]() -> bool {
                if (killTriggered.load()) return true;
                if (bytesThisAttempt.load() >= killThreshold) {
                    killTriggered = true;
                    return true;
                }
                return false;
            };

            policy.isCancelled = checkKill;
            policy.transferConfig.isCancelled = checkKill;
            policy.transferConfig.progressCallback = [&](uint64_t, uint64_t, uint64_t) {
                bytesThisAttempt.fetch_add(40 * 1024); // ~40 KB per instruction batch
                checkKill();
            };

            std::thread watchdog([&]() {
                while (threadsRunning.load()) {
                    if (killTriggered.load()) {
                        client.close();
                        acceptedSocket.close();
                        server.close();
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });

            bool responderSuccess = false;
            std::thread responderThread([&]() {
                try {
                    peersync::SyncOrchestrator responder(acceptedSocket, peersync::SyncOrchestrator::Role::Responder, policy);
                    responderSuccess = responder.syncDirectory(dirB);
                } catch (const std::exception& e) {
                    if (!killTriggered.load()) {
                        std::cerr << "  [Responder Exception] " << e.what() << "\n";
                    }
                    responderSuccess = false;
                } catch (...) {
                    responderSuccess = false;
                }
                acceptedSocket.close();
            });

            bool initiatorSuccess = false;
            try {
                peersync::SyncOrchestrator initiator(client, peersync::SyncOrchestrator::Role::Initiator, policy);
                initiatorSuccess = initiator.syncDirectory(dirA);
            } catch (const std::exception& e) {
                if (!killTriggered.load()) {
                    std::cerr << "  [Initiator Exception] " << e.what() << "\n";
                }
                initiatorSuccess = false;
            } catch (...) {
                initiatorSuccess = false;
            }
            client.close();

            if (responderThread.joinable()) responderThread.join();
            threadsRunning = false;
            if (watchdog.joinable()) watchdog.join();
            server.close();

            if (killTriggered.load()) {
                std::cout << "  -> Interruption triggered after ~" << (bytesThisAttempt.load() / 1024) << " KB.\n";
            } else if (initiatorSuccess && responderSuccess) {
                std::cout << "  -> Sync completed without interruption.\n";
            } else {
                std::cout << "  -> Sync terminated (initiatorSuccess=" << initiatorSuccess << ", responderSuccess=" << responderSuccess << ").\n";
            }
        }

        std::string finalErr;
        bool finalMatch = verifyDirectoryIdentical(dirA, dirB, finalErr);
        EXPECT_TRUE(finalMatch) << "Trial " << trial << " failed after " << totalAttempts << " attempts! Error: " << finalErr;
        if (finalMatch) {
            std::cout << "[Trial " << trial << "] SUCCESS: Verified byte-identical destination directory with zero leftover temporary files.\n";
        }
    }
}

} // anonymous namespace
