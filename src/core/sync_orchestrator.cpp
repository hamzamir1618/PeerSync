#include <peersync/sync_orchestrator.h>
#include <peersync/exceptions.h>
#include <peersync/message_framing.h>
#include <algorithm>
#include <unordered_map>
#include <thread>
#include <chrono>

namespace peersync {

SyncOrchestrator::SyncOrchestrator(TcpSocket& controlSocket, Role role, SyncPolicy policy)
    : m_controlSocket(controlSocket), m_role(role), m_policy(std::move(policy)) {}

void SyncOrchestrator::recordActiveTransferStart() {
    size_t current = ++m_activeTransfers;
    size_t maxObs = m_maxObservedConcurrency.load();
    while (current > maxObs && !m_maxObservedConcurrency.compare_exchange_weak(maxObs, current)) {}
}

void SyncOrchestrator::recordActiveTransferEnd() {
    --m_activeTransfers;
}

std::vector<FileEntry> SyncOrchestrator::buildManifest(const std::filesystem::path& localDir) {
    std::vector<FileEntry> manifest;
    std::error_code ec;
    if (!std::filesystem::exists(localDir, ec) || !std::filesystem::is_directory(localDir, ec)) {
        return manifest;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(localDir, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (ec) break;
        const auto& entry = *it;
        if (entry.is_directory(ec)) continue;

        std::string filename = entry.path().filename().string();
        if (filename.find(".peersync-tmp") != std::string::npos ||
            filename.find(".peersync-journal") != std::string::npos) {
            continue;
        }

        auto relPath = std::filesystem::relative(entry.path(), localDir, ec);
        if (ec) continue;

        std::string relStr = relPath.generic_string(); // Forward slashes
        uint64_t size = entry.file_size(ec);
        if (ec) size = 0;

        uint64_t mtime = 0;
        try {
            auto ftime = entry.last_write_time(ec);
            if (!ec) {
                auto dur = ftime.time_since_epoch();
                mtime = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(dur).count());
            }
        } catch (...) {}

        manifest.push_back({relStr, size, "", mtime});
    }
    // Sort by relative path for deterministic ordering
    std::sort(manifest.begin(), manifest.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.relativePath < b.relativePath;
    });
    return manifest;
}

std::vector<FileEntry> SyncOrchestrator::exchangeManifests(const std::vector<FileEntry>& localManifest) {
    if (m_role == Role::Initiator) {
        DirectoryManifestRequestMessage req{localManifest};
        sendMessage(m_controlSocket, req);

        std::vector<uint8_t> respPayload = recvFramedMessage(m_controlSocket);
        if (getMessageType(respPayload) != MessageType::DirectoryManifestResponse) {
            throw PeerSyncProtocolException("Expected DirectoryManifestResponse");
        }
        auto resp = deserializeDirectoryManifestResponseMessage(respPayload);
        m_remoteWorkerPort = resp.workerPort;
        return resp.files;
    } else {
        std::vector<uint8_t> reqPayload = recvFramedMessage(m_controlSocket);
        if (getMessageType(reqPayload) != MessageType::DirectoryManifestRequest) {
            throw PeerSyncProtocolException("Expected DirectoryManifestRequest");
        }
        auto req = deserializeDirectoryManifestRequestMessage(reqPayload);

        uint16_t workerPort = 0;
        if (m_policy.maxConcurrency > 0) {
            try {
                m_workerListener = std::make_unique<TcpSocket>(TcpSocket::listen(0, "127.0.0.1"));
                workerPort = m_workerListener->getBoundPort();
            } catch (...) {
                workerPort = 0;
            }
        }

        DirectoryManifestResponseMessage resp{localManifest, workerPort};
        sendMessage(m_controlSocket, resp);
        return req.files;
    }
}

SyncPlan SyncOrchestrator::computeSyncPlan(const std::vector<FileEntry>& localManifest,
                                           const std::vector<FileEntry>& remoteManifest) const {
    SyncPlan plan;
    std::unordered_map<std::string, const FileEntry*> remoteMap;
    for (const auto& rem : remoteManifest) {
        remoteMap[rem.relativePath] = &rem;
    }

    for (const auto& loc : localManifest) {
        auto it = remoteMap.find(loc.relativePath);
        if (it == remoteMap.end()) {
            if (m_policy.direction == SyncPolicy::Direction::Bidirectional ||
                m_policy.direction == SyncPolicy::Direction::OneWayPush) {
                plan.toSend.push_back(loc);
            } else {
                plan.skipped.push_back(loc);
            }
        } else {
            const auto* rem = it->second;
            if (loc.fileSize == rem->fileSize && loc.lastModified == rem->lastModified) {
                plan.skipped.push_back(loc);
            } else {
                plan.conflicts.push_back(loc);
                if (loc.lastModified > rem->lastModified) {
                    if (m_policy.direction == SyncPolicy::Direction::Bidirectional ||
                        m_policy.direction == SyncPolicy::Direction::OneWayPush) {
                        plan.toSend.push_back(loc);
                    } else {
                        plan.skipped.push_back(loc);
                    }
                } else if (loc.lastModified < rem->lastModified) {
                    if (m_policy.direction == SyncPolicy::Direction::Bidirectional ||
                        m_policy.direction == SyncPolicy::Direction::OneWayPull) {
                        plan.toReceive.push_back(*rem);
                    } else {
                        plan.skipped.push_back(loc);
                    }
                } else {
                    // Equal mtime, different size: local wins in push/bidirectional
                    if (m_policy.direction == SyncPolicy::Direction::Bidirectional ||
                        m_policy.direction == SyncPolicy::Direction::OneWayPush) {
                        plan.toSend.push_back(loc);
                    } else if (m_policy.direction == SyncPolicy::Direction::OneWayPull) {
                        plan.toReceive.push_back(*rem);
                    } else {
                        plan.skipped.push_back(loc);
                    }
                }
            }
        }
    }

    std::unordered_map<std::string, const FileEntry*> localMap;
    for (const auto& loc : localManifest) {
        localMap[loc.relativePath] = &loc;
    }
    for (const auto& rem : remoteManifest) {
        if (localMap.find(rem.relativePath) == localMap.end()) {
            if (m_policy.direction == SyncPolicy::Direction::Bidirectional ||
                m_policy.direction == SyncPolicy::Direction::OneWayPull) {
                plan.toReceive.push_back(rem);
            } else {
                plan.skipped.push_back(rem);
            }
        }
    }

    std::sort(plan.toSend.begin(), plan.toSend.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.relativePath < b.relativePath;
    });
    std::sort(plan.toReceive.begin(), plan.toReceive.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.relativePath < b.relativePath;
    });
    std::sort(plan.skipped.begin(), plan.skipped.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.relativePath < b.relativePath;
    });

    return plan;
}

bool SyncOrchestrator::executeSync(const std::filesystem::path& localDir, const SyncPlan& plan) {
    m_filesSkipped += plan.skipped.size();

    struct TransferTask {
        std::string relativePath;
        bool isSending;
    };
    std::vector<TransferTask> tasks;
    for (const auto& f : plan.toSend) {
        tasks.push_back({f.relativePath, true});
    }
    for (const auto& f : plan.toReceive) {
        tasks.push_back({f.relativePath, false});
    }
    std::sort(tasks.begin(), tasks.end(), [](const TransferTask& a, const TransferTask& b) {
        return a.relativePath < b.relativePath;
    });

    if (tasks.empty()) {
        return true;
    }

    size_t K = std::min(m_policy.maxConcurrency, tasks.size());
    if (K == 0) K = 1;

    std::vector<std::unique_ptr<TcpSocket>> workerSockets;
    if (m_role == Role::Initiator && m_remoteWorkerPort > 0 && K > 0) {
        for (size_t i = 0; i < K; ++i) {
            try {
                workerSockets.push_back(std::make_unique<TcpSocket>(TcpSocket::connect("127.0.0.1", m_remoteWorkerPort)));
            } catch (...) {
                break;
            }
        }
    } else if (m_role == Role::Responder && m_workerListener && m_workerListener->isValid() && K > 0) {
        for (size_t i = 0; i < K; ++i) {
            try {
                workerSockets.push_back(std::make_unique<TcpSocket>(m_workerListener->accept()));
            } catch (...) {
                break;
            }
        }
    }

    // If worker socket pool failed or wasn't established for multi-concurrency, fall back to sequential over control socket
    if (workerSockets.size() < K) {
        workerSockets.clear();
        TransferSession session(m_controlSocket, m_policy.transferConfig);
        for (const auto& task : tasks) {
            recordActiveTransferStart();
            bool res = false;
            if (task.isSending) {
                res = session.sendFile(localDir / task.relativePath, task.relativePath);
                if (res) m_filesSent++;
            } else {
                res = session.receiveFile(localDir);
                if (res) m_filesReceived++;
            }
            recordActiveTransferEnd();
            if (!res) return false;
        }
        return true;
    }

    std::vector<std::thread> workers;
    std::atomic<bool> overallSuccess{true};

    for (size_t workerIdx = 0; workerIdx < workerSockets.size(); ++workerIdx) {
        workers.emplace_back([this, workerIdx, K = workerSockets.size(), &tasks, &workerSockets, &localDir, &overallSuccess]() {
            try {
                TransferSession session(*workerSockets[workerIdx], m_policy.transferConfig);
                for (size_t j = workerIdx; j < tasks.size(); j += K) {
                    if (!overallSuccess.load()) break;
                    const auto& task = tasks[j];
                    recordActiveTransferStart();
                    bool res = false;
                    if (task.isSending) {
                        res = session.sendFile(localDir / task.relativePath, task.relativePath);
                        if (res) m_filesSent++;
                    } else {
                        res = session.receiveFile(localDir);
                        if (res) m_filesReceived++;
                    }
                    recordActiveTransferEnd();
                    if (!res) {
                        overallSuccess = false;
                    }
                }
            } catch (...) {
                overallSuccess = false;
            }
        });
    }

    for (auto& th : workers) {
        if (th.joinable()) th.join();
    }

    return overallSuccess.load();
}

bool SyncOrchestrator::syncDirectory(const std::filesystem::path& localDir) {
    auto localManifest = buildManifest(localDir);
    auto remoteManifest = exchangeManifests(localManifest);
    auto plan = computeSyncPlan(localManifest, remoteManifest);
    return executeSync(localDir, plan);
}

} // namespace peersync
