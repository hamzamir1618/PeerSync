#include <peersync/transfer.h>
#include <peersync/exceptions.h>
#include <peersync/message_framing.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>
#include <random>
#include <xxhash.h>

namespace {

struct JournalEntry {
    std::string relativePath;
    uint64_t expectedSize = 0;
    std::string sigHash;
    uint64_t lastSeq = 0;
};

std::string computeSignatureListHash(const std::vector<peersync::BlockSignature>& sigs) {
    XXH64_state_t* state = XXH64_createState();
    if (!state) return "";
    XXH64_reset(state, 0);
    for (const auto& sig : sigs) {
        XXH64_update(state, &sig.weakChecksum, sizeof(sig.weakChecksum));
        XXH64_update(state, &sig.strongHash, sizeof(sig.strongHash));
        XXH64_update(state, &sig.blockIndex, sizeof(sig.blockIndex));
    }
    uint64_t hash = XXH64_digest(state);
    XXH64_freeState(state);
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return ss.str();
}

bool saveJournal(const std::filesystem::path& journalPath, const JournalEntry& entry) {
    std::error_code ec;
    std::filesystem::create_directories(journalPath.parent_path(), ec);
    std::ofstream ofs(journalPath, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs << "path=" << entry.relativePath << "\n";
    ofs << "expected_size=" << entry.expectedSize << "\n";
    ofs << "sig_hash=" << entry.sigHash << "\n";
    ofs << "last_seq=" << entry.lastSeq << "\n";
    return true;
}

bool loadJournal(const std::filesystem::path& journalPath, JournalEntry& entry) {
    std::error_code ec;
    if (!std::filesystem::exists(journalPath, ec) || ec) return false;
    std::ifstream ifs(journalPath);
    if (!ifs.is_open()) return false;
    std::string line;
    while (std::getline(ifs, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        if (key == "path") entry.relativePath = val;
        else if (key == "expected_size") entry.expectedSize = std::stoull(val);
        else if (key == "sig_hash") entry.sigHash = val;
        else if (key == "last_seq") entry.lastSeq = std::stoull(val);
    }
    return !entry.relativePath.empty();
}

void deleteJournalAndTemp(const std::filesystem::path& targetFile) {
    std::error_code ec;
    std::filesystem::remove(targetFile.string() + ".peersync-journal", ec);
    std::filesystem::remove(targetFile.string() + ".peersync-tmp", ec);
}

} // anonymous namespace

namespace peersync {

TransferSession::TransferSession(TcpSocket& socket, Config config)
    : m_socket(socket)
    , m_config(config)
    , m_bytesSent(0)
    , m_bytesReceived(0)
    , m_instructionsApplied(0)
{
    if (m_config.blockSize == 0) {
        m_config.blockSize = 1024;
    }
    if (m_config.maxInstructionsPerMessage == 0) {
        m_config.maxInstructionsPerMessage = 500;
    }
}

std::string TransferSession::computeFileHash(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        throw PeerSyncProtocolException("File does not exist for hashing: " + file.string());
    }
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs.is_open()) {
        throw PeerSyncProtocolException("Failed to open file for hashing: " + file.string());
    }
    XXH64_state_t* state = XXH64_createState();
    if (!state) {
        throw PeerSyncProtocolException("Failed to create XXH64 state");
    }
    XXH64_reset(state, 0);
    char buf[8192];
    while (ifs.read(buf, sizeof(buf))) {
        XXH64_update(state, buf, static_cast<size_t>(ifs.gcount()));
    }
    if (ifs.gcount() > 0) {
        XXH64_update(state, buf, static_cast<size_t>(ifs.gcount()));
    }
    uint64_t hash = XXH64_digest(state);
    XXH64_freeState(state);

    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return oss.str();
}

void TransferSession::sendMsg(const std::vector<uint8_t>& payload) {
    sendFramedMessage(m_socket, payload);
    m_bytesSent += payload.size() + 4;
}

std::vector<uint8_t> TransferSession::recvMsg() {
    auto payload = recvFramedMessage(m_socket);
    m_bytesReceived += payload.size() + 4;
    return payload;
}

bool TransferSession::sendFile(const std::filesystem::path& localFile, const std::string& relativePath) {
    std::error_code ec;
    if (!std::filesystem::exists(localFile, ec) || ec) {
        throw PeerSyncProtocolException("Local file does not exist for send: " + localFile.string());
    }
    uint64_t fileSize = std::filesystem::file_size(localFile, ec);
    if (ec) {
        throw PeerSyncProtocolException("Failed to get local file size: " + localFile.string());
    }
    std::string expectedHash = computeFileHash(localFile);

    // 1. Send ManifestRequest
    ManifestRequestMessage reqMsg{relativePath};
    sendMsg(serializeMessage(reqMsg));

    // 2. Receive ManifestResponse or ResumeRequest
    auto respPayload = recvMsg();
    MessageType respType = getMessageType(respPayload);
    std::vector<BlockSignature> peerSignatures;
    uint64_t resumeOffset = 0;
    bool isResuming = false;

    if (respType == MessageType::ResumeRequest) {
        auto resReq = deserializeResumeRequestMessage(respPayload);
        uint64_t expectedSize = UINT64_MAX;
        try {
            expectedSize = std::stoull(resReq.fileHash);
        } catch (...) {}

        if (fileSize == expectedSize) {
            ResumeResponseMessage resResp{relativePath, true, resReq.lastOffset};
            sendMsg(serializeMessage(resResp));
            peerSignatures = std::move(resReq.signatures);
            resumeOffset = resReq.lastOffset;
            isResuming = true;
        } else {
            ResumeResponseMessage resResp{relativePath, false, 0};
            sendMsg(serializeMessage(resResp));
            respPayload = recvMsg();
            respType = getMessageType(respPayload);
        }
    }

    if (respType != MessageType::ManifestResponse && !isResuming) {
        throw PeerSyncProtocolException("Expected ManifestResponse message");
    }
    if (respType == MessageType::ManifestResponse) {
        auto respMsg = deserializeManifestResponseMessage(respPayload);
        peerSignatures = std::move(respMsg.signatures);
    }

    // 3. Compute delta instructions against peer's signatures
    auto delta = computeDelta(localFile, peerSignatures, m_config.blockSize);

    // 4. Send instructions in chunks, extracting large literals as BlockData messages
    std::vector<DeltaInstruction> currentBatch;
    uint64_t blockDataSeq = 1; // Sequence 1-indexed for BlockData references
    uint64_t instIndex = 0;

    auto sendCurrentBatch = [&]() {
        DeltaInstructionsMessage deltaMsg{relativePath, fileSize, static_cast<uint32_t>(m_config.blockSize), currentBatch};
        sendMsg(serializeMessage(deltaMsg));
        currentBatch.clear();

        // Wait for TransferAck
        auto ackPayload = recvMsg();
        if (getMessageType(ackPayload) != MessageType::TransferAck) {
            throw PeerSyncProtocolException("Expected TransferAck message after sending instructions");
        }
        auto ackMsg = deserializeTransferAckMessage(ackPayload);
        (void)ackMsg; // ACK confirmed by receiver
    };

    for (const auto& inst : delta) {
        if (instIndex < resumeOffset) {
            if (inst.type == DeltaInstructionType::Literal && inst.bytes.size() > m_config.literalThreshold) {
                blockDataSeq++;
            }
            instIndex++;
            continue;
        }
        instIndex++;

        if (inst.type == DeltaInstructionType::Literal && inst.bytes.size() > m_config.literalThreshold) {
            // Send BlockData message first
            BlockDataMessage blockMsg{relativePath, blockDataSeq, inst.bytes};
            sendMsg(serializeMessage(blockMsg));

            // Reference this block in the instruction batch with empty bytes and blockIndex = blockDataSeq
            DeltaInstruction refInst;
            refInst.type = DeltaInstructionType::Literal;
            refInst.blockIndex = blockDataSeq++;
            refInst.bytes = {};
            currentBatch.push_back(std::move(refInst));
        } else {
            currentBatch.push_back(inst);
        }

        if (currentBatch.size() >= m_config.maxInstructionsPerMessage) {
            sendCurrentBatch();
        }
    }

    if (!currentBatch.empty() || (delta.empty() && !isResuming)) {
        sendCurrentBatch();
    }

    // 5. Receive TransferComplete from receiver
    auto compPayload = recvMsg();
    if (getMessageType(compPayload) != MessageType::TransferComplete) {
        throw PeerSyncProtocolException("Expected TransferComplete message");
    }
    auto compMsg = deserializeTransferCompleteMessage(compPayload);
    if (!compMsg.success || compMsg.finalHash != expectedHash) {
        TransferCompleteMessage failAck{relativePath, false, expectedHash};
        sendMsg(serializeMessage(failAck));
        return false;
    }

    // 6. Send final TransferComplete success acknowledgment
    TransferCompleteMessage successAck{relativePath, true, expectedHash};
    sendMsg(serializeMessage(successAck));

    m_finalHash = expectedHash;
    return true;
}

bool TransferSession::receiveFile(const std::filesystem::path& localDir) {
    // 1. Receive ManifestRequest
    auto reqPayload = recvMsg();
    if (getMessageType(reqPayload) != MessageType::ManifestRequest) {
        throw PeerSyncProtocolException("Expected ManifestRequest message");
    }
    auto reqMsg = deserializeManifestRequestMessage(reqPayload);
    std::string relativePath = reqMsg.path;
    std::filesystem::path targetFile = localDir / relativePath;

    std::error_code ec;
    std::vector<BlockSignature> sigs;
    uint64_t existingSize = 0;
    std::string existingHash;
    if (std::filesystem::exists(targetFile, ec) && !ec) {
        existingSize = std::filesystem::file_size(targetFile, ec);
        sigs = computeSignatures(targetFile, m_config.blockSize);
        existingHash = computeFileHash(targetFile);
    }

    std::filesystem::path tempPath = targetFile.string() + ".peersync-tmp";
    std::filesystem::path journalPath = targetFile.string() + ".peersync-journal";

    std::string currentSigHash = computeSignatureListHash(sigs);
    JournalEntry journal;
    bool isResuming = false;
    if (loadJournal(journalPath, journal) && std::filesystem::exists(tempPath, ec)) {
        if (journal.relativePath == relativePath && journal.sigHash == currentSigHash && std::filesystem::file_size(tempPath, ec) <= journal.expectedSize) {
            isResuming = true;
        } else {
            deleteJournalAndTemp(targetFile);
        }
    }

    // 2. Send ManifestResponse or ResumeRequest
    if (isResuming) {
        ResumeRequestMessage resumeReq;
        resumeReq.relativePath = relativePath;
        resumeReq.fileHash = std::to_string(journal.expectedSize);
        resumeReq.lastOffset = journal.lastSeq;
        resumeReq.signatures = sigs;
        sendMsg(serializeMessage(resumeReq));

        auto respPayload = recvMsg();
        if (getMessageType(respPayload) != MessageType::ResumeResponse) {
            throw PeerSyncProtocolException("Expected ResumeResponse message");
        }
        auto resumeResp = deserializeResumeResponseMessage(respPayload);
        if (!resumeResp.canResume) {
            isResuming = false;
            deleteJournalAndTemp(targetFile);
        }
    }

    if (!isResuming) {
        ManifestResponseMessage respMsg;
        FileEntry entry{relativePath, existingSize, existingHash, 0};
        respMsg.files.push_back(entry);
        respMsg.signatures = std::move(sigs);
        sendMsg(serializeMessage(respMsg));
    }

    // 3. Incremental receiving loop
    std::filesystem::path parentDir = targetFile.parent_path();
    if (parentDir.empty()) {
        parentDir = ".";
    }
    if (!std::filesystem::exists(parentDir, ec)) {
        std::filesystem::create_directories(parentDir, ec);
        if (ec) {
            throw PeerSyncProtocolException("Failed to create target directory: " + targetFile.string());
        }
    }

    struct TempFileGuard {
        std::filesystem::path path;
        std::filesystem::path jPath;
        bool commit = false;
        ~TempFileGuard() {
            if (!commit) {
                std::error_code ec_remove;
                if (!std::filesystem::exists(jPath, ec_remove)) {
                    std::filesystem::remove(path, ec_remove);
                }
            }
        }
    } guard{tempPath, journalPath, false};

    std::ifstream ifs;
    if (std::filesystem::exists(targetFile, ec) && !ec && existingSize > 0) {
        ifs.open(targetFile, std::ios::binary);
        if (!ifs.is_open()) {
            throw PeerSyncProtocolException("Failed to open existing local file for reading: " + targetFile.string());
        }
    }

    std::ofstream ofs;
    uint64_t totalBytesApplied = 0;
    uint64_t totalInstructionsApplied = 0;
    uint64_t expectedFileSize = UINT64_MAX;
    uint32_t currentBlockSize = static_cast<uint32_t>(m_config.blockSize);
    bool receivedAtLeastOneDeltaMsg = false;
    std::unordered_map<uint64_t, std::vector<uint8_t>> receivedBlockData;

    if (isResuming) {
        ofs.open(tempPath, std::ios::binary | std::ios::in | std::ios::out | std::ios::ate);
        if (!ofs.is_open()) {
            throw PeerSyncProtocolException("Failed to open temp file for resuming: " + tempPath.string());
        }
        totalBytesApplied = std::filesystem::file_size(tempPath, ec);
        totalInstructionsApplied = journal.lastSeq;
        expectedFileSize = journal.expectedSize;
        receivedAtLeastOneDeltaMsg = true;
    } else {
        deleteJournalAndTemp(targetFile);
        ofs.open(tempPath, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!ofs.is_open()) {
            throw PeerSyncProtocolException("Failed to open temp file for writing: " + tempPath.string());
        }
        journal.relativePath = relativePath;
        journal.sigHash = currentSigHash;
        journal.lastSeq = 0;
    }

    if (!isResuming || expectedFileSize == 0 || totalBytesApplied < expectedFileSize) {
        while (true) {
            auto payload = recvMsg();
            MessageType type = getMessageType(payload);

            if (type == MessageType::BlockData) {
                auto blockMsg = deserializeBlockDataMessage(payload);
                receivedBlockData[blockMsg.offset] = std::move(blockMsg.data);
                continue;
            }

            if (type == MessageType::DeltaInstructions) {
                auto deltaMsg = deserializeDeltaInstructionsMessage(payload);
                receivedAtLeastOneDeltaMsg = true;
                expectedFileSize = deltaMsg.targetFileSize;
                if (!isResuming) {
                    journal.expectedSize = expectedFileSize;
                    journal.lastSeq = 0;
                    saveJournal(journalPath, journal);
                }
                if (deltaMsg.blockSize > 0) {
                    currentBlockSize = deltaMsg.blockSize;
                }

                std::vector<uint8_t> readBuffer(currentBlockSize);
                for (const auto& inst : deltaMsg.instructions) {
                    if (inst.type == DeltaInstructionType::Copy) {
                        if (!ifs.is_open()) {
                            throw PeerSyncProtocolException("Copy instruction received but no local file open for copying");
                        }
                        uint64_t offset = inst.blockIndex * currentBlockSize;
                        if (offset >= existingSize && !(existingSize == 0 && offset == 0 && inst.blockIndex == 0)) {
                            throw PeerSyncProtocolException("Copy instruction block index out of bounds");
                        }
                        if (existingSize == 0) {
                            throw PeerSyncProtocolException("Cannot copy from empty existing file");
                        }
                        size_t bytesToRead = static_cast<size_t>(std::min(static_cast<uint64_t>(currentBlockSize), existingSize - offset));
                        ifs.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                        if (!ifs) {
                            throw PeerSyncProtocolException("Failed to seek in local file during copy");
                        }
                        ifs.read(reinterpret_cast<char*>(readBuffer.data()), static_cast<std::streamsize>(bytesToRead));
                        if (ifs.gcount() != static_cast<std::streamsize>(bytesToRead)) {
                            throw PeerSyncProtocolException("Failed to read expected bytes from local file");
                        }
                        ofs.write(reinterpret_cast<const char*>(readBuffer.data()), static_cast<std::streamsize>(bytesToRead));
                        if (!ofs) {
                            throw PeerSyncProtocolException("Failed to write copy block to temp file");
                        }
                        totalBytesApplied += bytesToRead;
                        totalInstructionsApplied++;
                    } else if (inst.type == DeltaInstructionType::Literal) {
                        if (!inst.bytes.empty()) {
                            ofs.write(reinterpret_cast<const char*>(inst.bytes.data()), static_cast<std::streamsize>(inst.bytes.size()));
                            if (!ofs) {
                                throw PeerSyncProtocolException("Failed to write inlined literal bytes to temp file");
                            }
                            totalBytesApplied += inst.bytes.size();
                            totalInstructionsApplied++;
                        } else {
                            auto it = receivedBlockData.find(inst.blockIndex);
                            if (it == receivedBlockData.end()) {
                                throw PeerSyncProtocolException("Missing BlockData for referenced literal index: " + std::to_string(inst.blockIndex));
                            }
                            ofs.write(reinterpret_cast<const char*>(it->second.data()), static_cast<std::streamsize>(it->second.size()));
                            if (!ofs) {
                                throw PeerSyncProtocolException("Failed to write referenced literal bytes to temp file");
                            }
                            totalBytesApplied += it->second.size();
                            totalInstructionsApplied++;
                            receivedBlockData.erase(it);
                        }
                    } else {
                        throw PeerSyncProtocolException("Unknown instruction type");
                    }
                }

                TransferAckMessage ackMsg{relativePath, totalBytesApplied};
                sendMsg(serializeMessage(ackMsg));

                journal.lastSeq = totalInstructionsApplied;
                saveJournal(journalPath, journal);

                if (receivedAtLeastOneDeltaMsg && totalBytesApplied == expectedFileSize) {
                    break;
                }
            } else if (type == MessageType::ErrorMessage) {
                auto errMsg = deserializeErrorMessageMessage(payload);
                throw PeerSyncProtocolException("Remote error: " + errMsg.errorMessage);
            } else {
                throw PeerSyncProtocolException("Unexpected message type during receive: " + std::to_string(static_cast<int>(type)));
            }
        }
    }

    if (ifs.is_open()) {
        ifs.close();
    }
    ofs.flush();
    ofs.close();
    if (!ofs) {
        throw PeerSyncProtocolException("Error flushing/closing temporary transfer file");
    }

    if (std::filesystem::file_size(tempPath, ec) != expectedFileSize) {
        throw PeerSyncProtocolException("Reconstructed file size mismatch");
    }

    std::error_code renameEc;
    std::filesystem::rename(tempPath, targetFile, renameEc);
    if (renameEc) {
        if (std::filesystem::exists(targetFile)) {
            std::error_code removeEc;
            std::filesystem::remove(targetFile, removeEc);
            if (!removeEc) {
                renameEc.clear();
                std::filesystem::rename(tempPath, targetFile, renameEc);
            }
        }
        if (renameEc) {
            throw PeerSyncProtocolException("Failed to rename temp file over target file: " + renameEc.message());
        }
    }

    guard.commit = true;
    std::filesystem::remove(journalPath, ec);

    std::string finalHash = computeFileHash(targetFile);
    TransferCompleteMessage compMsg{relativePath, true, finalHash};
    sendMsg(serializeMessage(compMsg));

    auto replyPayload = recvMsg();
    if (getMessageType(replyPayload) != MessageType::TransferComplete) {
        throw PeerSyncProtocolException("Expected TransferComplete reply from sender");
    }
    auto replyMsg = deserializeTransferCompleteMessage(replyPayload);
    if (!replyMsg.success || replyMsg.finalHash != finalHash) {
        return false;
    }

    m_instructionsApplied = totalInstructionsApplied;
    m_finalHash = finalHash;
    return true;
}

} // namespace peersync
