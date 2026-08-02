#include <peersync/delta.h>
#include <peersync/exceptions.h>
#include <fstream>
#include <unordered_map>
#include <algorithm>
#include <utility>
#include <xxhash.h>
#include <random>
#include <chrono>
#include <iostream>
#include <atomic>

namespace peersync {

static std::atomic<size_t> g_adler32CallCount{0};

void resetAdler32CallCount() { g_adler32CallCount = 0; }
size_t getAdler32CallCount() { return g_adler32CallCount.load(); }

uint32_t computeAdler32(const uint8_t* data, size_t len) {
    g_adler32CallCount++;
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
    g_adler32CallCount++;
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

std::vector<BlockSignature> computeSignatures(const std::filesystem::path& file, size_t blockSize, std::function<void(uint64_t processed, uint64_t total)> progressCb) {
    if (blockSize == 0) {
        throw PeerSyncDeltaException("Block size must be greater than zero");
    }

    std::vector<BlockSignature> signatures;

    if (blockSize == 0 || blockSize > 1024 * 1024 * 64) {
        throw PeerSyncDeltaException("Block size must be between 1 and 64 MB");
    }
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        throw PeerSyncDeltaException("File does not exist: " + file.u8string());
    }

    uint64_t fileSize = std::filesystem::file_size(file, ec);
    if (ec) {
        throw PeerSyncDeltaException("Failed to get file size: " + file.u8string());
    }

    if (fileSize == 0) {
        return signatures;
    }

    std::ifstream ifs(file, std::ios::binary);
    if (!ifs.is_open()) {
        throw PeerSyncDeltaException("Failed to open file for reading: " + file.u8string());
    }

    std::vector<uint8_t> buffer(blockSize);
    uint64_t blockIndex = 0;
    uint64_t processed = 0;

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

        processed += bytesRead;
        if (progressCb) progressCb(processed, fileSize);
    }

    return signatures;
}

void computeDelta(const std::filesystem::path& newFile,
                  const std::vector<BlockSignature>& oldFileSignatures,
                  size_t blockSize,
                  std::function<void(DeltaInstruction)> onInstruction,
                  std::function<void(uint64_t processed, uint64_t total)> progressCb) {
    std::error_code ec;
    if (!std::filesystem::exists(newFile, ec) || ec) {
        throw PeerSyncDeltaException("File does not exist: " + newFile.u8string());
    }

    uint64_t fileSize = std::filesystem::file_size(newFile, ec);
    if (ec) {
        throw PeerSyncDeltaException("Failed to get file size: " + newFile.u8string());
    }

    if (fileSize == 0) {
        return;
    }

    std::ifstream ifs(newFile, std::ios::binary);
    if (!ifs.is_open()) {
        throw PeerSyncDeltaException("Failed to open file for reading: " + newFile.u8string());
    }

    struct Candidate {
        uint64_t blockIndex;
        uint64_t strongHash;
    };

    std::unordered_map<uint32_t, std::vector<Candidate>> signatureMap;
    for (const auto& sig : oldFileSignatures) {
        signatureMap[sig.weakChecksum].push_back({sig.blockIndex, sig.strongHash});
    }

    const size_t CHUNK_SIZE = std::max<size_t>(4 * 1024 * 1024, blockSize * 4); // Read in 4MB chunks
    std::vector<uint8_t> buffer;
    buffer.reserve(CHUNK_SIZE + blockSize);
    
    std::vector<uint8_t> readBuf(CHUNK_SIZE);
    ifs.read(reinterpret_cast<char*>(readBuf.data()), CHUNK_SIZE);
    std::streamsize bytesRead = ifs.gcount();
    buffer.insert(buffer.end(), readBuf.begin(), readBuf.begin() + bytesRead);

    size_t i = 0;
    std::vector<uint8_t> pendingLiteral;
    uint32_t currentWeak = 0;
    bool haveRollingChecksum = false;
    uint64_t lastReported = 0;
    size_t matchCount = 0;
    uint64_t totalProcessed = 0; // Total bytes completely processed (advanced past `i`)

    auto deltaStartTime = std::chrono::steady_clock::now();

    uint64_t rollingChecksumComputations = 0;
    bool fastPathTaken = false;

    if (signatureMap.empty()) {
        fastPathTaken = true;
        // Fast path for completely new files (no signatures to match against)
        while (i < buffer.size()) {
            if (buffer.size() - i <= blockSize && ifs.good() && !ifs.eof()) {
                buffer.erase(buffer.begin(), buffer.begin() + i);
                totalProcessed += i;
                i = 0;
                ifs.read(reinterpret_cast<char*>(readBuf.data()), CHUNK_SIZE);
                bytesRead = ifs.gcount();
                if (bytesRead > 0) {
                    buffer.insert(buffer.end(), readBuf.begin(), readBuf.begin() + bytesRead);
                }
            }
            
            size_t winLen = std::min(blockSize, buffer.size() - i);
            if (winLen == 0) break;
            
            std::vector<uint8_t> chunk(buffer.begin() + i, buffer.begin() + i + winLen);
            onInstruction(DeltaInstruction::Literal(std::move(chunk)));
            
            i += winLen;
            uint64_t currentAbsolutePos = totalProcessed + i;
            if (progressCb && (currentAbsolutePos - lastReported >= 1024 * 1024 || currentAbsolutePos == fileSize)) {
                progressCb(currentAbsolutePos, fileSize);
                lastReported = currentAbsolutePos;
            }
        }
    } else {
        // Standard rolling hash byte-by-byte delta logic
        while (i < buffer.size()) {
            if (buffer.size() - i <= blockSize && ifs.good() && !ifs.eof()) {
                buffer.erase(buffer.begin(), buffer.begin() + i);
                totalProcessed += i;
                i = 0;

                ifs.read(reinterpret_cast<char*>(readBuf.data()), CHUNK_SIZE);
                bytesRead = ifs.gcount();
                if (bytesRead > 0) {
                    buffer.insert(buffer.end(), readBuf.begin(), readBuf.begin() + bytesRead);
                }
            }

            size_t winLen = std::min(blockSize, buffer.size() - i);

            if (!haveRollingChecksum) {
                currentWeak = computeAdler32(&buffer[i], winLen);
                haveRollingChecksum = true;
            }

            bool matched = false;
            auto it = signatureMap.find(currentWeak);
            if (it != signatureMap.end()) {
                uint64_t strongHash = computeXxHash64(&buffer[i], winLen);
                for (const auto& candidate : it->second) {
                    if (candidate.strongHash == strongHash) {
                        if (!pendingLiteral.empty()) {
                            onInstruction(DeltaInstruction::Literal(std::move(pendingLiteral)));
                            pendingLiteral.clear();
                        }
                        onInstruction(DeltaInstruction::Copy(candidate.blockIndex));
                        i += winLen;
                        haveRollingChecksum = false;
                        matched = true;
                        matchCount++;
                        break;
                    }
                }
            }

            if (!matched) {
                pendingLiteral.push_back(buffer[i]);
                if (pendingLiteral.size() >= std::max<size_t>(65536, blockSize)) {
                    onInstruction(DeltaInstruction::Literal(std::move(pendingLiteral)));
                    pendingLiteral.clear();
                }
                if (buffer.size() - i > blockSize) {
                    currentWeak = rollAdler32(currentWeak, buffer[i], buffer[i + blockSize], blockSize);
                    rollingChecksumComputations++;
                    i++;
                } else {
                    i++;
                    haveRollingChecksum = false;
                }
            }

            uint64_t currentAbsolutePos = totalProcessed + i;
            if (progressCb && (currentAbsolutePos - lastReported >= 1024 * 1024 || currentAbsolutePos == fileSize)) {
                progressCb(currentAbsolutePos, fileSize);
                lastReported = currentAbsolutePos;
            }
        }
    }

    if (!pendingLiteral.empty()) {
        onInstruction(DeltaInstruction::Literal(std::move(pendingLiteral)));
    }
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
        throw PeerSyncDeltaException("Old file does not exist: " + oldFile.u8string());
    }

    uint64_t oldFileSize = std::filesystem::file_size(oldFile, ec);
    if (ec) {
        throw PeerSyncDeltaException("Failed to get old file size: " + oldFile.u8string());
    }

    std::filesystem::path parentDir = outputFile.parent_path();
    if (parentDir.empty()) {
        parentDir = ".";
    }
    if (!std::filesystem::exists(parentDir, ec)) {
        std::filesystem::create_directories(parentDir, ec);
        if (ec) {
            throw PeerSyncDeltaException("Failed to create parent directory for output file: " + outputFile.u8string());
        }
    }

    std::random_device rd;
    std::filesystem::path tempPath = parentDir / (outputFile.filename().u8string() + ".tmp." + std::to_string(rd()));

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
        throw PeerSyncDeltaException("Failed to open old file for reading: " + oldFile.u8string());
    }

    std::ofstream ofs(tempPath, std::ios::binary);
    if (!ofs.is_open()) {
        throw PeerSyncDeltaException("Failed to open temporary file for writing: " + tempPath.u8string());
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
