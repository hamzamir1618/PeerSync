#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <memory>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <peersync/discovery.h>
#include <peersync/socket.h>
#include <peersync/message_framing.h>
#include <peersync/pairing.h>
#include <peersync/transfer.h>
#include <peersync/sync_orchestrator.h>
#include <peersync/gui_logic.h>
#include <tinyfiledialogs.h>

static std::string formatBytes(uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        oss << (double)bytes / (1024.0 * 1024.0 * 1024.0) << " GB";
    } else if (bytes >= 1024ULL * 1024ULL) {
        oss << (double)bytes / (1024.0 * 1024.0) << " MB";
    } else if (bytes >= 1024ULL) {
        oss << (double)bytes / 1024.0 << " KB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

class TransferWorker {
public:
    enum class State {
        Idle,
        Connecting,
        Pairing,
        Transferring,
        Completed,
        Failed
    };

    struct Stats {
        State state{State::Idle};
        std::string statusMessage{"Ready."};
        std::string errorMessage;
        uint64_t bytesTransferred{0};
        uint64_t totalBytes{0};
        uint64_t deltaSavingsBytes{0};
        size_t filesTransferred{0};
        size_t totalFiles{0};
        std::string currentFileName;
        bool isResuming{false};
        uint16_t boundPort{0};
    };

    TransferWorker() = default;
    ~TransferWorker() {
        stop();
    }

    void stop() {
        m_cancelRequested.store(true);
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void reset() {
        stop();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats = Stats{};
        m_cancelRequested.store(false);
    }

    Stats getStats() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats;
    }

    void startInitiator(const std::string& ip, uint16_t port, const std::string& pin, bool isDir, const std::string& path, bool allowResume = true) {
        reset();
        m_thread = std::thread(&TransferWorker::runInitiator, this, ip, port, pin, isDir, path, allowResume);
    }

    void startResponder(uint16_t port, const std::string& pin, bool isDir, const std::string& path, bool allowResume = true) {
        reset();
        m_thread = std::thread(&TransferWorker::runResponder, this, port, pin, isDir, path, allowResume);
    }

private:
    std::thread m_thread;
    mutable std::mutex m_mutex;
    Stats m_stats;
    std::atomic<bool> m_cancelRequested{false};

    void updateState(State state, const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.state = state;
        m_stats.statusMessage = msg;
    }

    void updateError(const std::string& err) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.state = State::Failed;
        m_stats.errorMessage = err;
        m_stats.statusMessage = "Error: " + err;
    }

    void updateProgress(uint64_t bytes, uint64_t total, const std::string& file, size_t idx, size_t count) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.bytesTransferred = bytes;
        m_stats.totalBytes = total;
        if (!file.empty()) m_stats.currentFileName = file;
        m_stats.filesTransferred = idx;
        m_stats.totalFiles = count;
        if (total > 0 && bytes < total) {
            m_stats.deltaSavingsBytes = total - bytes;
        } else {
            m_stats.deltaSavingsBytes = 0;
        }
    }

    void runInitiator(std::string ip, uint16_t port, std::string pin, bool isDir, std::string path, bool allowResume) {
        try {
            updateState(State::Connecting, "Connecting to " + ip + ":" + std::to_string(port) + "...");
            peersync::TcpSocket client = peersync::TcpSocket::connect(ip, port);

            updateState(State::Pairing, "Connected! Performing secure PIN pairing...");
            peersync::PairingSession pairing(peersync::PairingRole::Initiator, pin);
            pairing.start();

            while (!pairing.isFinished() && !m_cancelRequested.load()) {
                while (pairing.hasOutgoingMessage()) {
                    peersync::sendFramedMessage(client, pairing.popOutgoingMessage());
                }
                if (!pairing.isFinished()) {
                    auto incoming = peersync::recvFramedMessage(client);
                    pairing.processMessage(incoming);
                }
            }
            while (pairing.hasOutgoingMessage()) {
                peersync::sendFramedMessage(client, pairing.popOutgoingMessage());
            }
            if (!pairing.isAuthenticated()) {
                throw std::runtime_error("Pairing authentication failed: " + pairing.getErrorMessage());
            }
            if (m_cancelRequested.load()) {
                client.close();
                return;
            }

            updateState(State::Transferring, "Pairing successful! Starting transfer...");
            executeTransfer(client, peersync::SyncOrchestrator::Role::Initiator, isDir, path, true, allowResume);
        } catch (const std::exception& e) {
            if (!m_cancelRequested.load()) {
                updateError(e.what());
            }
        }
    }

    void runResponder(uint16_t port, std::string pin, bool isDir, std::string path, bool allowResume) {
        try {
            updateState(State::Connecting, "Binding listening socket on port " + std::to_string(port) + "...");
            peersync::TcpSocket server = peersync::TcpSocket::listen(port, "0.0.0.0");
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stats.boundPort = server.getBoundPort();
                m_stats.statusMessage = "Listening on port " + std::to_string(server.getBoundPort()) + "... Waiting for peer connection.";
            }

            peersync::TcpSocket client;
            while (!client.isValid() && !m_cancelRequested.load()) {
                try {
                    client = server.accept();
                } catch (...) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
            if (!client.isValid() || m_cancelRequested.load()) {
                server.close();
                return;
            }
            server.close();

            updateState(State::Pairing, "Client connected! Performing PIN authentication...");
            auto firstMsg = peersync::recvFramedMessage(client);
            if (peersync::getMessageType(firstMsg) != peersync::MessageType::PairChallenge) {
                throw std::runtime_error("Expected PairChallenge message from initiating peer.");
            }

            peersync::PairingSession pairing(peersync::PairingRole::Responder, pin);
            pairing.processMessage(firstMsg);
            while (!pairing.isFinished() && !m_cancelRequested.load()) {
                while (pairing.hasOutgoingMessage()) {
                    peersync::sendFramedMessage(client, pairing.popOutgoingMessage());
                }
                if (!pairing.isFinished()) {
                    auto incoming = peersync::recvFramedMessage(client);
                    pairing.processMessage(incoming);
                }
            }
            while (pairing.hasOutgoingMessage()) {
                peersync::sendFramedMessage(client, pairing.popOutgoingMessage());
            }
            if (!pairing.isAuthenticated()) {
                throw std::runtime_error("PIN mismatch or authentication failure: " + pairing.getErrorMessage());
            }
            if (m_cancelRequested.load()) {
                client.close();
                return;
            }

            updateState(State::Transferring, "Pairing successful! Receiving transfer...");
            executeTransfer(client, peersync::SyncOrchestrator::Role::Responder, isDir, path, false, allowResume);
        } catch (const std::exception& e) {
            if (!m_cancelRequested.load()) {
                updateError(e.what());
            }
        }
    }

    void executeTransfer(peersync::TcpSocket& socket, peersync::SyncOrchestrator::Role role, bool isDir, const std::string& path, bool isSending, bool allowResume) {
        if (!allowResume) {
            std::error_code ec;
            if (!isDir) {
                std::filesystem::remove(path + ".peersync-journal", ec);
                std::filesystem::remove(path + ".peersync-tmp", ec);
            } else {
                std::filesystem::path p(path);
                if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(p, ec)) {
                        if (ec) break;
                        std::string pStr = entry.path().string();
                        if (pStr.length() >= 17 && pStr.compare(pStr.length() - 17, 17, ".peersync-journal") == 0) {
                            std::filesystem::remove(entry.path(), ec);
                            std::string tStr = pStr.substr(0, pStr.length() - 17) + ".peersync-tmp";
                            std::filesystem::remove(tStr, ec);
                        }
                    }
                }
            }
        }

        if (!isDir) {
            peersync::TransferSession::Config config;
            config.isCancelled = [this]() { return m_cancelRequested.load(); };
            config.allowResume = allowResume;
            config.onResumeDetected = [this](const std::string& relPath, bool resuming, uint64_t resBytes, uint64_t totSize) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stats.isResuming = resuming;
                if (resuming) {
                    m_stats.statusMessage = "Resuming incomplete transfer for " + relPath + "...";
                }
            };
            config.progressCallback = [this](uint64_t bytesSent, uint64_t fileProcessed, uint64_t totSize) {
                updateProgress(bytesSent, totSize, "", 1, 1);
            };

            peersync::TransferSession session(socket, config);
            std::filesystem::path fsPath(path);
            bool success = false;
            if (isSending) {
                uint64_t fSize = 0;
                std::error_code ec;
                if (std::filesystem::exists(fsPath, ec)) fSize = std::filesystem::file_size(fsPath, ec);
                updateProgress(0, fSize, fsPath.filename().string(), 1, 1);
                success = session.sendFile(fsPath, fsPath.filename().string());
            } else {
                updateProgress(0, 0, "receiving file...", 1, 1);
                success = session.receiveFile(fsPath);
            }
            socket.close();
            if (!success) {
                throw std::runtime_error("Transfer protocol reported failure or was rejected by remote peer.");
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stats.totalBytes == 0) m_stats.totalBytes = session.getBytesSent() > 0 ? session.getBytesSent() : session.getBytesReceived();
                m_stats.bytesTransferred = isSending ? session.getBytesSent() : session.getBytesReceived();
                if (m_stats.totalBytes > m_stats.bytesTransferred) {
                    m_stats.deltaSavingsBytes = m_stats.totalBytes - m_stats.bytesTransferred;
                } else {
                    m_stats.deltaSavingsBytes = 0;
                }
            }
            updateState(State::Completed, "File transfer completed successfully!");
        } else {
            peersync::SyncPolicy policy;
            policy.isCancelled = [this]() { return m_cancelRequested.load(); };
            policy.direction = peersync::SyncPolicy::Direction::Bidirectional;
            policy.maxConcurrency = 2;
            policy.allowResume = allowResume;
            policy.onResumeDetected = [this](const std::string& relPath, bool resuming, uint64_t resBytes, uint64_t totSize) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (resuming) m_stats.isResuming = true;
            };

            std::atomic<uint64_t> totalTransferred{0};
            std::atomic<uint64_t> totalSize{0};

            policy.onFileStart = [this](const std::string& relPath, size_t idx, size_t total, bool sending) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stats.currentFileName = relPath;
                m_stats.filesTransferred = idx;
                m_stats.totalFiles = total;
            };

            policy.transferConfig.progressCallback = [this, &totalTransferred, &totalSize](uint64_t sent, uint64_t processed, uint64_t totFile) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stats.bytesTransferred = totalTransferred.load() + sent;
                if (m_stats.totalBytes > m_stats.bytesTransferred) {
                    m_stats.deltaSavingsBytes = m_stats.totalBytes - m_stats.bytesTransferred;
                }
            };

            policy.onFileComplete = [this, &totalTransferred, &totalSize](const std::string& relPath, size_t idx, size_t total, uint64_t transferred, uint64_t fSize, bool sending) {
                totalTransferred += transferred;
                totalSize += fSize;
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stats.bytesTransferred = totalTransferred.load();
                m_stats.totalBytes = totalSize.load();
                m_stats.filesTransferred = idx;
                if (m_stats.totalBytes > m_stats.bytesTransferred) {
                    m_stats.deltaSavingsBytes = m_stats.totalBytes - m_stats.bytesTransferred;
                }
            };

            peersync::SyncOrchestrator orchestrator(socket, role, policy);
            bool success = orchestrator.syncDirectory(std::filesystem::path(path));
            socket.close();
            if (!success) {
                throw std::runtime_error("Directory synchronization failed.");
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stats.filesTransferred = orchestrator.getFilesSentCount() + orchestrator.getFilesReceivedCount();
                m_stats.totalFiles = m_stats.filesTransferred + orchestrator.getFilesSkippedCount();
            }
            updateState(State::Completed, "Directory synchronization completed successfully!");
        }
    }
};

class GuiApp {
public:
    GuiApp() = default;
    ~GuiApp() {
        shutdown();
    }

    bool init() {
        glfwSetErrorCallback([](int error, const char* description) {
            std::cerr << "GLFW Error " << error << ": " << description << "\n";
        });
        if (!glfwInit()) {
            std::cerr << "Failed to initialize GLFW.\n";
            return false;
        }

#if defined(__APPLE__)
        const char* glsl_version = "#version 150";
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
        const char* glsl_version = "#version 130";
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

        m_window = glfwCreateWindow(960, 640, "peersync - Local Network Sync & Discovery", nullptr, nullptr);
        if (!m_window) {
            std::cerr << "Failed to create GLFW window.\n";
            glfwTerminate();
            return false;
        }
        glfwMakeContextCurrent(m_window);
        glfwSwapInterval(1); // Enable vsync

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        
        applyTheme();

        ImGui_ImplGlfw_InitForOpenGL(m_window, true);
        ImGui_ImplOpenGL3_Init(glsl_version);

        startDiscovery();

        m_initialized = true;
        return true;
    }

    void startDiscovery() {
        m_browser.stop();
        {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            m_cachedPeers.clear();
            m_statusText = "Scanning local network via mDNS...";
        }
        m_browser.clearPeers();
        m_browser.start([this](const peersync::DiscoveredPeer& peer) {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            m_cachedPeers = m_browser.getCurrentPeers();
            m_statusText = "Active scan complete. Listening for network advertisements...";
        });
        m_isScanning = true;
    }

    void run() {
        while (!glfwWindowShouldClose(m_window)) {
            glfwPollEvents();

            {
                std::lock_guard<std::mutex> lock(m_peersMutex);
                if (m_browser.isRunning()) {
                    auto current = m_browser.getCurrentPeers();
                    if (current.size() != m_cachedPeers.size() || !current.empty()) {
                        m_cachedPeers = std::move(current);
                    }
                }
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            renderMainViewport();

            ImGui::Render();
            int display_w, display_h;
            glfwGetFramebufferSize(m_window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);
            glClearColor(0.12f, 0.13f, 0.15f, 1.00f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(m_window);
        }
    }

    void shutdown() {
        if (m_initialized) {
            m_worker.stop();
            m_browser.stop();
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            if (m_window) {
                glfwDestroyWindow(m_window);
                m_window = nullptr;
            }
            glfwTerminate();
            m_initialized = false;
        }
    }

private:
    GLFWwindow* m_window{nullptr};
    bool m_initialized{false};

    peersync::PeerBrowser m_browser;
    TransferWorker m_worker;

    mutable std::mutex m_peersMutex;
    std::vector<peersync::DiscoveredPeer> m_cachedPeers;
    std::string m_statusText{"Ready."};
    std::atomic<bool> m_isScanning{false};

    bool m_showConnectPanel{false};
    bool m_initiatorMode{true};
    peersync::DiscoveredPeer m_selectedPeer;
    std::string m_generatedPin;
    char m_enteredPin[16]{""};
    std::string m_selectedPath;
    bool m_isFolderMode{false};
    int m_listenPort{0};

    mutable std::mutex m_historyMutex;
    std::vector<peersync::TransferHistoryEntry> m_history;
    size_t m_activeHistoryIndex{SIZE_MAX};

    void updateActiveHistoryFromStats(const TransferWorker::Stats& stats) {
        std::lock_guard<std::mutex> lock(m_historyMutex);
        if (m_activeHistoryIndex != SIZE_MAX && m_activeHistoryIndex < m_history.size()) {
            auto& entry = m_history[m_activeHistoryIndex];
            entry.bytesTransferred = stats.bytesTransferred;
            if (stats.totalBytes > 0) entry.totalBytes = stats.totalBytes;
            if (stats.state == TransferWorker::State::Completed) {
                entry.status = stats.isResuming ? peersync::TransferStatus::Resumed : peersync::TransferStatus::Completed;
            } else if (stats.state == TransferWorker::State::Failed) {
                if (stats.errorMessage.find("cancel") != std::string::npos || stats.statusMessage.find("cancel") != std::string::npos) {
                    entry.status = peersync::TransferStatus::Interrupted;
                } else {
                    entry.status = peersync::TransferStatus::Failed;
                }
            } else if (stats.state == TransferWorker::State::Idle) {
                if (entry.status == peersync::TransferStatus::InProgress) {
                    entry.status = peersync::TransferStatus::Interrupted;
                }
            }
        }
    }

    void startTransferWithHistory(bool isInitiator, bool allowResume) {
        {
            std::lock_guard<std::mutex> lock(m_historyMutex);
            peersync::TransferHistoryEntry entry;
            entry.peerName = m_selectedPeer.instanceName.empty() ? m_selectedPeer.ipAddress : m_selectedPeer.instanceName;
            entry.peerIp = m_selectedPeer.ipAddress;
            entry.peerPort = isInitiator ? m_selectedPeer.port : static_cast<uint16_t>(m_listenPort);
            entry.path = m_selectedPath;
            entry.isFolder = m_isFolderMode;
            entry.status = peersync::TransferStatus::InProgress;
            auto now = std::chrono::system_clock::now();
            entry.timestampSec = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
            m_history.push_back(entry);
            m_activeHistoryIndex = m_history.size() - 1;
        }
        if (isInitiator) {
            m_worker.startInitiator(m_selectedPeer.ipAddress, m_selectedPeer.port, m_generatedPin, m_isFolderMode, m_selectedPath, allowResume);
        } else {
            m_worker.startResponder(static_cast<uint16_t>(m_listenPort), m_enteredPin, m_isFolderMode, m_selectedPath, allowResume);
        }
    }

    void renderMainViewport() {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("MainWindow", nullptr, window_flags);
        ImGui::PopStyleVar(2);

        renderHeader();
        ImGui::Separator();
        ImGui::Spacing();

        renderPeersTable();

        ImGui::Spacing();
        ImGui::Separator();
        renderHistoryPanel();

        ImGui::Spacing();
        ImGui::Separator();
        renderStatusBar();

        if (m_showConnectPanel) {
            ImGui::OpenPopup("Connect & Transfer");
            renderConnectModal();
        }

        ImGui::End();
    }

    void renderHistoryPanel() {
        if (ImGui::CollapsingHeader("Transfer History (Session)", ImGuiTreeNodeFlags_DefaultOpen)) {
            std::vector<peersync::TransferHistoryEntry> historyCopy;
            {
                std::lock_guard<std::mutex> lock(m_historyMutex);
                historyCopy = m_history;
            }
            if (historyCopy.empty()) {
                ImGui::TextDisabled("No transfers recorded in current session.");
            } else {
                ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
                if (ImGui::BeginTable("HistoryTable", 6, flags, ImVec2(0, 100))) {
                    ImGui::TableSetupColumn("Peer", ImGuiTableColumnFlags_WidthStretch, 0.2f);
                    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, 65.0f);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 85.0f);
                    ImGui::TableSetupColumn("Time / Action", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                    ImGui::TableHeadersRow();

                    auto now = std::chrono::system_clock::now();
                    uint64_t currentSec = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());

                    for (int i = static_cast<int>(historyCopy.size()) - 1; i >= 0; --i) {
                        const auto& entry = historyCopy[i];
                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(entry.peerName.c_str());

                        ImGui::TableSetColumnIndex(1);
                        std::filesystem::path p(entry.path);
                        std::string fname = p.filename().string();
                        if (fname.empty()) fname = entry.path;
                        ImGui::TextUnformatted(fname.c_str());

                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(formatBytes(entry.totalBytes).c_str());

                        ImGui::TableSetColumnIndex(3);
                        int pct = (entry.totalBytes > 0) ? static_cast<int>((entry.bytesTransferred * 100) / entry.totalBytes) : (entry.status == peersync::TransferStatus::Completed ? 100 : 0);
                        if (pct > 100) pct = 100;
                        ImGui::Text("%d%%", pct);

                        ImGui::TableSetColumnIndex(4);
                        std::string statusLbl = peersync::formatTransferStatusLabel(entry.status);
                        if (entry.status == peersync::TransferStatus::Completed || entry.status == peersync::TransferStatus::Resumed) {
                            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "%s", statusLbl.c_str());
                        } else if (entry.status == peersync::TransferStatus::Interrupted) {
                            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", statusLbl.c_str());
                        } else if (entry.status == peersync::TransferStatus::Failed) {
                            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", statusLbl.c_str());
                        } else {
                            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", statusLbl.c_str());
                        }

                        ImGui::TableSetColumnIndex(5);
                        uint64_t elapsed = (currentSec >= entry.timestampSec) ? (currentSec - entry.timestampSec) : 0;
                        ImGui::TextUnformatted(peersync::formatElapsedTime(elapsed).c_str());

                        if (entry.status == peersync::TransferStatus::Interrupted || entry.status == peersync::TransferStatus::Failed) {
                            ImGui::SameLine();
                            ImGui::PushID(i);
                            if (ImGui::SmallButton("Resume")) {
                                peersync::DiscoveredPeer peer;
                                peer.instanceName = entry.peerName;
                                peer.ipAddress = entry.peerIp;
                                peer.port = entry.peerPort;
                                m_selectedPeer = peer;
                                m_selectedPath = entry.path;
                                m_isFolderMode = entry.isFolder;
                                m_initiatorMode = true;
                                m_generatedPin = peersync::generatePin();
                                m_enteredPin[0] = '\0';
                                m_showConnectPanel = true;
                                m_worker.reset();
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }
    }

    void renderHeader() {
        ImGui::TextColored(ImVec4(0.24f, 0.64f, 0.78f, 1.00f), "peersync");
        ImGui::SameLine();
        ImGui::TextDisabled("| Local Network File Synchronization & Discovery");

        float rightButtonsWidth = 320.0f;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - rightButtonsWidth);

        if (ImGui::Button("Receive / Accept", ImVec2(150, 0))) {
            m_showConnectPanel = true;
            m_initiatorMode = false;
            m_enteredPin[0] = '\0';
            m_selectedPath.clear();
            m_isFolderMode = false;
            m_worker.reset();
        }
        ImGui::SameLine();
        if (ImGui::Button("Rescan Network", ImVec2(150, 0))) {
            startDiscovery();
        }
    }

    void renderPeersTable() {
        float footerHeight = 160.0f;
        ImVec2 tableSize = ImVec2(0, ImGui::GetContentRegionAvail().y - footerHeight);
        
        ImGui::BeginChild("TableContainer", tableSize, true);

        std::vector<peersync::DiscoveredPeer> peersCopy;
        {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            peersCopy = m_cachedPeers;
        }

        if (peersCopy.empty()) {
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 280.0f) * 0.5f);
            ImGui::TextDisabled("No peers discovered yet on the local network.");
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 380.0f) * 0.5f);
            ImGui::TextDisabled("Start another instance with 'peersync listen' or click Rescan.");
        } else {
            ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                         ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY |
                                         ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                                         ImGuiTableFlags_PadOuterX;

            if (ImGui::BeginTable("DiscoveredPeersTable", 4, tableFlags)) {
                ImGui::TableSetupColumn("Peer Name", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("IP Address", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < peersCopy.size(); ++i) {
                    const auto& peer = peersCopy[i];
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(peer.instanceName.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(peer.ipAddress.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%u", static_cast<unsigned>(peer.port));

                    ImGui::TableSetColumnIndex(3);
                    std::string btnLabel = "Connect##" + std::to_string(i);
                    if (ImGui::Button(btnLabel.c_str(), ImVec2(-FLT_MIN, 0))) {
                        m_selectedPeer = peer;
                        m_showConnectPanel = true;
                        m_initiatorMode = true;
                        m_generatedPin = peersync::generatePin();
                        m_enteredPin[0] = '\0';
                        m_selectedPath.clear();
                        m_isFolderMode = false;
                        m_worker.reset();
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
    }

    void renderConnectModal() {
        ImGui::SetNextWindowSize(ImVec2(560, 440), ImGuiCond_Appearing);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
        if (ImGui::BeginPopupModal("Connect & Transfer", &m_showConnectPanel, flags)) {
            TransferWorker::Stats stats = m_worker.getStats();

            if (stats.state == TransferWorker::State::Idle) {
                if (ImGui::RadioButton("Initiator (Connect & Send/Sync)", m_initiatorMode)) {
                    m_initiatorMode = true;
                    if (m_generatedPin.empty()) m_generatedPin = peersync::generatePin();
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Responder (Listen & Receive)", !m_initiatorMode)) {
                    m_initiatorMode = false;
                }
                ImGui::Separator();
                ImGui::Spacing();

                if (m_initiatorMode) {
                    ImGui::Text("Target Peer: %s", m_selectedPeer.instanceName.c_str());
                    ImGui::TextDisabled("Address: %s : %u", m_selectedPeer.ipAddress.c_str(), (unsigned)m_selectedPeer.port);
                    ImGui::Spacing();

                    ImGui::Text("Secure Pairing PIN:");
                    ImGui::SetWindowFontScale(1.4f);
                    ImGui::TextColored(ImVec4(0.24f, 0.88f, 0.78f, 1.00f), "%s", m_generatedPin.c_str());
                    ImGui::SetWindowFontScale(1.0f);
                    ImGui::TextDisabled("Tell the person on the receiving device to enter this 6-digit PIN.");
                    ImGui::Spacing();

                    ImGui::Text("Select Data to Transfer:");
                    if (ImGui::Button("Browse File...", ImVec2(140, 32))) {
                        const char* res = tinyfd_openFileDialog("Select File to Send", "", 0, nullptr, nullptr, 0);
                        if (res) {
                            m_selectedPath = res;
                            m_isFolderMode = false;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Browse Folder...", ImVec2(140, 32))) {
                        const char* res = tinyfd_selectFolderDialog("Select Folder to Sync", "");
                        if (res) {
                            m_selectedPath = res;
                            m_isFolderMode = true;
                        }
                    }
                    ImGui::TextWrapped("Selected Path: %s", m_selectedPath.empty() ? "(None selected)" : m_selectedPath.c_str());
                    ImGui::Spacing();

                    uint64_t jApplied = 0, jExpected = 0;
                    bool diskFound = !m_selectedPath.empty() && peersync::detectJournalForPath(m_selectedPath, m_isFolderMode, jApplied, jExpected);
                    const peersync::TransferHistoryEntry* hist = nullptr;
                    if (!m_selectedPath.empty()) {
                        std::lock_guard<std::mutex> lock(m_historyMutex);
                        std::string pName = m_selectedPeer.instanceName.empty() ? m_selectedPeer.ipAddress : m_selectedPeer.instanceName;
                        hist = peersync::findRelevantResumableSession(m_history, pName, m_selectedPath);
                    }
                    bool canResume = diskFound || (hist != nullptr);
                    uint64_t resBytes = diskFound ? jApplied : (hist ? hist->bytesTransferred : 0);
                    uint64_t totBytes = diskFound ? jExpected : (hist ? hist->totalBytes : 0);

                    if (canResume) {
                        int pct = (totBytes > 0) ? static_cast<int>((resBytes * 100) / totBytes) : 0;
                        if (pct > 100) pct = 100;
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.22f, 0.28f, 1.0f));
                        ImGui::BeginChild("ResumeNoticeInit", ImVec2(0, 55), true);
                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "[ Incomplete Prior Transfer Detected ]");
                        if (totBytes > 0) {
                            ImGui::Text("Progress: %s of %s (%d%% complete)", formatBytes(resBytes).c_str(), formatBytes(totBytes).c_str(), pct);
                        } else {
                            ImGui::Text("Previous interrupted sync session found for this target.");
                        }
                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                    }

                    ImGui::Separator();
                    ImGui::Spacing();

                    bool canStart = !m_selectedPath.empty() && !m_selectedPeer.ipAddress.empty() && m_selectedPeer.port != 0;
                    if (!canStart) ImGui::BeginDisabled();
                    if (canResume) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.65f, 0.5f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.75f, 0.6f, 1.0f));
                        if (ImGui::Button("Resume Transfer (Fast)", ImVec2(200, 36))) {
                            startTransferWithHistory(true, true);
                        }
                        ImGui::PopStyleColor(2);
                        ImGui::SameLine();
                        if (ImGui::Button("Start Fresh (--no-resume)", ImVec2(200, 36))) {
                            startTransferWithHistory(true, false);
                        }
                    } else {
                        if (ImGui::Button("Start Connection & Transfer", ImVec2(240, 36))) {
                            startTransferWithHistory(true, true);
                        }
                    }
                    if (!canStart) ImGui::EndDisabled();
                } else {
                    ImGui::Text("Responder Mode: Waiting for Peer Connection");
                    ImGui::InputInt("Listen Port (0 for auto)", &m_listenPort);
                    if (m_listenPort < 0) m_listenPort = 0;
                    if (m_listenPort > 65535) m_listenPort = 65535;
                    ImGui::Spacing();

                    ImGui::Text("Enter Pairing PIN from Initiator:");
                    ImGui::InputText("##PinInput", m_enteredPin, sizeof(m_enteredPin), ImGuiInputTextFlags_CharsDecimal);
                    ImGui::TextDisabled("Enter the 6-digit PIN displayed on the connecting device.");
                    ImGui::Spacing();

                    ImGui::Text("Select Destination / Accept Folder:");
                    if (ImGui::Button("Browse Accept Folder...", ImVec2(180, 32))) {
                        const char* res = tinyfd_selectFolderDialog("Select Destination Folder", "");
                        if (res) {
                            m_selectedPath = res;
                            m_isFolderMode = true;
                        }
                    }
                    ImGui::SameLine();
                    ImGui::Checkbox("Folder Sync Mode", &m_isFolderMode);
                    ImGui::TextWrapped("Destination Path: %s", m_selectedPath.empty() ? "(None selected)" : m_selectedPath.c_str());
                    ImGui::Spacing();

                    uint64_t jApplied = 0, jExpected = 0;
                    bool diskFound = !m_selectedPath.empty() && peersync::detectJournalForPath(m_selectedPath, m_isFolderMode, jApplied, jExpected);
                    const peersync::TransferHistoryEntry* hist = nullptr;
                    if (!m_selectedPath.empty()) {
                        std::lock_guard<std::mutex> lock(m_historyMutex);
                        std::string pName = m_selectedPeer.instanceName.empty() ? m_selectedPeer.ipAddress : m_selectedPeer.instanceName;
                        hist = peersync::findRelevantResumableSession(m_history, pName, m_selectedPath);
                    }
                    bool canResume = diskFound || (hist != nullptr);
                    uint64_t resBytes = diskFound ? jApplied : (hist ? hist->bytesTransferred : 0);
                    uint64_t totBytes = diskFound ? jExpected : (hist ? hist->totalBytes : 0);

                    if (canResume) {
                        int pct = (totBytes > 0) ? static_cast<int>((resBytes * 100) / totBytes) : 0;
                        if (pct > 100) pct = 100;
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.22f, 0.28f, 1.0f));
                        ImGui::BeginChild("ResumeNoticeResp", ImVec2(0, 55), true);
                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "[ Incomplete Prior Transfer Detected ]");
                        if (totBytes > 0) {
                            ImGui::Text("Progress: %s of %s (%d%% complete)", formatBytes(resBytes).c_str(), formatBytes(totBytes).c_str(), pct);
                        } else {
                            ImGui::Text("Previous interrupted sync session found for this target.");
                        }
                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                    }

                    ImGui::Separator();
                    ImGui::Spacing();

                    bool canStart = !m_selectedPath.empty() && strlen(m_enteredPin) > 0;
                    if (!canStart) ImGui::BeginDisabled();
                    if (canResume) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.65f, 0.5f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.75f, 0.6f, 1.0f));
                        if (ImGui::Button("Resume Receive (Fast)", ImVec2(200, 36))) {
                            startTransferWithHistory(false, true);
                        }
                        ImGui::PopStyleColor(2);
                        ImGui::SameLine();
                        if (ImGui::Button("Receive Fresh (--no-resume)", ImVec2(200, 36))) {
                            startTransferWithHistory(false, false);
                        }
                    } else {
                        if (ImGui::Button("Start Listening & Pairing", ImVec2(240, 36))) {
                            startTransferWithHistory(false, true);
                        }
                    }
                    if (!canStart) ImGui::EndDisabled();
                }

                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 36))) {
                    ImGui::CloseCurrentPopup();
                    m_showConnectPanel = false;
                }
            } else {
                updateActiveHistoryFromStats(stats);

                std::string stateTitle = "Status: Working...";
                if (stats.state == TransferWorker::State::Connecting) stateTitle = "Status: Connecting...";
                else if (stats.state == TransferWorker::State::Pairing) stateTitle = "Status: Secure Pairing...";
                else if (stats.state == TransferWorker::State::Transferring) stateTitle = "Status: Transferring / Synchronizing...";
                else if (stats.state == TransferWorker::State::Completed) stateTitle = "Status: Transfer Completed!";
                else if (stats.state == TransferWorker::State::Failed) stateTitle = "Status: Transfer Failed";

                if (stats.state == TransferWorker::State::Completed) {
                    ImGui::TextColored(ImVec4(0.24f, 0.88f, 0.78f, 1.00f), "%s", stateTitle.c_str());
                } else if (stats.state == TransferWorker::State::Failed) {
                    ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.00f), "%s", stateTitle.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.24f, 0.64f, 0.78f, 1.00f), "%s", stateTitle.c_str());
                }
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextWrapped("%s", stats.statusMessage.c_str());
                ImGui::Spacing();

                if (stats.state == TransferWorker::State::Transferring) {
                    float progress = 0.0f;
                    if (stats.totalBytes > 0) {
                        progress = static_cast<float>(stats.bytesTransferred) / static_cast<float>(stats.totalBytes);
                    } else if (stats.totalFiles > 0) {
                        progress = static_cast<float>(stats.filesTransferred) / static_cast<float>(stats.totalFiles);
                    }
                    if (progress > 1.0f) progress = 1.0f;

                    ImGui::ProgressBar(progress, ImVec2(-1, 24));
                    ImGui::Spacing();

                    ImGui::Text("Transferred: %s of %s", formatBytes(stats.bytesTransferred).c_str(), formatBytes(stats.totalBytes).c_str());
                    if (!stats.currentFileName.empty()) {
                        ImGui::Text("Current File: [File %u/%u] %s", (unsigned)stats.filesTransferred, (unsigned)stats.totalFiles, stats.currentFileName.c_str());
                    }
                    double savedPct = (stats.totalBytes > 0) ? (100.0 * static_cast<double>(stats.deltaSavingsBytes) / static_cast<double>(stats.totalBytes)) : 0.0;
                    ImGui::TextColored(ImVec4(0.38f, 0.82f, 0.50f, 1.00f), "Delta Savings: %s (%.1f%% saved over network)", formatBytes(stats.deltaSavingsBytes).c_str(), savedPct);
                } else if (stats.state == TransferWorker::State::Completed) {
                    ImGui::BeginChild("SummaryBox", ImVec2(0, 140), true);
                    ImGui::TextColored(ImVec4(0.24f, 0.88f, 0.78f, 1.00f), "Final Transfer Summary:");
                    ImGui::Separator();
                    ImGui::Text("Total Bytes Transferred: %s", formatBytes(stats.bytesTransferred).c_str());
                    ImGui::Text("Total Source Size:       %s", formatBytes(stats.totalBytes).c_str());
                    double savedPct = (stats.totalBytes > 0) ? (100.0 * static_cast<double>(stats.deltaSavingsBytes) / static_cast<double>(stats.totalBytes)) : 0.0;
                    ImGui::TextColored(ImVec4(0.38f, 0.82f, 0.50f, 1.00f), "Bandwidth Saved:         %s (%.1f%%)", formatBytes(stats.deltaSavingsBytes).c_str(), savedPct);
                    ImGui::Text("Total Files Synced:      %u", (unsigned)stats.totalFiles);
                    ImGui::EndChild();
                } else if (stats.state == TransferWorker::State::Failed) {
                    ImGui::BeginChild("ErrorBox", ImVec2(0, 100), true);
                    ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.00f), "Error Details:");
                    ImGui::Separator();
                    ImGui::TextWrapped("%s", stats.errorMessage.c_str());
                    ImGui::EndChild();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (stats.state == TransferWorker::State::Completed || stats.state == TransferWorker::State::Failed) {
                    if (ImGui::Button("Close / Return to Peer List", ImVec2(240, 36))) {
                        updateActiveHistoryFromStats(stats);
                        m_activeHistoryIndex = SIZE_MAX;
                        m_worker.reset();
                        ImGui::CloseCurrentPopup();
                        m_showConnectPanel = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Start Another Transfer", ImVec2(200, 36))) {
                        updateActiveHistoryFromStats(stats);
                        m_activeHistoryIndex = SIZE_MAX;
                        m_worker.reset();
                    }
                } else {
                    if (ImGui::Button("Cancel Operation", ImVec2(180, 36))) {
                        {
                            std::lock_guard<std::mutex> lock(m_historyMutex);
                            if (m_activeHistoryIndex != SIZE_MAX && m_activeHistoryIndex < m_history.size()) {
                                m_history[m_activeHistoryIndex].status = peersync::TransferStatus::Interrupted;
                            }
                        }
                        m_activeHistoryIndex = SIZE_MAX;
                        m_worker.stop();
                    }
                }
            }

            ImGui::EndPopup();
        } else {
            m_showConnectPanel = false;
        }
    }

    void renderStatusBar() {
        std::string statusCopy;
        size_t peerCount = 0;
        {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            statusCopy = m_statusText;
            peerCount = m_cachedPeers.size();
        }

        ImGui::TextDisabled("Status:");
        ImGui::SameLine();
        ImGui::TextUnformatted(statusCopy.c_str());

        std::string countStr = "Discovered Peers: " + std::to_string(peerCount);
        float countWidth = ImGui::CalcTextSize(countStr.c_str()).x;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - countWidth - 8.0f);
        ImGui::TextColored(ImVec4(0.24f, 0.64f, 0.78f, 1.00f), "%s", countStr.c_str());
    }

    void applyTheme() {
        ImGuiStyle& style = ImGui::GetStyle();
        
        style.WindowPadding = ImVec2(16.0f, 16.0f);
        style.FramePadding = ImVec2(12.0f, 8.0f);
        style.CellPadding = ImVec2(12.0f, 8.0f);
        style.ItemSpacing = ImVec2(12.0f, 10.0f);
        style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
        style.ScrollbarSize = 14.0f;
        style.GrabMinSize = 12.0f;
        
        style.WindowRounding = 8.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 6.0f;
        style.PopupRounding = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 6.0f;
        
        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.PopupBorderSize = 1.0f;
        
        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text]                   = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
        colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);
        colors[ImGuiCol_WindowBg]               = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
        colors[ImGuiCol_ChildBg]                = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
        colors[ImGuiCol_PopupBg]                = ImVec4(0.15f, 0.16f, 0.19f, 0.98f);
        colors[ImGuiCol_Border]                 = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
        colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg]                = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.26f, 0.29f, 0.34f, 1.00f);
        colors[ImGuiCol_FrameBgActive]          = ImVec4(0.30f, 0.34f, 0.40f, 1.00f);
        colors[ImGuiCol_TitleBg]                = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
        colors[ImGuiCol_TitleBgActive]          = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
        colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
        colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.10f, 0.11f, 0.13f, 0.60f);
        colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.26f, 0.29f, 0.34f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.32f, 0.36f, 0.42f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.38f, 0.42f, 0.50f, 1.00f);
        colors[ImGuiCol_CheckMark]              = ImVec4(0.24f, 0.64f, 0.78f, 1.00f);
        colors[ImGuiCol_SliderGrab]             = ImVec4(0.24f, 0.64f, 0.78f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.31f, 0.73f, 0.88f, 1.00f);
        colors[ImGuiCol_Button]                 = ImVec4(0.20f, 0.50f, 0.62f, 1.00f);
        colors[ImGuiCol_ButtonHovered]          = ImVec4(0.25f, 0.60f, 0.74f, 1.00f);
        colors[ImGuiCol_ButtonActive]           = ImVec4(0.16f, 0.42f, 0.52f, 1.00f);
        colors[ImGuiCol_Header]                 = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
        colors[ImGuiCol_HeaderHovered]          = ImVec4(0.26f, 0.29f, 0.34f, 1.00f);
        colors[ImGuiCol_HeaderActive]           = ImVec4(0.30f, 0.34f, 0.40f, 1.00f);
        colors[ImGuiCol_Separator]              = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
        colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.24f, 0.64f, 0.78f, 1.00f);
        colors[ImGuiCol_SeparatorActive]        = ImVec4(0.31f, 0.73f, 0.88f, 1.00f);
        colors[ImGuiCol_ResizeGrip]             = ImVec4(0.24f, 0.64f, 0.78f, 0.25f);
        colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.24f, 0.64f, 0.78f, 0.67f);
        colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.24f, 0.64f, 0.78f, 0.95f);
        colors[ImGuiCol_Tab]                    = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
        colors[ImGuiCol_TabHovered]             = ImVec4(0.24f, 0.64f, 0.78f, 0.80f);
        colors[ImGuiCol_TabActive]              = ImVec4(0.20f, 0.50f, 0.62f, 1.00f);
        colors[ImGuiCol_TabUnfocused]           = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
        colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
        colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
        colors[ImGuiCol_TableBorderLight]       = ImVec4(0.20f, 0.22f, 0.25f, 1.00f);
        colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
        colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.24f, 0.64f, 0.78f, 0.35f);
        colors[ImGuiCol_DragDropTarget]         = ImVec4(0.95f, 0.80f, 0.00f, 0.90f);
        colors[ImGuiCol_NavHighlight]           = ImVec4(0.24f, 0.64f, 0.78f, 1.00f);
        colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
        colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
    }
};

int main(int argc, char* argv[]) {
    GuiApp app;
    if (!app.init()) {
        std::cerr << "Failed to initialize GUI application.\n";
        return 1;
    }
    app.run();
    return 0;
}
