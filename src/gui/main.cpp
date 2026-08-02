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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include "IconsFontAwesome6.h"
#include "fa_solid_900.h"
#include <GLFW/glfw3.h>

#include <peersync/discovery.h>
#include <fstream>
static void debug_log(const std::string& msg) {
    std::ofstream ofs("peersync_debug.log", std::ios::app);
    ofs << msg << "\n";
}
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

        bool inPreTransfer{false};
        std::string preTransferPhase;
        uint64_t preTransferProcessed{0};
        uint64_t preTransferTotal{0};

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

    void cancelAsync() {
        m_cancelRequested.store(true);
        std::lock_guard<std::mutex> lock(m_socketMutex);
        if (m_activeSocket) {
            m_activeSocket->close();
        }
    }

    bool isFinished() const {
        return m_isFinished.load();
    }

    void reset() {
        stop();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats = Stats{};
        m_cancelRequested.store(false);
        m_isFinished.store(true);
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
    std::atomic<bool> m_isFinished{true};
    std::mutex m_socketMutex;
    peersync::TcpSocket* m_activeSocket{nullptr};

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

    struct ActiveSocketGuard {
        TransferWorker& worker;
        ActiveSocketGuard(TransferWorker& w, peersync::TcpSocket* sock) : worker(w) {
            std::lock_guard<std::mutex> lock(worker.m_socketMutex);
            worker.m_activeSocket = sock;
        }
        ~ActiveSocketGuard() {
            std::lock_guard<std::mutex> lock(worker.m_socketMutex);
            worker.m_activeSocket = nullptr;
        }
    };

    struct FinishedGuard {
        TransferWorker& worker;
        FinishedGuard(TransferWorker& w) : worker(w) { worker.m_isFinished.store(false); }
        ~FinishedGuard() { worker.m_isFinished.store(true); }
    };

    void runInitiator(std::string ip, uint16_t port, std::string pin, bool isDir, std::string path, bool allowResume) {
        FinishedGuard fGuard(*this);
        try {
            updateState(State::Connecting, "Connecting to " + ip + ":" + std::to_string(port) + "...");
            peersync::TcpSocket client = peersync::TcpSocket::connect(ip, port);
            ActiveSocketGuard sGuard(*this, &client);

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
        FinishedGuard fGuard(*this);
        try {
            updateState(State::Connecting, "Binding listening socket on port " + std::to_string(port) + "...");
            peersync::TcpSocket server = peersync::TcpSocket::listen(port, "0.0.0.0");
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stats.boundPort = server.getBoundPort();
                m_stats.statusMessage = "Listening on port " + std::to_string(server.getBoundPort()) + "... Waiting for peer connection.";
            }

            peersync::TcpSocket client;
            {
                ActiveSocketGuard sGuard(*this, &server);
                while (!client.isValid() && !m_cancelRequested.load()) {
                    try {
                        client = server.accept();
                    } catch (...) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
            }
            if (!client.isValid() || m_cancelRequested.load()) {
                server.close();
                return;
            }
            server.close();

            ActiveSocketGuard sGuardClient(*this, &client);
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
        debug_log("executeTransfer start: " + path);
        if (!allowResume) {
            std::error_code ec;
            if (!isDir) {
                try {
                    std::filesystem::path p = std::filesystem::u8path(path);
                    std::filesystem::path j = p; j += ".peersync-journal";
                    std::filesystem::path t = p; t += ".peersync-tmp";
                    std::filesystem::remove(j, ec);
                    std::filesystem::remove(t, ec);
                } catch(const std::exception& e) {
                    debug_log(std::string("executeTransfer single file cleanup threw: ") + e.what());
                    throw;
                }
            } else {
                try {
                    std::filesystem::path p = std::filesystem::u8path(path);
                    if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
                        for (const auto& entry : std::filesystem::recursive_directory_iterator(p, ec)) {
                            if (ec) break;
                            std::string pStr = entry.path().u8string();
                            if (pStr.length() >= 17 && pStr.compare(pStr.length() - 17, 17, ".peersync-journal") == 0) {
                                std::filesystem::remove(entry.path(), ec);
                                std::filesystem::path tPath = std::filesystem::u8path(pStr.substr(0, pStr.length() - 17));
                                tPath += ".peersync-tmp";
                                std::filesystem::remove(tPath, ec);
                            }
                        }
                    }
                } catch(const std::exception& e) {
                    debug_log(std::string("executeTransfer folder cleanup threw: ") + e.what());
                    throw;
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
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stats.inPreTransfer = false;
                }
                updateProgress(bytesSent, totSize, "", 1, 1);
            };
            config.preTransferProgressCallback = [this](const std::string& phase, uint64_t processed, uint64_t total) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stats.inPreTransfer = true;
                m_stats.preTransferPhase = phase;
                m_stats.preTransferProcessed = processed;
                m_stats.preTransferTotal = total;
            };

            peersync::TransferSession session(socket, config);
            std::filesystem::path fsPath = std::filesystem::u8path(path);
            bool success = false;
            if (isSending) {
                uint64_t fSize = 0;
                std::error_code ec;
                if (std::filesystem::exists(fsPath, ec)) fSize = std::filesystem::file_size(fsPath, ec);
                updateProgress(0, fSize, fsPath.filename().u8string(), 1, 1);
                success = session.sendFile(fsPath, fsPath.filename().u8string());
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
                m_stats.inPreTransfer = false;
                m_stats.bytesTransferred = totalTransferred.load() + sent;
                if (m_stats.totalBytes > m_stats.bytesTransferred) {
                    m_stats.deltaSavingsBytes = m_stats.totalBytes - m_stats.bytesTransferred;
                }
            };
            policy.transferConfig.preTransferProgressCallback = [this](const std::string& phase, uint64_t processed, uint64_t total) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stats.inPreTransfer = true;
                m_stats.preTransferPhase = phase;
                m_stats.preTransferProcessed = processed;
                m_stats.preTransferTotal = total;
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
            bool success = orchestrator.syncDirectory(std::filesystem::u8path(path));
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
        ImFontConfig config;
        config.MergeMode = true;
        config.PixelSnapH = true;
        config.FontDataOwnedByAtlas = false;
        static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };

        m_fontRegular = io.Fonts->AddFontFromFileTTF("build/_deps/imgui-src/misc/fonts/Roboto-Medium.ttf", 16.0f);
        if (m_fontRegular) {
            io.Fonts->AddFontFromMemoryTTF((void*)fa_solid_900_ttf, fa_solid_900_ttf_len, 16.0f, &config, icon_ranges);
        } else {
            m_fontRegular = io.Fonts->AddFontDefault();
            io.Fonts->AddFontFromMemoryTTF((void*)fa_solid_900_ttf, fa_solid_900_ttf_len, 13.0f, &config, icon_ranges);
        }

        m_fontTitle = io.Fonts->AddFontFromFileTTF("build/_deps/imgui-src/misc/fonts/Roboto-Medium.ttf", 24.0f);
        if (m_fontTitle) {
            io.Fonts->AddFontFromMemoryTTF((void*)fa_solid_900_ttf, fa_solid_900_ttf_len, 24.0f, &config, icon_ranges);
        } else {
            m_fontTitle = m_fontRegular;
        }

        m_fontMetadata = io.Fonts->AddFontFromFileTTF("build/_deps/imgui-src/misc/fonts/Roboto-Medium.ttf", 14.0f);
        if (m_fontMetadata) {
            io.Fonts->AddFontFromMemoryTTF((void*)fa_solid_900_ttf, fa_solid_900_ttf_len, 14.0f, &config, icon_ranges);
        } else {
            m_fontMetadata = m_fontRegular;
        }
        bool fontRes = io.Fonts->Build();
        std::ostringstream fs;
        fs << "[INIT] Build Timestamp: " << __DATE__ << " " << __TIME__ << "\n";
        fs << "[INIT] Font Build Returned: " << (fontRes ? "true" : "false") << "\n";
        fs << "[INIT] Number of fonts loaded: " << io.Fonts->Fonts.Size << "\n";
        for (int i = 0; i < io.Fonts->Fonts.Size; i++) {
            ImFont* f = io.Fonts->Fonts[i];
            fs << "[INIT] Font " << i << ": " << (f->ConfigData ? f->ConfigData->Name : "Unknown") << ", Size: " << f->FontSize << "\n";
        }
        if (!fontRes || io.Fonts->Fonts.Size == 0) {
            fs << "[ERROR] Font loading failed silently!\n";
        } else {
            bool hasFA = false;
            for (int i = 0; i < io.Fonts->Fonts.Size; i++) {
                if (io.Fonts->Fonts[i]->ConfigData && strstr(io.Fonts->Fonts[i]->ConfigData->Name, "Font Awesome")) hasFA = true;
            }
            if (!hasFA) fs << "[ERROR] FontAwesome merge failed to register as a font layer!\n";
        }
        debug_log(fs.str());
        
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

            

            static bool screenshot_taken = false;
            static float accumTime = 0.0f;
            accumTime += ImGui::GetIO().DeltaTime;
            if (!screenshot_taken && accumTime > 1.5f) {
                screenshot_taken = true;
                int w, h;
                glfwGetFramebufferSize(m_window, &w, &h);
                unsigned char* pixels = new unsigned char[3 * w * h];
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels);
                unsigned char* flipped = new unsigned char[3 * w * h];
                for(int y = 0; y < h; y++) {
                    memcpy(flipped + (h - 1 - y) * w * 3, pixels + y * w * 3, w * 3);
                }
                stbi_write_png("verification_screenshot.png", w, h, 3, flipped, w * 3);
                delete[] pixels;
                delete[] flipped;
                glfwSwapBuffers(m_window);
            } else {
                glfwSwapBuffers(m_window);
            }
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
    ImFont* m_fontRegular = nullptr;
    ImFont* m_fontTitle = nullptr;
    ImFont* m_fontMetadata = nullptr;

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
    char m_manualIp[64]{""};
    int m_manualPort{0};

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


    peersync::AppScreen m_currentScreen{peersync::AppScreen::Discovery};

    void dispatchEvent(peersync::GuiEvent event) {
        m_currentScreen = peersync::transitionScreen(m_currentScreen, event);
    }

    ImVec2 calcButtonSize(const char* label, float minWidth = 100.0f) {
        ImVec2 size = ImGui::CalcTextSize(label);
        float width = size.x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float height = size.y + ImGui::GetStyle().FramePadding.y * 2.0f;
        if (width < minWidth) width = minWidth;
        return ImVec2(width, height);
    }

    void renderSidebar() {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.24f, 0.64f, 0.78f, 1.00f));
        ImGui::SetWindowFontScale(1.2f);
        if (m_fontTitle) ImGui::PushFont(m_fontTitle);
        ImGui::TextColored(ImVec4(0.24f, 0.64f, 0.78f, 1.0f), "%s PeerSync", ICON_FA_ARROWS_ROTATE);
        if (m_fontTitle) ImGui::PopFont();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::BeginChild("SidebarStepper", ImVec2(0, -80), ImGuiChildFlags_None);
        const char* steps[] = { "1. Discover", "2. Setup", "3. Transfer", "4. Complete" };
        peersync::AppScreen screens[] = { peersync::AppScreen::Discovery, peersync::AppScreen::Setup, peersync::AppScreen::Transferring, peersync::AppScreen::Complete };
        for (int i = 0; i < 4; ++i) {
            if (m_currentScreen == screens[i]) {
                ImGui::TextColored(ImVec4(0.38f, 0.82f, 0.50f, 1.0f), "%s %s", ICON_FA_ARROW_RIGHT, steps[i]);
            } else {
                if (m_fontMetadata) ImGui::PushFont(m_fontMetadata);
        ImGui::TextDisabled("  %s", steps[i]);
        if (m_fontMetadata) ImGui::PopFont();
            }
            ImGui::Spacing();
        }
        ImGui::EndChild();
        
        ImGui::Separator();
        std::string statusCopy;
        size_t peerCount = 0;
        {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            statusCopy = m_statusText;
            peerCount = m_cachedPeers.size();
        }
        if (m_fontMetadata) ImGui::PushFont(m_fontMetadata);
        ImGui::TextDisabled("Peers Found: %zu", peerCount);
        if (m_fontMetadata) ImGui::PopFont();
        ImGui::TextWrapped("%s", statusCopy.c_str());
    }

    void renderDiscoveryScreen() {
        if (m_fontTitle) ImGui::PushFont(m_fontTitle);
        ImGui::TextColored(ImVec4(0.24f, 0.64f, 0.78f, 1.0f), "Discovery & Recent");
        if (m_fontTitle) ImGui::PopFont();
        ImGui::Spacing();
        ImGui::Text("Find a peer to send files to, or wait to receive files.");
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_DOWNLOAD " Receive Files", calcButtonSize(ICON_FA_DOWNLOAD " Receive Files", 150.0f))) {
            m_initiatorMode = false;
            m_listenPort = 0;
            m_enteredPin[0] = '\0';
            m_selectedPath.clear();
            m_isFolderMode = false;
            m_worker.reset();
            dispatchEvent(peersync::GuiEvent::StartSetupResponder);
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PAPER_PLANE " Manual Send (IP)", calcButtonSize(ICON_FA_PAPER_PLANE " Manual Send (IP)", 150.0f))) {
            m_initiatorMode = true;
            m_selectedPeer = peersync::DiscoveredPeer();
            m_generatedPin = peersync::generatePin();
            m_enteredPin[0] = '\0';
            m_selectedPath.clear();
            m_isFolderMode = false;
            m_manualIp[0] = '\0';
            m_manualPort = 0;
            m_worker.reset();
            dispatchEvent(peersync::GuiEvent::StartSetupInitiator);
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Rescan Network", calcButtonSize(ICON_FA_ARROWS_ROTATE " Rescan Network", 150.0f))) {
            startDiscovery();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float tableHeight = ImGui::GetContentRegionAvail().y * 0.5f;
        ImGui::BeginChild("PeersTableChild", ImVec2(0, tableHeight), ImGuiChildFlags_Border);
        
        std::vector<peersync::DiscoveredPeer> peersCopy;
        {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            peersCopy = m_cachedPeers;
        }

        if (peersCopy.empty()) {
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.38f, 0.82f, 0.50f, 1.0f), "%s Searching for peers on your local network...", ICON_FA_SPINNER);
            ImGui::Spacing();
            if (m_fontMetadata) ImGui::PushFont(m_fontMetadata);
        ImGui::TextDisabled("Ensure the other device is open and connected to the same LAN.");
        if (m_fontMetadata) ImGui::PopFont();
        } else {
            ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("DiscoveredPeersTable", 4, tableFlags)) {
                ImGui::TableSetupColumn("Peer Name", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("IP Address", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < peersCopy.size(); ++i) {
                    const auto& peer = peersCopy[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(peer.instanceName.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", peer.instanceName.c_str());
                    
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(peer.ipAddress.c_str());
                    
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%u", static_cast<unsigned>(peer.port));
                    
                    ImGui::TableSetColumnIndex(3);
                    std::string btnLabel = "Send##" + std::to_string(i);
                    if (ImGui::Button(btnLabel.c_str(), ImVec2(-FLT_MIN, 0))) {
                        m_selectedPeer = peer;
                        m_initiatorMode = true;
                        m_generatedPin = peersync::generatePin();
                        m_enteredPin[0] = '\0';
                        m_selectedPath.clear();
                        m_isFolderMode = false;
                        m_worker.reset();
                        dispatchEvent(peersync::GuiEvent::StartSetupInitiator);
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Transfer History");
        ImGui::BeginChild("HistoryTableChild", ImVec2(0, 0), ImGuiChildFlags_Border);
        std::vector<peersync::TransferHistoryEntry> historyCopy;
        {
            std::lock_guard<std::mutex> lock(m_historyMutex);
            historyCopy = m_history;
        }
        if (historyCopy.empty()) {
            if (m_fontMetadata) ImGui::PushFont(m_fontMetadata);
        ImGui::TextDisabled("No transfers recorded in current session.");
        if (m_fontMetadata) ImGui::PopFont();
        } else {
            ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
            if (ImGui::BeginTable("HistoryTable", 5, flags)) {
                ImGui::TableSetupColumn("Peer");
                ImGui::TableSetupColumn("Path");
                ImGui::TableSetupColumn("Size");
                ImGui::TableSetupColumn("Progress");
                ImGui::TableSetupColumn("Status");
                ImGui::TableHeadersRow();

                for (int i = static_cast<int>(historyCopy.size()) - 1; i >= 0; --i) {
                    const auto& entry = historyCopy[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(entry.peerName.c_str());
                    
                    ImGui::TableSetColumnIndex(1);
                    std::filesystem::path p = std::filesystem::u8path(entry.path);
                    ImGui::TextUnformatted(p.filename().u8string().c_str());
                    
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(formatBytes(entry.totalBytes).c_str());
                    
                    ImGui::TableSetColumnIndex(3);
                    int pct = (entry.totalBytes > 0) ? static_cast<int>((entry.bytesTransferred * 100) / entry.totalBytes) : 0;
                    if (entry.status == peersync::TransferStatus::Completed) pct = 100;
                    ImGui::Text("%d%%", pct);
                    
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(peersync::formatTransferStatusLabel(entry.status).c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
    }

    void renderSetupScreen() {
        if (m_fontTitle) ImGui::PushFont(m_fontTitle);
        ImGui::TextColored(ImVec4(0.24f, 0.64f, 0.78f, 1.0f), "%s", m_initiatorMode ? "Setup: Send to Peer" : "Setup: Receive from Peer");
        if (m_fontTitle) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::BeginChild("PairingGroup", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
        if (m_initiatorMode) {
            if (m_selectedPeer.ipAddress.empty()) {
                ImGui::Text("Target Peer: (Manual Entry)");
                ImGui::InputText("IP Address", m_manualIp, sizeof(m_manualIp));
                ImGui::InputInt("Port (optional)", &m_manualPort);
            } else {
                ImGui::Text("Target Peer: %s", m_selectedPeer.instanceName.empty() ? m_selectedPeer.ipAddress.c_str() : m_selectedPeer.instanceName.c_str());
            }
            ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.2f, 1.0f), "Pairing PIN: %s", m_generatedPin.c_str());
            if (m_fontMetadata) ImGui::PushFont(m_fontMetadata);
        ImGui::TextDisabled("Enter this PIN on the receiving device when prompted.");
        if (m_fontMetadata) ImGui::PopFont();
        } else {
            ImGui::Text("Responder Mode: Waiting for Peer Connection");
            ImGui::InputInt("Listen Port (0 for auto)", &m_listenPort);
            ImGui::Text("Enter Pairing PIN from Initiator:");
            ImGui::InputText("##PinInput", m_enteredPin, sizeof(m_enteredPin), ImGuiInputTextFlags_CharsDecimal);
        }
        ImGui::EndChild();

        ImGui::Spacing();

        ImGui::BeginChild("FileGroup", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
        ImGui::Text("%s", m_initiatorMode ? "Select File/Folder to Send:" : "Select Destination Folder:");
        if (m_initiatorMode) {
            if (ImGui::Button(ICON_FA_FILE " Browse File...", calcButtonSize(ICON_FA_FILE " Browse File...", 140.0f))) {
                const char* res = tinyfd_openFileDialog("Select File to Send", "", 0, nullptr, nullptr, 0);
                if (res) { m_selectedPath = res; m_isFolderMode = false; }
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FOLDER_OPEN " Browse Folder...", calcButtonSize(ICON_FA_FOLDER_OPEN " Browse Folder...", 140.0f))) {
                const char* res = tinyfd_selectFolderDialog("Select Folder to Send", "");
                if (res) { m_selectedPath = res; m_isFolderMode = true; }
            }
        } else {
            if (ImGui::Button(ICON_FA_FOLDER_OPEN " Browse Accept Folder...", calcButtonSize(ICON_FA_FOLDER_OPEN " Browse Accept Folder...", 180.0f))) {
                const char* res = tinyfd_selectFolderDialog("Select Destination Folder", "");
                if (res) { m_selectedPath = res; }
            }
            ImGui::SameLine();
            ImGui::Checkbox("Expect Folder Sync", &m_isFolderMode);
        }
        if (m_fontMetadata) ImGui::PushFont(m_fontMetadata);
        ImGui::TextWrapped("Path: %s", m_selectedPath.empty() ? "(None selected)" : m_selectedPath.c_str());
        if (m_fontMetadata) ImGui::PopFont();
        ImGui::EndChild();

        ImGui::Spacing();

        float bottomBtnHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        float previewHeight = ImGui::GetContentRegionAvail().y - bottomBtnHeight;
        if (previewHeight < 100.0f) previewHeight = 100.0f;

        ImGui::BeginChild("PreviewGroup", ImVec2(0, previewHeight), ImGuiChildFlags_Border);
        
        float availY = ImGui::GetContentRegionAvail().y;
        float expectedY = ImGui::GetTextLineHeightWithSpacing() * 4.0f;
        if (availY > expectedY) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (availY - expectedY) / 2.0f);
        }

        if (m_fontTitle) ImGui::PushFont(m_fontTitle);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "TRANSFER PREVIEW");
        if (m_fontTitle) ImGui::PopFont();
        ImGui::Separator();
        
        if (m_initiatorMode) {
            std::string target = m_selectedPeer.ipAddress.empty() ? (m_manualIp[0] == '\0' ? "Unknown" : std::string(m_manualIp) + ":" + std::to_string(m_manualPort)) : m_selectedPeer.ipAddress;
            ImGui::Text("Destination: %s", target.c_str());
            ImGui::Text("Payload:     %s", m_selectedPath.empty() ? "(No file selected)" : std::filesystem::u8path(m_selectedPath).filename().u8string().c_str());
            ImGui::Text("Type:        %s", m_isFolderMode ? "Directory Sync" : "Single File");
        } else {
            ImGui::Text("Listening on Port: %s", m_listenPort == 0 ? "Auto" : std::to_string(m_listenPort).c_str());
            ImGui::Text("Expected PIN:      %s", m_enteredPin[0] == '\0' ? "(None entered)" : m_enteredPin);
            ImGui::Text("Save Location:     %s", m_selectedPath.empty() ? "(No folder selected)" : std::filesystem::u8path(m_selectedPath).filename().u8string().c_str());
        }

        ImGui::EndChild();
        
        ImGui::Spacing();
        bool canStart = false;
        if (m_initiatorMode) {
            bool hasTarget = !m_selectedPeer.ipAddress.empty() || m_manualIp[0] != '\0';
            canStart = !m_selectedPath.empty() && hasTarget;
        } else {
            canStart = !m_selectedPath.empty() && m_enteredPin[0] != '\0';
        }

        if (ImGui::Button(ICON_FA_XMARK " Cancel Setup", calcButtonSize(ICON_FA_XMARK " Cancel Setup", 120.0f))) {
            dispatchEvent(peersync::GuiEvent::CancelSetup);
        }

        std::string btnLabel;
        if (m_initiatorMode) {
            btnLabel = m_isFolderMode ? (ICON_FA_UPLOAD " Send Folder to Peer") : (ICON_FA_UPLOAD " Send File to Peer");
        } else {
            btnLabel = ICON_FA_DOWNLOAD " Listen & Accept";
        }

        ImVec2 startBtnSize = calcButtonSize(btnLabel.c_str(), 200.0f);
        float offsetPos = ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - startBtnSize.x;
        if (offsetPos > ImGui::GetCursorPosX()) ImGui::SameLine(offsetPos);
        else ImGui::Spacing();

        if (!canStart) ImGui::BeginDisabled();
        if (ImGui::Button(btnLabel.c_str(), startBtnSize)) {
            if (m_initiatorMode && m_selectedPeer.ipAddress.empty()) {
                m_selectedPeer.ipAddress = m_manualIp;
                m_selectedPeer.port = static_cast<uint16_t>(m_manualPort);
            }
            startTransferWithHistory(m_initiatorMode, true);
            dispatchEvent(peersync::GuiEvent::StartTransfer);
        }
        if (!canStart) ImGui::EndDisabled();
    }

    void renderTransferringScreen() {
        TransferWorker::Stats stats = m_worker.getStats();
        updateActiveHistoryFromStats(stats);
        
        if (m_fontTitle) ImGui::PushFont(m_fontTitle);
        ImGui::TextColored(ImVec4(0.24f, 0.64f, 0.78f, 1.0f), "Transfer in Progress...");
        if (m_fontTitle) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
        
        float bottomBtnHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        float contentHeight = ImGui::GetContentRegionAvail().y - bottomBtnHeight;
        if (contentHeight < 100.0f) contentHeight = 100.0f;

        ImGui::BeginChild("TransferContent", ImVec2(0, contentHeight), ImGuiChildFlags_Border);
        
        float availY = ImGui::GetContentRegionAvail().y;
        float expectedContentY = ImGui::GetTextLineHeightWithSpacing() * 6.0f;
        if (availY > expectedContentY) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (availY - expectedContentY) / 2.0f);
        }

        ImGui::Text("Status: %s", stats.statusMessage.c_str());
        if (stats.state == TransferWorker::State::Failed) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", stats.errorMessage.c_str());
        }
        
        ImGui::Spacing();

        if (stats.inPreTransfer) {
            float preProgress = stats.preTransferTotal > 0 ? (float)stats.preTransferProcessed / stats.preTransferTotal : 0.0f;
            ImGui::Text("Phase: %s", stats.preTransferPhase.c_str());
            ImGui::ProgressBar(preProgress, ImVec2(-1.0f, ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f));
            if (stats.preTransferTotal > 0) {
                ImGui::Text("%zu / %zu items processed", (size_t)stats.preTransferProcessed, (size_t)stats.preTransferTotal);
            }
        } else {
            float progress = 0.0f;
            if (stats.totalBytes > 0) progress = static_cast<float>(stats.bytesTransferred) / stats.totalBytes;
            
            ImGui::ProgressBar(progress, ImVec2(-1.0f, ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f));
            ImGui::Text("%s of %s", formatBytes(stats.bytesTransferred).c_str(), formatBytes(stats.totalBytes).c_str());
            if (!stats.currentFileName.empty()) {
                ImGui::Text("Current File: %s", stats.currentFileName.c_str());
            }
        }
        ImGui::EndChild();
        
        ImGui::Spacing();
        if (stats.state == TransferWorker::State::Completed || stats.state == TransferWorker::State::Failed) {
            if (ImGui::Button("Continue", calcButtonSize("Continue", 120.0f))) {
                if (stats.state == TransferWorker::State::Completed) dispatchEvent(peersync::GuiEvent::TransferFinished);
                else dispatchEvent(peersync::GuiEvent::ReturnToHome);
            }
        } else {
            if (ImGui::Button(ICON_FA_XMARK " Cancel Transfer", calcButtonSize(ICON_FA_XMARK " Cancel Transfer", 150.0f))) {
                m_worker.cancelAsync();
                dispatchEvent(peersync::GuiEvent::CancelTransfer);
            }
        }
    }

    void renderCancellingScreen() {
        if (m_fontTitle) ImGui::PushFont(m_fontTitle);
        ImGui::TextColored(ImVec4(0.24f, 0.64f, 0.78f, 1.0f), "Cancelling Transfer...");
        if (m_fontTitle) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::BeginChild("CancelContent", ImVec2(0, 0), ImGuiChildFlags_Border);
        float availY = ImGui::GetContentRegionAvail().y;
        float textY = ImGui::GetTextLineHeight();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (availY - textY) / 2.0f);
        
        const char* cancelText = "Cleaning up network sockets and stopping transfer thread...";
        float textX = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(cancelText).x) / 2.0f;
        if (textX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textX);
        ImGui::Text("%s", cancelText);
        ImGui::EndChild();

        if (m_worker.isFinished()) {
            dispatchEvent(peersync::GuiEvent::ReturnToHome);
        }
    }

    void renderCompleteScreen() {
        TransferWorker::Stats stats = m_worker.getStats();
        if (m_fontTitle) ImGui::PushFont(m_fontTitle);
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "%s Transfer Complete!", ICON_FA_CIRCLE_CHECK);
        if (m_fontTitle) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::BeginChild("SummaryBox", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
        ImGui::Text("Successfully transferred %s", formatBytes(stats.totalBytes).c_str());
        ImGui::Text("Total Files: %zu", stats.totalFiles);
        ImGui::EndChild();
        
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_HOUSE " Return to Home", calcButtonSize(ICON_FA_HOUSE " Return to Home", 150.0f))) {
            dispatchEvent(peersync::GuiEvent::ReturnToHome);
        }
    }

    void renderMainViewport() {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
        
        if (ImGui::Begin("PeerSync", nullptr, flags)) {
            ImGui::BeginChild("Sidebar", ImVec2(220, 0), ImGuiChildFlags_Border);
            renderSidebar();
            ImGui::EndChild();
            
            ImGui::SameLine();
            
            ImGui::BeginChild("Content", ImVec2(0, 0), ImGuiChildFlags_None);
            if (m_currentScreen == peersync::AppScreen::Discovery) {
                renderDiscoveryScreen();
            } else if (m_currentScreen == peersync::AppScreen::Setup) {
                renderSetupScreen();
            } else if (m_currentScreen == peersync::AppScreen::Transferring) {
                renderTransferringScreen();
            } else if (m_currentScreen == peersync::AppScreen::Cancelling) {
                renderCancellingScreen();
            } else if (m_currentScreen == peersync::AppScreen::Complete) {
                renderCompleteScreen();
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }
    void applyTheme() {
        debug_log("[INIT] applyTheme() is being called!");
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
#ifdef _WIN32
    extern int tinyfd_winUtf8;
    tinyfd_winUtf8 = 1;
#endif
    GuiApp app;
    if (!app.init()) {
        std::cerr << "Failed to initialize GUI application.\n";
        return 1;
    }
    app.run();
    return 0;
}
