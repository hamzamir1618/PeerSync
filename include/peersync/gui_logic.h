#ifndef PEERSYNC_GUI_LOGIC_H
#define PEERSYNC_GUI_LOGIC_H

#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>

namespace peersync {

enum class TransferStatus {
    Completed,
    Interrupted,
    Resumed,
    Failed,
    InProgress
};

struct TransferHistoryEntry {
    std::string peerName;
    std::string peerIp;
    uint16_t peerPort{0};
    std::string path;
    bool isFolder{false};
    uint64_t totalBytes{0};
    uint64_t bytesTransferred{0};
    TransferStatus status{TransferStatus::InProgress};
    uint64_t timestampSec{0};
};

std::string formatElapsedTime(uint64_t elapsedSec);
std::string formatTransferStatusLabel(TransferStatus status);

const TransferHistoryEntry* findRelevantResumableSession(
    const std::vector<TransferHistoryEntry>& history,
    const std::string& peerName,
    const std::string& selectedPath
);

bool detectJournalForPath(
    const std::string& path,
    bool isFolder,
    uint64_t& outBytesApplied,
    uint64_t& outExpectedSize
);

} // namespace peersync

#endif // PEERSYNC_GUI_LOGIC_H
