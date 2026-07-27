#include <peersync/delta.h>
#include <peersync/exceptions.h>
#include <fstream>
#include <unordered_map>
#include <algorithm>
#include <utility>
#include <xxhash.h>
#include <random>

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

uint32_t rollAdler32(uint32_t oldChecksum, uint8_t outByte, uint8_t inByte, size_t winLen) {
    const uint32_t MOD_ADLER = 65521;
    uint32_t a = oldChecksum & 0xFFFF;
    uint32_t b = (oldChecksum >> 16) & 0xFFFF;

    uint32_t new_a = (a + MOD_ADLER - (outByte % MOD_ADLER) + (inByte % MOD_ADLER)) % MOD_ADLER;
    uint32_t term = static_cast<uint32_t>(((static_cast<uint64_t>(winLen) % MOD_ADLER) * outByte) % MOD_ADLER);
    uint32_t new_b = (b + MOD_ADLER - term + new_a + MOD_ADLER - 1) % MOD_ADLER;

    return (new_b << 16) | new_a;
}

uint64_t computeXxHash64(const uint8_t* data, size_t len) {
    return XXH64(data, len, 0);
}

std::vector<BlockSignature> computeSignatures(const std::filesystem::path& file, size_t blockSize) {
    if (blockSize == 0) {
        throw PeerSyncDeltaException("Block size must be greater than zero");
    }

    std::vector<BlockSignature> signatures;

    if (blockSize == 0 || blockSize > 1024 * 1024 * 64) {
        throw PeerSyncDeltaException("Block size must be between 1 and 64 MB");
    }
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

std::vector<DeltaInstruction> computeDelta(const std::filesystem::path& newFile,
                                           const std::vector<BlockSignature>& oldFileSignatures,
                                           size_t blockSize) {
    if (blockSize == 0 || blockSize > 1024 * 1024 * 64) {
        throw PeerSyncDeltaException("Block size must be between 1 and 64 MB");
    }

    std::error_code ec;
    if (!std::filesystem::exists(newFile, ec) || ec) {
        throw PeerSyncDeltaException("File does not exist: " + newFile.string());
    }

    uint64_t fileSize = std::filesystem::file_size(newFile, ec);
    if (ec) {
        throw PeerSyncDeltaException("Failed to get file size: " + newFile.string());
    }

    std::vector<DeltaInstruction> instructions;
    if (fileSize == 0) {
        return instructions;
    }

    std::ifstream ifs(newFile, std::ios::binary);
    if (!ifs.is_open()) {
        throw PeerSyncDeltaException("Failed to open file for reading: " + newFile.string());
    }

    std::vector<uint8_t> fileData(static_cast<size_t>(fileSize));
    ifs.read(reinterpret_cast<char*>(fileData.data()), static_cast<std::streamsize>(fileSize));
    if (ifs.gcount() != static_cast<std::streamsize>(fileSize)) {
        throw PeerSyncDeltaException("Failed to read complete file: " + newFile.string());
    }

    struct Candidate {
        uint64_t blockIndex;
        uint64_t strongHash;
    };

    std::unordered_map<uint32_t, std::vector<Candidate>> signatureMap;
    for (const auto& sig : oldFileSignatures) {
        signatureMap[sig.weakChecksum].push_back({sig.blockIndex, sig.strongHash});
    }

    size_t i = 0;
    std::vector<uint8_t> pendingLiteral;
    uint32_t currentWeak = 0;
    bool haveRollingChecksum = false;

    while (i < fileData.size()) {
        size_t winLen = std::min(blockSize, fileData.size() - i);

        if (!haveRollingChecksum) {
            currentWeak = computeAdler32(&fileData[i], winLen);
            haveRollingChecksum = true;
        }

        bool matched = false;
        auto it = signatureMap.find(currentWeak);
        if (it != signatureMap.end()) {
            uint64_t strongHash = computeXxHash64(&fileData[i], winLen);
            for (const auto& candidate : it->second) {
                if (candidate.strongHash == strongHash) {
                    if (!pendingLiteral.empty()) {
                        instructions.push_back(DeltaInstruction::Literal(std::move(pendingLiteral)));
                        pendingLiteral.clear();
                    }
                    instructions.push_back(DeltaInstruction::Copy(candidate.blockIndex));
                    i += winLen;
                    haveRollingChecksum = false;
                    matched = true;
                    break;
                }
            }
        }

        if (!matched) {
            pendingLiteral.push_back(fileData[i]);
            if (pendingLiteral.size() >= std::max<size_t>(65536, blockSize)) {
                instructions.push_back(DeltaInstruction::Literal(std::move(pendingLiteral)));
                pendingLiteral.clear();
            }
            if (fileData.size() - i > blockSize) {
                currentWeak = rollAdler32(currentWeak, fileData[i], fileData[i + blockSize], blockSize);
                i++;
            } else {
                i++;
                haveRollingChecksum = false;
            }
        }
    }

    if (!pendingLiteral.empty()) {
        instructions.push_back(DeltaInstruction::Literal(std::move(pendingLiteral)));
    }

    return instructions;
}

void reconstructFile(const std::filesystem::path& oldFile,
                     const std::vector<DeltaInstruction>& instructions,
                     const std::filesystem::path& outputFile,
                     size_t blockSize) {
    if (blockSize == 0 || blockSize > 1024 * 1024 * 64) {
        throw PeerSyncDeltaException("Block size must be between 1 and 64 MB");
    }

    std::error_code ec;
    if (!std::filesystem::exists(oldFile, ec) || ec) {
        throw PeerSyncDeltaException("Old file does not exist: " + oldFile.string());
    }

    uint64_t oldFileSize = std::filesystem::file_size(oldFile, ec);
    if (ec) {
        throw PeerSyncDeltaException("Failed to get old file size: " + oldFile.string());
    }

    std::filesystem::path parentDir = outputFile.parent_path();
    if (parentDir.empty()) {
        parentDir = ".";
    }
    if (!std::filesystem::exists(parentDir, ec)) {
        std::filesystem::create_directories(parentDir, ec);
        if (ec) {
            throw PeerSyncDeltaException("Failed to create parent directory for output file: " + outputFile.string());
        }
    }

    std::random_device rd;
    std::filesystem::path tempPath = parentDir / (outputFile.filename().string() + ".tmp." + std::to_string(rd()));

    struct TempFileGuard {
        std::filesystem::path path;
        bool commit = false;
        ~TempFileGuard() {
            if (!commit) {
                std::error_code ec_remove;
                std::filesystem::remove(path, ec_remove);
            }
        }
    } guard{tempPath, false};

    std::ifstream ifs(oldFile, std::ios::binary);
    if (!ifs.is_open()) {
        throw PeerSyncDeltaException("Failed to open old file for reading: " + oldFile.string());
    }

    std::ofstream ofs(tempPath, std::ios::binary);
    if (!ofs.is_open()) {
        throw PeerSyncDeltaException("Failed to open temporary file for writing: " + tempPath.string());
    }

    std::vector<uint8_t> buffer(blockSize);

    for (const auto& inst : instructions) {
        if (inst.type == DeltaInstructionType::Copy) {
            if (oldFileSize == 0) {
                throw PeerSyncDeltaException("Cannot copy from an empty file");
            }
            if (blockSize > 0 && inst.blockIndex > UINT64_MAX / blockSize) {
                throw PeerSyncDeltaException("Copy instruction block index arithmetic overflow");
            }
            uint64_t offset = inst.blockIndex * blockSize;
            if (offset >= oldFileSize) {
                throw PeerSyncDeltaException("Copy instruction block index out of bounds");
            }
            size_t bytesToRead = static_cast<size_t>(std::min(static_cast<uint64_t>(blockSize), oldFileSize - offset));
            ifs.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            if (!ifs) {
                throw PeerSyncDeltaException("Failed to seek in old file");
            }
            ifs.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(bytesToRead));
            if (ifs.gcount() != static_cast<std::streamsize>(bytesToRead)) {
                throw PeerSyncDeltaException("Failed to read expected bytes from old file");
            }
            ofs.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(bytesToRead));
            if (!ofs) {
                throw PeerSyncDeltaException("Failed to write copy block to temporary file");
            }
        } else if (inst.type == DeltaInstructionType::Literal) {
            if (!inst.bytes.empty()) {
                ofs.write(reinterpret_cast<const char*>(inst.bytes.data()), static_cast<std::streamsize>(inst.bytes.size()));
                if (!ofs) {
                    throw PeerSyncDeltaException("Failed to write literal bytes to temporary file");
                }
            }
        } else {
            throw PeerSyncDeltaException("Unknown delta instruction type");
        }
    }

    ifs.close();
    ofs.flush();
    ofs.close();
    if (!ofs) {
        throw PeerSyncDeltaException("Error flushing or closing temporary file");
    }

    std::error_code renameEc;
    std::filesystem::rename(tempPath, outputFile, renameEc);
    if (renameEc) {
        if (std::filesystem::exists(outputFile)) {
            std::error_code removeEc;
            std::filesystem::remove(outputFile, removeEc);
            if (!removeEc) {
                renameEc.clear();
                std::filesystem::rename(tempPath, outputFile, renameEc);
            }
        }
        if (renameEc) {
            throw PeerSyncDeltaException("Failed to rename temporary file to output file: " + renameEc.message());
        }
    }

    guard.commit = true;
}

} // namespace peersync
