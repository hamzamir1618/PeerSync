#include <peersync/delta.h>
#include <peersync/exceptions.h>
#include <fstream>
#include <xxhash.h>

namespace peersync {

uint32_t computeAdler32(const uint8_t* data, size_t len) {
    const uint32_t MOD_ADLER = 65521;
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % MOD_ADLER;
        b = (b + a) % MOD_ADLER;
    }
    return (b << 16) | a;
}

uint64_t computeXxHash64(const uint8_t* data, size_t len) {
    return XXH64(data, len, 0);
}

std::vector<BlockSignature> computeSignatures(const std::filesystem::path& file, size_t blockSize) {
    if (blockSize == 0) {
        throw PeerSyncDeltaException("Block size must be greater than zero");
    }

    std::vector<BlockSignature> signatures;

    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        throw PeerSyncDeltaException("File does not exist: " + file.string());
    }

    uint64_t fileSize = std::filesystem::file_size(file, ec);
    if (ec) {
        throw PeerSyncDeltaException("Failed to get file size: " + file.string());
    }

    if (fileSize == 0) {
        return signatures;
    }

    std::ifstream ifs(file, std::ios::binary);
    if (!ifs.is_open()) {
        throw PeerSyncDeltaException("Failed to open file for reading: " + file.string());
    }

    std::vector<uint8_t> buffer(blockSize);
    uint64_t blockIndex = 0;

    while (ifs) {
        ifs.read(reinterpret_cast<char*>(buffer.data()), blockSize);
        std::streamsize bytesRead = ifs.gcount();
        if (bytesRead <= 0) {
            break;
        }

        BlockSignature sig;
        sig.weakChecksum = computeAdler32(buffer.data(), static_cast<size_t>(bytesRead));
        sig.strongHash = computeXxHash64(buffer.data(), static_cast<size_t>(bytesRead));
        sig.blockIndex = blockIndex++;
        signatures.push_back(sig);
    }

    return signatures;
}

} // namespace peersync
