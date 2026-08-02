#include <peersync/gui_logic.h>
#include <fstream>
#include <system_error>
#include <algorithm>

namespace peersync {

std::string formatElapsedTime(uint64_t elapsedSec) {
    if (elapsedSec < 60) {
        return "< 1 min ago";
    } else if (elapsedSec < 3600) {
        uint64_t mins = elapsedSec / 60;
        return std::to_string(mins) + (mins == 1 ? " min ago" : " mins ago");
    } else if (elapsedSec < 86400) {
        uint64_t hours = elapsedSec / 3600;
        return std::to_string(hours) + (hours == 1 ? " hour ago" : " hours ago");
    } else {
        uint64_t days = elapsedSec / 86400;
        return std::to_string(days) + (days == 1 ? " day ago" : " days ago");
    }
}

std::string formatTransferStatusLabel(TransferStatus status) {
    switch (status) {
        case TransferStatus::Completed: return "Completed";
        case TransferStatus::Interrupted: return "Interrupted";
        case TransferStatus::Resumed: return "Resumed";
        case TransferStatus::Failed: return "Failed";
        case TransferStatus::InProgress: return "In Progress";
        default: return "Unknown";
    }
}

const TransferHistoryEntry* findRelevantResumableSession(
    const std::vector<TransferHistoryEntry>& history,
    const std::string& peerName,
    const std::string& selectedPath
) {
    const TransferHistoryEntry* best = nullptr;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->status != TransferStatus::Interrupted && it->status != TransferStatus::Failed) {
            continue;
        }
        if (!peerName.empty() && it->peerName != peerName && it->peerIp != peerName) {
            continue;
        }
        if (!selectedPath.empty() && it->path != selectedPath) {
            continue;
        }
        if (!best || it->timestampSec > best->timestampSec) {
            best = &(*it);
        }
    }
    return best;
}

static bool parseJournalFile(const std::filesystem::path& journalPath, uint64_t& bytesApplied, uint64_t& expectedSize) {
    std::error_code ec;
    if (!std::filesystem::exists(journalPath, ec) || ec) return false;
    std::ifstream ifs(journalPath);
    if (!ifs.is_open()) return false;
    std::string line;
    bytesApplied = 0;
    expectedSize = 0;
    bool foundPath = false;
    while (std::getline(ifs, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        if (key == "path") foundPath = !val.empty();
        else if (key == "expected_size") {
            try { expectedSize = std::stoull(val); } catch (...) {}
        }
        else if (key == "bytes_applied") {
            try { bytesApplied = std::stoull(val); } catch (...) {}
        }
    }
    return foundPath && bytesApplied > 0 && bytesApplied < expectedSize;
}

bool detectJournalForPath(
    const std::string& path,
    bool isFolder,
    uint64_t& outBytesApplied,
    uint64_t& outExpectedSize
) {
    outBytesApplied = 0;
    outExpectedSize = 0;
    if (path.empty()) return false;

    std::error_code ec;
    std::filesystem::path basePath = std::filesystem::u8path(path);

    try {
        if (!isFolder) {
            std::filesystem::path jPath = std::filesystem::u8path(path); jPath += ".peersync-journal";
            std::filesystem::path tPath = std::filesystem::u8path(path); tPath += ".peersync-tmp";
            if (std::filesystem::exists(jPath, ec) && std::filesystem::exists(tPath, ec)) {
                uint64_t ba = 0, es = 0;
                if (parseJournalFile(jPath, ba, es)) {
                    outBytesApplied = ba;
                    outExpectedSize = es;
                    return true;
                }
            }
        } else {
            if (!std::filesystem::is_directory(basePath, ec)) {
                return false;
            }
            uint64_t totalBa = 0;
            uint64_t totalEs = 0;
            bool foundAny = false;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(basePath, ec)) {
                if (ec) break;
                std::string pathStr = entry.path().u8string();
                std::string ext = entry.path().extension().u8string();
                if (entry.is_regular_file() && ext == ".peersync-journal") {
                    std::string baseStr = pathStr.substr(0, pathStr.length() - 17);
                    std::filesystem::path tPath = std::filesystem::u8path(baseStr); tPath += ".peersync-tmp";
                    if (std::filesystem::exists(tPath)) {
                        uint64_t ba = 0, es = 0;
                        if (parseJournalFile(entry.path(), ba, es)) {
                            totalBa += ba;
                            totalEs += es;
                            foundAny = true;
                        }
                    }
                }
            }
            if (foundAny) {
                outBytesApplied = totalBa;
                outExpectedSize = totalEs;
                return true;
            }
        }
    } catch (...) {
        return false;
    }
    return false;
}

AppScreen transitionScreen(AppScreen current, GuiEvent event) {
    switch (current) {
        case AppScreen::Discovery:
            if (event == GuiEvent::StartSetupInitiator || 
                event == GuiEvent::StartSetupResponder || 
                event == GuiEvent::ResumeFromHistory) {
                return AppScreen::Setup;
            }
            break;
        case AppScreen::Setup:
            if (event == GuiEvent::CancelSetup) {
                return AppScreen::Discovery;
            } else if (event == GuiEvent::StartTransfer) {
                return AppScreen::Transferring;
            }
            break;
        case AppScreen::Transferring:
            if (event == GuiEvent::CancelTransfer) {
                return AppScreen::Cancelling;
            } else if (event == GuiEvent::TransferFinished) {
                return AppScreen::Complete;
            } else if (event == GuiEvent::ReturnToHome) {
                return AppScreen::Discovery;
            }
            break;
        case AppScreen::Cancelling:
            if (event == GuiEvent::ReturnToHome) {
                return AppScreen::Discovery;
            }
            break;
        case AppScreen::Complete:
            if (event == GuiEvent::ReturnToHome) {
                return AppScreen::Discovery;
            }
            break;
    }
    return current;
}

SetupConfig getSetupFromHistory(const TransferHistoryEntry& entry) {
    SetupConfig config;
    config.isInitiator = !entry.peerIp.empty();
    config.targetIp = entry.peerIp;
    config.targetPort = entry.peerPort;
    config.path = entry.path;
    config.isFolder = entry.isFolder;
    return config;
}

} // namespace peersync
