#ifndef PEERSYNC_SYNC_ORCHESTRATOR_H
#define PEERSYNC_SYNC_ORCHESTRATOR_H

#include <filesystem>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include <peersync/protocol.h>
#include <peersync/socket.h>
#include <peersync/transfer.h>

namespace peersync {

struct SyncPolicy {
    enum class Direction {
        OneWayPush,       // Local pushes new/modified files to Remote
        OneWayPull,       // Local pulls new/modified files from Remote
        Bidirectional     // Last-write-wins (by mtime) bidirectional sync
    };
    Direction direction = Direction::Bidirectional;
    size_t maxConcurrency = 4; // Max concurrent TransferSessions
    TransferSession::Config transferConfig{};
    std::function<void(const std::string& relPath, size_t fileIndex, size_t totalFiles, bool isSending)> onFileStart = nullptr;
    std::function<void(const std::string& relPath, size_t fileIndex, size_t totalFiles, uint64_t bytesTransferred, uint64_t fileSize, bool isSending)> onFileComplete = nullptr;
};

struct SyncPlan {
    std::vector<FileEntry> toSend;
    std::vector<FileEntry> toReceive;
    std::vector<FileEntry> skipped;
    std::vector<FileEntry> conflicts; // Tracked when files differ on both sides
};

class SyncOrchestrator {
public:
    enum class Role {
        Initiator, // Client side: sends DirectoryManifestRequest and connects worker sockets
        Responder  // Server side: sends DirectoryManifestResponse and accepts worker sockets
    };

    SyncOrchestrator(TcpSocket& controlSocket, Role role, SyncPolicy policy = {});
    ~SyncOrchestrator() = default;

    // Builds a manifest of all files under localDir
    static std::vector<FileEntry> buildManifest(const std::filesystem::path& localDir);

    // Exchanges directory manifests over controlSocket and returns remote manifest
    std::vector<FileEntry> exchangeManifests(const std::vector<FileEntry>& localManifest);

    // Computes the sync actions (toSend, toReceive, skipped, conflicts) based on policy
    SyncPlan computeSyncPlan(const std::vector<FileEntry>& localManifest,
                             const std::vector<FileEntry>& remoteManifest) const;

    // Executes the sync plan using a worker socket pool bounded by maxConcurrency
    bool executeSync(const std::filesystem::path& localDir, const SyncPlan& plan);

    // All-in-one convenience: build manifest, exchange, compute plan, execute sync
    bool syncDirectory(const std::filesystem::path& localDir);

    // Statistics and testing verification
    size_t getMaxObservedConcurrency() const { return m_maxObservedConcurrency.load(); }
    size_t getFilesSentCount() const { return m_filesSent.load(); }
    size_t getFilesReceivedCount() const { return m_filesReceived.load(); }
    size_t getFilesSkippedCount() const { return m_filesSkipped.load(); }
    uint16_t getRemoteWorkerPort() const { return m_remoteWorkerPort; }

private:
    TcpSocket& m_controlSocket;
    Role m_role;
    SyncPolicy m_policy;
    uint16_t m_remoteWorkerPort{0};
    std::unique_ptr<TcpSocket> m_workerListener; // Used when m_role == Role::Responder

    std::atomic<size_t> m_activeTransfers{0};
    std::atomic<size_t> m_maxObservedConcurrency{0};
    std::atomic<size_t> m_filesSent{0};
    std::atomic<size_t> m_filesReceived{0};
    std::atomic<size_t> m_filesSkipped{0};

    void recordActiveTransferStart();
    void recordActiveTransferEnd();
};

} // namespace peersync

#endif // PEERSYNC_SYNC_ORCHESTRATOR_H
