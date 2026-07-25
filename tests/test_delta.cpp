#include <gtest/gtest.h>
#include <peersync/delta.h>
#include <peersync/exceptions.h>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>

namespace fs = std::filesystem;

class DeltaTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir_ = fs::temp_directory_path() / "peersync_test_delta";
        fs::create_directories(testDir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(testDir_, ec);
    }

    fs::path createTempFile(const std::string& name, const std::vector<uint8_t>& data) {
        fs::path filePath = testDir_ / name;
        std::ofstream ofs(filePath, std::ios::binary);
        if (!data.empty()) {
            ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
        }
        return filePath;
    }

    fs::path testDir_;
};

TEST_F(DeltaTest, ExactMultipleOfBlockSize) {
    // 120 bytes file, blockSize = 40 => exactly 3 blocks, no short last block
    std::vector<uint8_t> data(120);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }
    fs::path file = createTempFile("exact.bin", data);

    auto sigs = peersync::computeSignatures(file, 40);
    ASSERT_EQ(sigs.size(), 3u);

    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(sigs[i].blockIndex, static_cast<uint64_t>(i));
        uint32_t expectedWeak = peersync::computeAdler32(&data[i * 40], 40);
        uint64_t expectedStrong = peersync::computeXxHash64(&data[i * 40], 40);
        EXPECT_EQ(sigs[i].weakChecksum, expectedWeak);
        EXPECT_EQ(sigs[i].strongHash, expectedStrong);
    }
}

TEST_F(DeltaTest, NotExactMultipleProducesShorterFinalBlock) {
    // 100 bytes file, blockSize = 40 => 3 blocks: two 40-byte blocks, one 20-byte block
    std::vector<uint8_t> data(100);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 11 + 3) & 0xFF);
    }
    fs::path file = createTempFile("short_last.bin", data);

    auto sigs = peersync::computeSignatures(file, 40);
    ASSERT_EQ(sigs.size(), 3u);

    // Block 0 (40 bytes)
    EXPECT_EQ(sigs[0].weakChecksum, peersync::computeAdler32(&data[0], 40));
    EXPECT_EQ(sigs[0].strongHash, peersync::computeXxHash64(&data[0], 40));

    // Block 1 (40 bytes)
    EXPECT_EQ(sigs[1].weakChecksum, peersync::computeAdler32(&data[40], 40));
    EXPECT_EQ(sigs[1].strongHash, peersync::computeXxHash64(&data[40], 40));

    // Block 2 (shorter 20-byte final block)
    EXPECT_EQ(sigs[2].weakChecksum, peersync::computeAdler32(&data[80], 20));
    EXPECT_EQ(sigs[2].strongHash, peersync::computeXxHash64(&data[80], 20));
}

TEST_F(DeltaTest, EmptyFileProducesZeroSignaturesWithoutCrashing) {
    fs::path file = createTempFile("empty.bin", {});
    auto sigs = peersync::computeSignatures(file, 40);
    EXPECT_TRUE(sigs.empty());
}

TEST_F(DeltaTest, IdenticalFilesProduceIdenticalSignatures) {
    std::vector<uint8_t> data(250);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 3 + 1) & 0xFF);
    }
    fs::path file1 = createTempFile("file1.bin", data);
    fs::path file2 = createTempFile("file2.bin", data);

    auto sigs1 = peersync::computeSignatures(file1, 64);
    auto sigs2 = peersync::computeSignatures(file2, 64);

    ASSERT_EQ(sigs1.size(), sigs2.size());
    for (size_t i = 0; i < sigs1.size(); ++i) {
        EXPECT_EQ(sigs1[i].blockIndex, sigs2[i].blockIndex);
        EXPECT_EQ(sigs1[i].weakChecksum, sigs2[i].weakChecksum);
        EXPECT_EQ(sigs1[i].strongHash, sigs2[i].strongHash);
    }
}

TEST_F(DeltaTest, OneBlockDifferenceProducesExactlyOneSignatureDifference) {
    std::vector<uint8_t> data1(200, 0xAA);
    std::vector<uint8_t> data2 = data1;

    // Modify only block 1 (bytes 50 to 99 for blockSize = 50)
    data2[75] = 0xBB;

    fs::path file1 = createTempFile("diff1.bin", data1);
    fs::path file2 = createTempFile("diff2.bin", data2);

    auto sigs1 = peersync::computeSignatures(file1, 50);
    auto sigs2 = peersync::computeSignatures(file2, 50);

    ASSERT_EQ(sigs1.size(), 4u);
    ASSERT_EQ(sigs2.size(), 4u);

    // Block 0 identical
    EXPECT_EQ(sigs1[0].weakChecksum, sigs2[0].weakChecksum);
    EXPECT_EQ(sigs1[0].strongHash, sigs2[0].strongHash);

    // Block 1 differs
    EXPECT_NE(sigs1[1].weakChecksum, sigs2[1].weakChecksum);
    EXPECT_NE(sigs1[1].strongHash, sigs2[1].strongHash);

    // Block 2 identical
    EXPECT_EQ(sigs1[2].weakChecksum, sigs2[2].weakChecksum);
    EXPECT_EQ(sigs1[2].strongHash, sigs2[2].strongHash);

    // Block 3 identical
    EXPECT_EQ(sigs1[3].weakChecksum, sigs2[3].weakChecksum);
    EXPECT_EQ(sigs1[3].strongHash, sigs2[3].strongHash);
}
