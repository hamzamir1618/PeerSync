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

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <peersync/discovery.h>
#include <tinyfiledialogs.h>

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

        // Start background discovery thread on our long-lived PeerBrowser member
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

            // Periodic refresh of cached peers to sync timestamps or catch background updates
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

    // Long-lived background worker member
    peersync::PeerBrowser m_browser;

    // Mutex-protected cross-thread state
    mutable std::mutex m_peersMutex;
    std::vector<peersync::DiscoveredPeer> m_cachedPeers;
    std::string m_statusText{"Ready."};
    std::atomic<bool> m_isScanning{false};

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

        // Header Toolbar
        renderHeader();
        ImGui::Separator();
        ImGui::Spacing();

        // Main Table Content
        renderPeersTable();

        // Bottom Status Bar
        ImGui::Spacing();
        ImGui::Separator();
        renderStatusBar();

        ImGui::End();
    }

    void renderHeader() {
        ImGui::TextColored(ImVec4(0.24f, 0.64f, 0.78f, 1.00f), "peersync");
        ImGui::SameLine();
        ImGui::TextDisabled("| Local Network File Synchronization & Discovery");

        // Right-align the Rescan button
        float buttonWidth = 140.0f;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - buttonWidth);

        if (ImGui::Button("Rescan Network", ImVec2(buttonWidth, 0))) {
            startDiscovery();
        }
    }

    void renderPeersTable() {
        float footerHeight = 36.0f;
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
                        std::cout << "Selected peer: " << peer.instanceName << " (" << peer.ipAddress << ":" << peer.port << ")\n";
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
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
