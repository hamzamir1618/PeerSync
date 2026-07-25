#ifndef PEERSYNC_DELTA_H
#define PEERSYNC_DELTA_H

#include <vector>
#include <cstdint>
#include <cstddef>
#include <filesystem>

namespace peersync {

struct BlockSignature {
    uint32_t weakChecksum;
    uint64_t strongHash;
    uint64_t blockIndex;
};

// Computes an Adler-32 rolling weak checksum over a buffer of bytes
uint32_t computeAdler32(const uint8_t* data, size_t len);

// Computes an xxHash64 strong hash over a buffer of bytes
uint64_t computeXxHash64(const uint8_t* data, size_t len);

// Reads the file in blockSize-sized chunks (last block may be shorter), computing for each block
// an Adler-32 weak checksum and an xxHash64 strong hash.
std::vector<BlockSignature> computeSignatures(const std::filesystem::path& file, size_t blockSize);

} // namespace peersync

#endif // PEERSYNC_DELTA_H
