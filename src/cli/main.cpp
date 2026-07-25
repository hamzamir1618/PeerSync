#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <CLI/CLI.hpp>
#include <peersync/discovery.h>
#include <peersync/socket.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

static std::atomic<bool> g_shutdownRequested{false};

#ifdef _WIN32
BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        g_shutdownRequested = true;
        return TRUE;
    }
    return FALSE;
}
void setupSignalHandlers() {
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
}
#else
void signalHandler(int /*signum*/) {
    g_shutdownRequested = true;
}
void setupSignalHandlers() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
}
#endif

int runListen(const std::string& name, uint16_t port) {
    setupSignalHandlers();

    peersync::TcpSocket server;
    try {
        server = peersync::TcpSocket::listen(port, "0.0.0.0");
    } catch (const std::exception& e) {
        std::cerr << "Failed to bind listening socket on port " << port << ": " << e.what() << "\n";
        return 1;
    }

    uint16_t actualPort = server.getBoundPort();
    std::string deviceName = name;
    if (deviceName.empty()) {
        deviceName = peersync::discovery::getLocalHostname();
    }

    peersync::PeerAdvertiser advertiser(actualPort, deviceName);
    if (!advertiser.start()) {
        std::cerr << "Failed to start mDNS advertiser.\n";
        return 1;
    }

    std::cout << "Listening as " << deviceName << " on port " << actualPort << ", waiting for peers...\n";
    std::cout << std::flush;

    while (!g_shutdownRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nShutting down advertiser and listener...\n";
    advertiser.stop();
    server.close();
    return 0;
}

int runDiscover(int timeoutSeconds) {
    setupSignalHandlers();

    peersync::PeerBrowser browser;
    if (!browser.start()) {
        std::cerr << "Failed to start mDNS peer browser.\n";
        return 1;
    }

    std::cout << "Browsing for peers for " << timeoutSeconds << " seconds...\n";
    std::cout << std::flush;

    auto start = std::chrono::steady_clock::now();
    while (!g_shutdownRequested.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeoutSeconds) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    browser.stop();

    auto peers = browser.getCurrentPeers();
    std::cout << "\nDiscovered Peers:\n";
    std::cout << std::left << std::setw(32) << "Name" 
              << std::setw(20) << "IP Address" 
              << std::setw(10) << "Port" << "\n";
    std::cout << std::string(62, '-') << "\n";
    if (peers.empty()) {
        std::cout << "No peers discovered.\n";
    } else {
        for (const auto& peer : peers) {
            std::cout << std::left << std::setw(32) << peer.instanceName 
                      << std::setw(20) << peer.ipAddress 
                      << std::setw(10) << peer.port << "\n";
        }
    }
    std::cout << std::flush;
    return 0;
}

int main(int argc, char* argv[]) {
    CLI::App app{"peersync - local network file synchronization"};
    app.require_subcommand(1);

    auto* listenCmd = app.add_subcommand("listen", "Start listening for peer connections and advertise over mDNS");
    std::string listenName = "";
    uint16_t listenPort = 0;
    listenCmd->add_option("--name", listenName, "Device name to advertise (defaults to hostname)");
    listenCmd->add_option("--port", listenPort, "TCP port to listen on (defaults to 0 for ephemeral)");

    auto* discoverCmd = app.add_subcommand("discover", "Discover local network peers over mDNS");
    int discoverTimeout = 3;
    discoverCmd->add_option("--timeout", discoverTimeout, "Discovery timeout in seconds")->default_val(3);

    CLI11_PARSE(app, argc, argv);

    if (app.got_subcommand(listenCmd)) {
        return runListen(listenName, listenPort);
    } else if (app.got_subcommand(discoverCmd)) {
        return runDiscover(discoverTimeout);
    }

    return 0;
}
