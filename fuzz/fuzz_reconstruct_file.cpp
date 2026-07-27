#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <exception>
#include <peersync/delta.h>
#include <peersync/protocol.h>

namespace {
struct TempFileGuard {
    std::filesystem::path p1;
    std::filesystem::path p2;
    ~TempFileGuard() {
        std::error_code ec;
        std::filesystem::remove(p1, ec);
        std::filesystem::remove(p2, ec);
    }
};
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) {
        return 0;
    }

    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path oldFile = tempDir / "peersync_fuzz_old.tmp";
    std::filesystem::path newFile = tempDir / "peersync_fuzz_new.tmp";

    TempFileGuard guard{oldFile, newFile};

    // Use first byte to determine synthetic oldFile size (0 to 128 bytes)
    size_t oldSize = data[0] % 129;
    {
        std::ofstream ofs(oldFile, std::ios::binary | std::ios::trunc);
        if (oldSize > 0) {
            std::vector<uint8_t> oldData(oldSize, 0xAA);
            // Fill with some data from input if available
            for (size_t i = 0; i < oldSize && (1 + i) < size; ++i) {
                oldData[i] = data[1 + i];
            }
            ofs.write(reinterpret_cast<const char*>(oldData.data()), static_cast<std::streamsize>(oldSize));
        }
    }

    std::vector<uint8_t> payload(data, data + size);

    // 1. Try via protocol deserialization first
    try {
        auto msg = peersync::deserializeDeltaInstructionsMessage(payload);
        peersync::reconstructFile(oldFile, msg.instructions, newFile, msg.blockSize);
    } catch (const std::exception&) {}

    // 2. Direct instruction construction from remaining bytes
    try {
        std::vector<peersync::DeltaInstruction> instructions;
        size_t offset = 1;
        uint32_t blockSize = 64;
        if (offset + 4 <= size) {
            uint32_t rawBs = (static_cast<uint32_t>(data[offset]) << 24) |
                             (static_cast<uint32_t>(data[offset+1]) << 16) |
                             (static_cast<uint32_t>(data[offset+2]) << 8) |
                             static_cast<uint32_t>(data[offset+3]);
            if (rawBs != 0) {
                blockSize = rawBs % (1024 * 1024) + 1;
            }
            offset += 4;
        }

        while (offset < size) {
            uint8_t tag = data[offset++];
            if (tag % 3 == 0) { // Copy instruction with 8-byte index
                if (offset + 8 <= size) {
                    uint64_t idx = 0;
                    for (int i = 0; i < 8; ++i) {
                        idx = (idx << 8) | data[offset++];
                    }
                    instructions.push_back(peersync::DeltaInstruction::Copy(idx));
                } else {
                    break;
                }
            } else if (tag % 3 == 1) { // Copy instruction with extreme boundary index
                uint64_t idx = 0;
                if (offset < size) {
                    uint8_t sel = data[offset++] % 4;
                    if (sel == 0) idx = UINT64_MAX;
                    else if (sel == 1) idx = UINT64_MAX / blockSize + 1;
                    else if (sel == 2) idx = (oldSize > 0 && blockSize > 0) ? (oldSize / blockSize + 10) : 100;
                    else idx = 0;
                }
                instructions.push_back(peersync::DeltaInstruction::Copy(idx));
            } else { // Literal instruction
                size_t litLen = (offset < size) ? (data[offset++] % 64) : 0;
                if (offset + litLen <= size) {
                    std::vector<uint8_t> litData(data + offset, data + offset + litLen);
                    offset += litLen;
                    instructions.push_back(peersync::DeltaInstruction::Literal(std::move(litData)));
                } else {
                    break;
                }
            }
        }

        peersync::reconstructFile(oldFile, instructions, newFile, blockSize);
    } catch (const std::exception&) {}

    return 0;
}
