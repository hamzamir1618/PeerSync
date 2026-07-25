#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#endif

struct Subprocess {
#ifdef _WIN32
    PROCESS_INFORMATION pi{};
    HANDLE hChildStdOutRead{nullptr};
    bool valid{false};

    Subprocess() = default;
    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;
    Subprocess(Subprocess&& other) noexcept {
        *this = std::move(other);
    }
    Subprocess& operator=(Subprocess&& other) noexcept {
        if (this != &other) {
            terminate();
            pi = other.pi;
            hChildStdOutRead = other.hChildStdOutRead;
            valid = other.valid;
            other.pi = {};
            other.hChildStdOutRead = nullptr;
            other.valid = false;
        }
        return *this;
    }

    static Subprocess launch(const std::string& cmdline, bool captureOutput = false) {
        Subprocess sub;
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        if (captureOutput) {
            HANDLE hWrite = nullptr;
            CreatePipe(&sub.hChildStdOutRead, &hWrite, &sa, 0);
            SetHandleInformation(sub.hChildStdOutRead, HANDLE_FLAG_INHERIT, 0);
            si.hStdOutput = hWrite;
            si.hStdError = hWrite;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            si.dwFlags |= STARTF_USESTDHANDLES;
        }

        std::string cmd = cmdline;
        if (CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, captureOutput ? TRUE : FALSE,
                           0, nullptr, nullptr, &si, &sub.pi)) {
            sub.valid = true;
        }
        if (captureOutput && si.hStdOutput) {
            CloseHandle(si.hStdOutput);
        }
        return sub;
    }

    std::string readOutput() {
        if (!hChildStdOutRead) return "";
        std::string out;
        char buf[1024];
        DWORD bytesRead = 0;
        while (ReadFile(hChildStdOutRead, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
            out.append(buf, bytesRead);
        }
        return out;
    }

    void terminate() {
        if (valid && pi.hProcess) {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 3000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            valid = false;
        }
        if (hChildStdOutRead) {
            CloseHandle(hChildStdOutRead);
            hChildStdOutRead = nullptr;
        }
    }

    ~Subprocess() {
        terminate();
    }
#else
    pid_t pid{-1};
    int fdRead{-1};
    bool valid{false};

    Subprocess() = default;
    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;
    Subprocess(Subprocess&& other) noexcept {
        *this = std::move(other);
    }
    Subprocess& operator=(Subprocess&& other) noexcept {
        if (this != &other) {
            terminate();
            pid = other.pid;
            fdRead = other.fdRead;
            valid = other.valid;
            other.pid = -1;
            other.fdRead = -1;
            other.valid = false;
        }
        return *this;
    }

    static Subprocess launch(const std::string& cmdline, bool captureOutput = false) {
        Subprocess sub;
        int pipefd[2] = {-1, -1};
        if (captureOutput) {
            pipe(pipefd);
        }
        pid_t p = fork();
        if (p == 0) {
            if (captureOutput) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[1]);
            }
            execl("/bin/sh", "sh", "-c", cmdline.c_str(), (char*)nullptr);
            _exit(127);
        } else if (p > 0) {
            sub.pid = p;
            sub.valid = true;
            if (captureOutput) {
                close(pipefd[1]);
                sub.fdRead = pipefd[0];
            }
        }
        return sub;
    }

    std::string readOutput() {
        if (fdRead < 0) return "";
        std::string out;
        char buf[1024];
        ssize_t n = 0;
        while ((n = read(fdRead, buf, sizeof(buf))) > 0) {
            out.append(buf, n);
        }
        return out;
    }

    void terminate() {
        if (valid && pid > 0) {
            kill(pid, SIGTERM);
            int status = 0;
            for (int i = 0; i < 30; ++i) {
                if (waitpid(pid, &status, WNOHANG) == pid) break;
                usleep(100000);
            }
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            valid = false;
            pid = -1;
        }
        if (fdRead >= 0) {
            close(fdRead);
            fdRead = -1;
        }
    }

    ~Subprocess() {
        terminate();
    }
#endif
};

// NOTE ON MULTICAST CI SANDBOXING:
// This test launches a background `peersync listen` process and runs `peersync discover`
// to verify end-to-end service advertising and discovery across subprocesses.
// While this runs cleanly on local machines and loopback environments, many CI network sandboxes
// (such as default GitHub Actions runners or containerized network namespaces) filter, restrict,
// or completely block UDP multicast traffic on port 5353.
// If multicast is unavailable, discovery will time out and not find the background peer.
// In accordance with the project's established testing convention (see Project 1 "performance" label),
// this test is tagged with an explicit CTest label "requires_multicast" in CMakeLists.txt so that it can
// be excluded from default sandbox CI runs while still providing automated verification where multicast is permitted.
TEST(CliIntegrationTest, DiscoverFindsListeningAdvertiser) {
#ifndef PEERSYNC_CLI_PATH
    FAIL() << "PEERSYNC_CLI_PATH not defined";
#else
    std::string path = PEERSYNC_CLI_PATH;
#ifdef _WIN32
    for (char& c : path) {
        if (c == '/') c = '\\';
    }
#endif
    std::string cliExe = std::string("\"") + path + "\"";
    std::string peerName = "test_cli_peer_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 10000);
    std::string listenCmd = cliExe + " listen --name " + peerName + " --port 0";

    auto listenProc = Subprocess::launch(listenCmd, false);
    ASSERT_TRUE(listenProc.valid) << "Failed to launch listen process: " << listenCmd;

    // Allow time for socket bind and mDNS service announcement
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    std::string discoverCmd = cliExe + " discover --timeout 3";
    auto discoverProc = Subprocess::launch(discoverCmd, true);
    ASSERT_TRUE(discoverProc.valid) << "Failed to launch discover process: " << discoverCmd;

    std::string output = discoverProc.readOutput();

    discoverProc.terminate();
    listenProc.terminate();

    EXPECT_NE(output.find(peerName), std::string::npos)
        << "Expected to find peer name '" << peerName << "' in discover output:\n" << output;
#endif
}
