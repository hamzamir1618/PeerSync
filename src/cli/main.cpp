#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <CLI/CLI.hpp>
#include <peersync/discovery.h>
#include <peersync/socket.h>
#include <peersync/pairing.h>
#include <peersync/transfer.h>
#include <peersync/message_framing.h>
#include <peersync/exceptions.h>
#include <peersync/sync_orchestrator.h>

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

int runSend(const std::string& filePath, const std::string& target, uint16_t port) {
    setupSignalHandlers();
    std::error_code ec;
    std::filesystem::path localFile(filePath);
    if (!std::filesystem::exists(localFile, ec) || ec) {
        std::cerr << "Error: File '" << filePath << "' does not exist.\n";
        return 1;
    }
    uint64_t fileSize = std::filesystem::file_size(localFile, ec);
    if (ec) {
        std::cerr << "Error: Could not get file size for '" << filePath << "'.\n";
        return 1;
    }

    std::string ip = target;
    uint16_t destPort = port;

    auto colonPos = target.find(':');
    if (colonPos != std::string::npos) {
        ip = target.substr(0, colonPos);
        try {
            if (destPort == 0) {
                destPort = static_cast<uint16_t>(std::stoul(target.substr(colonPos + 1)));
            }
        } catch (...) {}
    }

    bool isAddress = !ip.empty() && (std::isdigit(ip[0]) || ip == "localhost");
    if (ip == "localhost") ip = "127.0.0.1";

    if (!isAddress) {
        std::cout << "Looking up peer '" << target << "' via mDNS...\n" << std::flush;
        peersync::PeerBrowser browser;
        if (!browser.start()) {
            std::cerr << "Failed to start mDNS browser.\n";
            return 1;
        }
        auto start = std::chrono::steady_clock::now();
        bool found = false;
        while (!g_shutdownRequested.load()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= 3000) break;
            for (const auto& peer : browser.getCurrentPeers()) {
                if (peer.instanceName == target || peer.instanceName.find(target) != std::string::npos) {
                    ip = peer.ipAddress;
                    if (destPort == 0) destPort = peer.port;
                    found = true;
                    break;
                }
            }
            if (found) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        browser.stop();
        if (!found) {
            std::cerr << "Error: Could not find peer '" << target << "' on local network.\n";
            return 1;
        }
    }

    if (destPort == 0) {
        std::cerr << "Error: Destination port not specified and could not be determined.\n";
        return 1;
    }

    std::cout << "Connecting to " << ip << ":" << destPort << "...\n" << std::flush;
    peersync::TcpSocket client;
    try {
        client = peersync::TcpSocket::connect(ip, destPort);
    } catch (const std::exception& e) {
        std::cerr << "Failed to connect to peer: " << e.what() << "\n";
        return 1;
    }

    std::string pin = peersync::generatePin();
    std::cout << "Enter this PIN on the receiving device: " << pin << "\n" << std::flush;

    peersync::PairingSession pairing(peersync::PairingRole::Initiator, pin);
    pairing.start();
    while (!pairing.isFinished() && !g_shutdownRequested.load()) {
        while (pairing.hasOutgoingMessage()) {
            peersync::sendFramedMessage(client, pairing.popOutgoingMessage());
        }
        if (!pairing.isFinished()) {
            try {
                auto incoming = peersync::recvFramedMessage(client);
                pairing.processMessage(incoming);
            } catch (const std::exception& e) {
                std::cerr << "Pairing connection error: " << e.what() << "\n";
                client.close();
                return 1;
            }
        }
    }
    while (pairing.hasOutgoingMessage()) {
        peersync::sendFramedMessage(client, pairing.popOutgoingMessage());
    }
    if (!pairing.isAuthenticated()) {
        std::cerr << "Pairing failed: " << pairing.getErrorMessage() << "\n";
        client.close();
        return 1;
    }

    std::cout << "Pairing successful! Starting transfer of '" << localFile.filename().string() << "'...\n" << std::flush;

    peersync::TransferSession::Config config;
    config.progressCallback = [](uint64_t bytesSent, uint64_t fileBytesProcessed, uint64_t totalFileSize) {
        int percent = (totalFileSize > 0) ? static_cast<int>((fileBytesProcessed * 100) / totalFileSize) : 100;
        if (percent > 100) percent = 100;
        std::cout << "\rProgress: " << formatBytes(bytesSent) << " sent (" << percent << "% complete)   " << std::flush;
    };

    peersync::TransferSession session(client, config);
    bool success = false;
    try {
        success = session.sendFile(localFile, localFile.filename().string());
    } catch (const std::exception& e) {
        std::cerr << "\nTransfer error: " << e.what() << "\n";
        client.close();
        return 1;
    }
    client.close();

    if (!success) {
        std::cerr << "\nTransfer failed or rejected by peer.\n";
        return 1;
    }

    uint64_t totalSent = session.getBytesSent();
    double savedPercent = 0.0;
    if (fileSize > 0 && totalSent < fileSize) {
        savedPercent = 100.0 * static_cast<double>(fileSize - totalSent) / static_cast<double>(fileSize);
    }
    std::cout << "\nSent " << formatBytes(totalSent) << " of " << formatBytes(fileSize) << " file (" 
              << std::fixed << std::setprecision(1) << savedPercent << "% saved via delta sync)\n";
    return 0;
}

int runReceive(uint16_t port, const std::string& acceptDir) {
    setupSignalHandlers();
    std::error_code ec;
    std::filesystem::path dir(acceptDir);
    if (!std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::cerr << "Error: Could not create accept directory '" << acceptDir << "'.\n";
            return 1;
        }
    }

    peersync::TcpSocket server;
    try {
        server = peersync::TcpSocket::listen(port, "0.0.0.0");
    } catch (const std::exception& e) {
        std::cerr << "Failed to bind listening socket on port " << port << ": " << e.what() << "\n";
        return 1;
    }

    std::cout << "Listening for incoming file transfer on port " << server.getBoundPort() << "...\n" << std::flush;

    peersync::TcpSocket client;
    while (!client.isValid() && !g_shutdownRequested.load()) {
        try {
            client = server.accept();
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (!client.isValid() || g_shutdownRequested.load()) {
        server.close();
        return 0;
    }

    std::vector<uint8_t> firstMsg;
    try {
        firstMsg = peersync::recvFramedMessage(client);
    } catch (const std::exception& e) {
        std::cerr << "Error reading pairing challenge: " << e.what() << "\n";
        client.close();
        server.close();
        return 1;
    }

    if (peersync::getMessageType(firstMsg) != peersync::MessageType::PairChallenge) {
        std::cerr << "Error: Expected PairChallenge message, got type " << static_cast<int>(peersync::getMessageType(firstMsg)) << "\n";
        client.close();
        server.close();
        return 1;
    }

    std::cout << "Enter PIN: " << std::flush;
    std::string pin;
    if (!(std::cin >> pin)) {
        std::cerr << "\nError reading PIN from input.\n";
        client.close();
        server.close();
        return 1;
    }
    pin.erase(std::remove_if(pin.begin(), pin.end(), [](unsigned char c){ return std::isspace(c); }), pin.end());

    peersync::PairingSession pairing(peersync::PairingRole::Responder, pin);
    pairing.processMessage(firstMsg);
    while (!pairing.isFinished() && !g_shutdownRequested.load()) {
        while (pairing.hasOutgoingMessage()) {
            peersync::sendFramedMessage(client, pairing.popOutgoingMessage());
        }
        if (!pairing.isFinished()) {
            try {
                auto incoming = peersync::recvFramedMessage(client);
                pairing.processMessage(incoming);
            } catch (const std::exception& e) {
                std::cerr << "Pairing connection error: " << e.what() << "\n";
                client.close();
                server.close();
                return 1;
            }
        }
    }
    while (pairing.hasOutgoingMessage()) {
        peersync::sendFramedMessage(client, pairing.popOutgoingMessage());
    }
    if (!pairing.isAuthenticated()) {
        std::cerr << "Pairing failed: wrong PIN or authentication error.\n";
        client.close();
        server.close();
        return 1;
    }

    std::cout << "Pairing successful! Receiving file into '" << dir.string() << "'...\n" << std::flush;

    peersync::TransferSession session(client);
    bool success = false;
    try {
        success = session.receiveFile(dir);
    } catch (const std::exception& e) {
        std::cerr << "Receive transfer error: " << e.what() << "\n";
        client.close();
        server.close();
        return 1;
    }
    client.close();
    server.close();

    if (!success) {
        std::cerr << "Receive transfer failed.\n";
        return 1;
    }

    std::cout << "Transfer completed successfully! Saved file hash: " << session.getFinalHash() << "\n";
    return 0;
}

int runSync(const std::string& directory, const std::string& target, uint16_t port) {
    setupSignalHandlers();
    std::error_code ec;
    std::filesystem::path localDir(directory);
    if (!std::filesystem::exists(localDir, ec) || !std::filesystem::is_directory(localDir, ec)) {
        std::cerr << "Error: Directory '" << directory << "' does not exist or is not a directory.\n";
        return 1;
    }

    std::string ip = target;
    uint16_t destPort = port;
    auto colonPos = target.find(':');
    if (colonPos != std::string::npos && target.find_first_not_of("0123456789.:") == std::string::npos) {
        ip = target.substr(0, colonPos);
        try {
            destPort = static_cast<uint16_t>(std::stoi(target.substr(colonPos + 1)));
        } catch (...) {
            std::cerr << "Error: Invalid port in target address '" << target << "'.\n";
            return 1;
        }
    } else if (target.find_first_not_of("0123456789.") != std::string::npos || colonPos == std::string::npos) {
        std::cout << "Discovering peer '" << target << "' on local network...\n" << std::flush;
        peersync::PeerBrowser browser;
        bool found = false;
        browser.start([&](const peersync::DiscoveredPeer& p) {
            if (p.instanceName == target || p.instanceName.find(target) != std::string::npos) {
                ip = p.ipAddress;
                if (destPort == 0) destPort = p.port;
                found = true;
            }
        });
        auto start = std::chrono::steady_clock::now();
        while (!g_shutdownRequested.load()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= 3000) break;
            for (const auto& peer : browser.getCurrentPeers()) {
                if (peer.instanceName == target || peer.instanceName.find(target) != std::string::npos) {
                    ip = peer.ipAddress;
                    if (destPort == 0) destPort = peer.port;
                    found = true;
                    break;
                }
            }
            if (found) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        browser.stop();
        if (!found) {
            std::cerr << "Error: Could not find peer '" << target << "' on local network.\n";
            return 1;
        }
    }

    if (destPort == 0) {
        std::cerr << "Error: Destination port not specified and could not be determined.\n";
        return 1;
    }

    std::cout << "Connecting to " << ip << ":" << destPort << "...\n" << std::flush;
    peersync::TcpSocket client;
    try {
        client = peersync::TcpSocket::connect(ip, destPort);
    } catch (const std::exception& e) {
        std::cerr << "Failed to connect to peer: " << e.what() << "\n";
        return 1;
    }

    std::string pin = peersync::generatePin();
    std::cout << "Enter this PIN on the receiving device: " << pin << "\n" << std::flush;

    peersync::PairingSession pairing(peersync::PairingRole::Initiator, pin);
    pairing.start();
    while (!pairing.isFinished() && !g_shutdownRequested.load()) {
        while (pairing.hasOutgoingMessage()) {
            peersync::sendFramedMessage(client, pairing.popOutgoingMessage());
        }
        if (!pairing.isFinished()) {
            try {
                auto incoming = peersync::recvFramedMessage(client);
                pairing.processMessage(incoming);
            } catch (const std::exception& e) {
                std::cerr << "Pairing connection error: " << e.what() << "\n";
                client.close();
                return 1;
            }
        }
    }
    while (pairing.hasOutgoingMessage()) {
        peersync::sendFramedMessage(client, pairing.popOutgoingMessage());
    }
    if (!pairing.isAuthenticated()) {
        std::cerr << "Pairing failed: " << pairing.getErrorMessage() << "\n";
        client.close();
        return 1;
    }

    std::cout << "Pairing successful! Starting directory sync of '" << localDir.string() << "'...\n" << std::flush;

    peersync::SyncPolicy policy;
    policy.direction = peersync::SyncPolicy::Direction::Bidirectional;
    policy.maxConcurrency = 1;

    std::string currentFile;
    size_t currentIdx = 0;
    size_t totalCount = 0;
    uint64_t runningTotalTransferred = 0;
    uint64_t runningTotalSize = 0;

    policy.onFileStart = [&](const std::string& relPath, size_t fileIndex, size_t totalFiles, bool isSending) {
        currentFile = relPath;
        currentIdx = fileIndex;
        totalCount = totalFiles;
    };

    policy.transferConfig.progressCallback = [&](uint64_t bytesSent, uint64_t fileBytesProcessed, uint64_t totalFileSize) {
        int percent = (totalFileSize > 0) ? static_cast<int>((fileBytesProcessed * 100) / totalFileSize) : 100;
        if (percent > 100) percent = 100;
        std::cout << "\r[File " << currentIdx << "/" << totalCount << "] " << currentFile << ": "
                  << formatBytes(bytesSent) << " transferred (" << percent << "%)   " << std::flush;
    };

    policy.onFileComplete = [&](const std::string& relPath, size_t fileIndex, size_t totalFiles, uint64_t bytesTransferred, uint64_t fileSize, bool isSending) {
        runningTotalTransferred += bytesTransferred;
        runningTotalSize += fileSize;
        double fileSavedPct = 0.0;
        if (fileSize > 0 && bytesTransferred < fileSize) {
            fileSavedPct = 100.0 * static_cast<double>(fileSize - bytesTransferred) / static_cast<double>(fileSize);
        }
        double runningSavedPct = 0.0;
        if (runningTotalSize > 0 && runningTotalTransferred < runningTotalSize) {
            runningSavedPct = 100.0 * static_cast<double>(runningTotalSize - runningTotalTransferred) / static_cast<double>(runningTotalSize);
        }
        std::cout << "\r[File " << fileIndex << "/" << totalFiles << "] " << relPath << ": "
                  << "transferred " << formatBytes(bytesTransferred) << " of " << formatBytes(fileSize)
                  << " (" << std::fixed << std::setprecision(1) << fileSavedPct << "% saved) | "
                  << "Running total: " << formatBytes(runningTotalTransferred) << " of " << formatBytes(runningTotalSize)
                  << " (" << runningSavedPct << "% saved)                   \n" << std::flush;
    };

    peersync::SyncOrchestrator orchestrator(client, peersync::SyncOrchestrator::Role::Initiator, policy);
    bool success = false;
    try {
        success = orchestrator.syncDirectory(localDir);
    } catch (const std::exception& e) {
        std::cerr << "\nDirectory sync error: " << e.what() << "\n";
        client.close();
        return 1;
    }
    client.close();

    if (!success) {
        std::cerr << "\nDirectory sync failed or encountered errors.\n";
        return 1;
    }

    size_t totalSynced = orchestrator.getFilesSentCount() + orchestrator.getFilesReceivedCount() + orchestrator.getFilesSkippedCount();
    double totalSavedPct = 0.0;
    if (runningTotalSize > 0 && runningTotalTransferred < runningTotalSize) {
        totalSavedPct = 100.0 * static_cast<double>(runningTotalSize - runningTotalTransferred) / static_cast<double>(runningTotalSize);
    }
    std::cout << "\nDirectory sync complete!\n";
    std::cout << "Total files synced: " << totalSynced << "\n";
    std::cout << "Total bytes transferred: " << formatBytes(runningTotalTransferred) << " of " << formatBytes(runningTotalSize)
              << " (" << std::fixed << std::setprecision(1) << totalSavedPct << "% saved via delta sync)\n";
    return 0;
}

int runReceiveDir(uint16_t port, const std::string& acceptDir) {
    setupSignalHandlers();
    std::error_code ec;
    std::filesystem::path dir(acceptDir);
    if (!std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::cerr << "Error: Could not create accept directory '" << acceptDir << "'.\n";
            return 1;
        }
    }

    peersync::TcpSocket server;
    try {
        server = peersync::TcpSocket::listen(port, "0.0.0.0");
    } catch (const std::exception& e) {
        std::cerr << "Failed to bind listening socket on port " << port << ": " << e.what() << "\n";
        return 1;
    }

    std::cout << "Listening for incoming directory sync on port " << server.getBoundPort() << "...\n" << std::flush;

    peersync::TcpSocket client;
    while (!client.isValid() && !g_shutdownRequested.load()) {
        try {
            client = server.accept();
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (!client.isValid() || g_shutdownRequested.load()) {
        server.close();
        return 0;
    }

    std::vector<uint8_t> firstMsg;
    try {
        firstMsg = peersync::recvFramedMessage(client);
    } catch (const std::exception& e) {
        std::cerr << "Error reading pairing challenge: " << e.what() << "\n";
        client.close();
        server.close();
        return 1;
    }

    if (peersync::getMessageType(firstMsg) != peersync::MessageType::PairChallenge) {
        std::cerr << "Error: Expected PairChallenge message, got type " << static_cast<int>(peersync::getMessageType(firstMsg)) << "\n";
        client.close();
        server.close();
        return 1;
    }

    std::cout << "Enter PIN: " << std::flush;
    std::string pin;
    if (!(std::cin >> pin)) {
        std::cerr << "\nError reading PIN from input.\n";
        client.close();
        server.close();
        return 1;
    }
    pin.erase(std::remove_if(pin.begin(), pin.end(), [](unsigned char c){ return std::isspace(c); }), pin.end());

    peersync::PairingSession pairing(peersync::PairingRole::Responder, pin);
    pairing.processMessage(firstMsg);
    while (!pairing.isFinished() && !g_shutdownRequested.load()) {
        while (pairing.hasOutgoingMessage()) {
            peersync::sendFramedMessage(client, pairing.popOutgoingMessage());
        }
        if (!pairing.isFinished()) {
            try {
                auto incoming = peersync::recvFramedMessage(client);
                pairing.processMessage(incoming);
            } catch (const std::exception& e) {
                std::cerr << "Pairing connection error: " << e.what() << "\n";
                client.close();
                server.close();
                return 1;
            }
        }
    }
    while (pairing.hasOutgoingMessage()) {
        peersync::sendFramedMessage(client, pairing.popOutgoingMessage());
    }
    if (!pairing.isAuthenticated()) {
        std::cerr << "Pairing failed: wrong PIN or authentication error.\n";
        client.close();
        server.close();
        return 1;
    }

    std::cout << "Pairing successful! Starting directory sync into '" << dir.string() << "'...\n" << std::flush;

    peersync::SyncPolicy policy;
    policy.direction = peersync::SyncPolicy::Direction::Bidirectional;
    policy.maxConcurrency = 1;

    std::string currentFile;
    size_t currentIdx = 0;
    size_t totalCount = 0;
    uint64_t runningTotalTransferred = 0;
    uint64_t runningTotalSize = 0;

    policy.onFileStart = [&](const std::string& relPath, size_t fileIndex, size_t totalFiles, bool isSending) {
        currentFile = relPath;
        currentIdx = fileIndex;
        totalCount = totalFiles;
    };

    policy.transferConfig.progressCallback = [&](uint64_t bytesSent, uint64_t fileBytesProcessed, uint64_t totalFileSize) {
        int percent = (totalFileSize > 0) ? static_cast<int>((fileBytesProcessed * 100) / totalFileSize) : 100;
        if (percent > 100) percent = 100;
        std::cout << "\r[File " << currentIdx << "/" << totalCount << "] " << currentFile << ": "
                  << formatBytes(bytesSent) << " transferred (" << percent << "%)   " << std::flush;
    };

    policy.onFileComplete = [&](const std::string& relPath, size_t fileIndex, size_t totalFiles, uint64_t bytesTransferred, uint64_t fileSize, bool isSending) {
        runningTotalTransferred += bytesTransferred;
        runningTotalSize += fileSize;
        double fileSavedPct = 0.0;
        if (fileSize > 0 && bytesTransferred < fileSize) {
            fileSavedPct = 100.0 * static_cast<double>(fileSize - bytesTransferred) / static_cast<double>(fileSize);
        }
        double runningSavedPct = 0.0;
        if (runningTotalSize > 0 && runningTotalTransferred < runningTotalSize) {
            runningSavedPct = 100.0 * static_cast<double>(runningTotalSize - runningTotalTransferred) / static_cast<double>(runningTotalSize);
        }
        std::cout << "\r[File " << fileIndex << "/" << totalFiles << "] " << relPath << ": "
                  << "transferred " << formatBytes(bytesTransferred) << " of " << formatBytes(fileSize)
                  << " (" << std::fixed << std::setprecision(1) << fileSavedPct << "% saved) | "
                  << "Running total: " << formatBytes(runningTotalTransferred) << " of " << formatBytes(runningTotalSize)
                  << " (" << runningSavedPct << "% saved)                   \n" << std::flush;
    };

    peersync::SyncOrchestrator orchestrator(client, peersync::SyncOrchestrator::Role::Responder, policy);
    bool success = false;
    try {
        success = orchestrator.syncDirectory(dir);
    } catch (const std::exception& e) {
        std::cerr << "Receive directory sync error: " << e.what() << "\n";
        client.close();
        server.close();
        return 1;
    }
    client.close();
    server.close();

    if (!success) {
        std::cerr << "Receive directory sync failed.\n";
        return 1;
    }

    size_t totalSynced = orchestrator.getFilesSentCount() + orchestrator.getFilesReceivedCount() + orchestrator.getFilesSkippedCount();
    double totalSavedPct = 0.0;
    if (runningTotalSize > 0 && runningTotalTransferred < runningTotalSize) {
        totalSavedPct = 100.0 * static_cast<double>(runningTotalSize - runningTotalTransferred) / static_cast<double>(runningTotalSize);
    }
    std::cout << "\nDirectory sync complete!\n";
    std::cout << "Total files synced: " << totalSynced << "\n";
    std::cout << "Total bytes transferred: " << formatBytes(runningTotalTransferred) << " of " << formatBytes(runningTotalSize)
              << " (" << std::fixed << std::setprecision(1) << totalSavedPct << "% saved via delta sync)\n";
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

    auto* sendCmd = app.add_subcommand("send", "Send a file to a local network peer");
    std::string sendFile = "";
    std::string sendTo = "";
    uint16_t sendPort = 0;
    sendCmd->add_option("file", sendFile, "Path to the file to send")->required();
    sendCmd->add_option("--to", sendTo, "Peer name or IP address (or IP:port)")->required();
    sendCmd->add_option("--port", sendPort, "TCP port of destination peer");

    auto* receiveCmd = app.add_subcommand("receive", "Listen for and receive a file from a peer");
    uint16_t receivePort = 0;
    std::string receiveDir = ".";
    receiveCmd->add_option("--port", receivePort, "TCP port to listen on (defaults to 0 for ephemeral)");
    receiveCmd->add_option("--accept-dir", receiveDir, "Directory to save received file")->default_val(".");

    auto* syncCmd = app.add_subcommand("sync", "Synchronize a directory with a local network peer");
    std::string syncDir = "";
    std::string syncTo = "";
    uint16_t syncPort = 0;
    syncCmd->add_option("directory", syncDir, "Path to the directory to sync")->required();
    syncCmd->add_option("--to", syncTo, "Peer name or IP address (or IP:port)")->required();
    syncCmd->add_option("--port", syncPort, "TCP port of destination peer");

    auto* receiveDirCmd = app.add_subcommand("receive-dir", "Listen for and participate in a directory sync session");
    uint16_t receiveDirPort = 0;
    std::string receiveDirPath = ".";
    receiveDirCmd->add_option("--port", receiveDirPort, "TCP port to listen on (defaults to 0 for ephemeral)");
    receiveDirCmd->add_option("--accept-dir", receiveDirPath, "Directory to sync with peer")->default_val(".");

    CLI11_PARSE(app, argc, argv);

    if (app.got_subcommand(listenCmd)) {
        return runListen(listenName, listenPort);
    } else if (app.got_subcommand(discoverCmd)) {
        return runDiscover(discoverTimeout);
    } else if (app.got_subcommand(sendCmd)) {
        return runSend(sendFile, sendTo, sendPort);
    } else if (app.got_subcommand(receiveCmd)) {
        return runReceive(receivePort, receiveDir);
    } else if (app.got_subcommand(syncCmd)) {
        return runSync(syncDir, syncTo, syncPort);
    } else if (app.got_subcommand(receiveDirCmd)) {
        return runReceiveDir(receiveDirPort, receiveDirPath);
    }

    return 0;
}
