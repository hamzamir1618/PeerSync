#ifndef PEERSYNC_DELTA_H
#define PEERSYNC_DELTA_H

#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>
#include <filesystem>

namespace peersync {

struct BlockSignature {
    uint32_t weakChecksum;
    uint64_t strongHash;
    uint64_t blockIndex;
};

enum class DeltaInstructionType : uint8_t {
    Copy = 1,
    Literal = 2
};

struct DeltaInstruction {
    DeltaInstructionType type;
    uint64_t blockIndex;
    std::vector<uint8_t> bytes;

    static DeltaInstruction Copy(uint64_t index) {
        return {DeltaInstructionType::Copy, index, {}};
    }

    static DeltaInstruction Literal(std::vector<uint8_t> data) {
        return {DeltaInstructionType::Literal, 0, std::move(data)};
    }
};

// Computes an Adler-32 rolling weak checksum over a buffer of bytes
uint32_t computeAdler32(const uint8_t* data, size_t len);

// Rolls an Adler-32 checksum by removing outByte and adding inByte over a window of winLen
uint32_t rollAdler32(uint32_t oldChecksum, uint8_t outByte, uint8_t inByte, size_t winLen);

// Computes an xxHash64 strong hash over a buffer of bytes
uint64_t computeXxHash64(const uint8_t* data, size_t len);

// Reads the file in blockSize-sized chunks (last block may be shorter), computing for each block
// an Adler-32 weak checksum and an xxHash64 strong hash.
std::vector<BlockSignature> computeSignatures(const std::filesystem::path& file, size_t blockSize);

// Scans newFile using a rolling checksum against oldFileSignatures to produce delta instructions.
std::vector<DeltaInstruction> computeDelta(const std::filesystem::path& newFile,
                                           const std::vector<BlockSignature>& oldFileSignatures,
                                           size_t blockSize);

} // namespace peersync

#endif // PEERSYNC_DELTA_H
