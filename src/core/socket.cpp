#include <peersync/socket.h>
#include <peersync/exceptions.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #pragma comment(lib, "ws2_32.lib")
  using os_socket_t = SOCKET;
  static constexpr os_socket_t OS_INVALID_SOCKET = INVALID_SOCKET;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  using os_socket_t = int;
  static constexpr os_socket_t OS_INVALID_SOCKET = -1;
  #define SOCKET_ERROR (-1)
#endif

#include <memory>
#include <cstring>

namespace peersync {

#ifdef _WIN32
static int getLastError() noexcept {
    return WSAGetLastError();
}

static void throwNetworkError(const std::string& action, int errCode = getLastError()) {
    char* msgBuf = nullptr;
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&msgBuf, 0, nullptr);
    std::string errMsg = "Unknown Windows Socket error";
    if (size > 0 && msgBuf) {
        errMsg = msgBuf;
        LocalFree(msgBuf);
        while (!errMsg.empty() && (errMsg.back() == '\r' || errMsg.back() == '\n')) {
            errMsg.pop_back();
        }
    }
    throw PeerSyncNetworkException(action, errCode, errMsg);
}

namespace {
    struct WinsockInit {
        int startupResult = 0;
        WinsockInit() {
            WSADATA wsaData;
            startupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        }
        ~WinsockInit() {
            if (startupResult == 0) {
                WSACleanup();
            }
        }
    };

    void ensureWinsockInitialized() {
        static WinsockInit init;
        if (init.startupResult != 0) {
            throwNetworkError("WSAStartup", init.startupResult);
        }
    }
}
#else
static int getLastError() noexcept {
    return errno;
}

static void throwNetworkError(const std::string& action, int errCode = getLastError()) {
    std::string errMsg = strerror(errCode);
    throw PeerSyncNetworkException(action, errCode, errMsg);
}

namespace {
    void ensureWinsockInitialized() {}
}
#endif

static void closeSocketHandle(os_socket_t sock) noexcept {
    if (sock != OS_INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
    }
}

TcpSocket::~TcpSocket() {
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : m_handle(other.m_handle) {
    other.m_handle = invalid_handle;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = other.m_handle;
        other.m_handle = invalid_handle;
    }
    return *this;
}

void TcpSocket::close() noexcept {
    if (m_handle != invalid_handle) {
        closeSocketHandle(static_cast<os_socket_t>(m_handle));
        m_handle = invalid_handle;
    }
}

TcpSocket TcpSocket::listen(uint16_t port, const std::string& bindAddr) {
    ensureWinsockInitialized();

    os_socket_t sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == OS_INVALID_SOCKET) {
        throwNetworkError("socket");
    }

    int opt = 1;
#ifdef _WIN32
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bindAddr.empty() || bindAddr == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, bindAddr.c_str(), &addr.sin_addr) <= 0) {
            closeSocketHandle(sock);
            throw PeerSyncNetworkException("Invalid bind IP address: " + bindAddr);
        }
    }

    if (::bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        int err = getLastError();
        closeSocketHandle(sock);
        throwNetworkError("bind", err);
    }

    if (::listen(sock, SOMAXCONN) != 0) {
        int err = getLastError();
        closeSocketHandle(sock);
        throwNetworkError("listen", err);
    }

    return TcpSocket(static_cast<handle_type>(sock));
}

struct AddrInfoDeleter {
    void operator()(addrinfo* p) const { if (p) ::freeaddrinfo(p); }
};

TcpSocket TcpSocket::connect(const std::string& host, uint16_t port, int timeoutMs) {
    ensureWinsockInitialized();

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    std::string portStr = std::to_string(port);
    int res = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);
    if (res != 0 || !result) {
#ifdef _WIN32
        throw PeerSyncNetworkException("getaddrinfo failed for " + host + ": " + gai_strerrorA(res));
#else
        throw PeerSyncNetworkException("getaddrinfo failed for " + host + ": " + gai_strerror(res));
#endif
    }
    std::unique_ptr<addrinfo, AddrInfoDeleter> addrList(result);

    os_socket_t sock = ::socket(addrList->ai_family, addrList->ai_socktype, addrList->ai_protocol);
    if (sock == OS_INVALID_SOCKET) {
        throwNetworkError("socket");
    }

#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

    int connRes = ::connect(sock, addrList->ai_addr, static_cast<int>(addrList->ai_addrlen));
    if (connRes != 0) {
        int err = getLastError();
#ifdef _WIN32
        if (err != WSAEWOULDBLOCK) {
#else
        if (err != EINPROGRESS) {
#endif
            closeSocketHandle(sock);
            throwNetworkError("connect to " + host + ":" + std::to_string(port), err);
        }

        fd_set writeFds;
        fd_set errFds;
        FD_ZERO(&writeFds);
        FD_ZERO(&errFds);
        FD_SET(sock, &writeFds);
        FD_SET(sock, &errFds);

        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        int selRes = ::select(static_cast<int>(sock + 1), nullptr, &writeFds, &errFds, &tv);
        if (selRes == 0) {
            closeSocketHandle(sock);
            throw PeerSyncNetworkException("connect timed out to " + host + ":" + std::to_string(port));
        } else if (selRes < 0) {
            int selErr = getLastError();
            closeSocketHandle(sock);
            throwNetworkError("select", selErr);
        }

        int soError = 0;
        socklen_t len = sizeof(soError);
#ifdef _WIN32
        if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soError, &len) != 0) {
#else
        if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &soError, &len) != 0) {
#endif
            int optErr = getLastError();
            closeSocketHandle(sock);
            throwNetworkError("getsockopt(SO_ERROR)", optErr);
        }

        if (soError != 0) {
            closeSocketHandle(sock);
            throwNetworkError("connect to " + host + ":" + std::to_string(port), soError);
        }
    }

#ifdef _WIN32
    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    fcntl(sock, F_SETFL, flags);
#endif

    TcpSocket resSocket(static_cast<handle_type>(sock));
    resSocket.setRecvTimeout(60000);
    resSocket.setSendTimeout(60000);
    return resSocket;
}

TcpSocket TcpSocket::accept() {
    if (m_handle == invalid_handle) {
        throw PeerSyncNetworkException("accept called on invalid socket");
    }
    sockaddr_in clientAddr{};
    socklen_t len = sizeof(clientAddr);
    os_socket_t clientSock = ::accept(static_cast<os_socket_t>(m_handle), (struct sockaddr*)&clientAddr, &len);
    if (clientSock == OS_INVALID_SOCKET) {
        throwNetworkError("accept");
    }
    TcpSocket clientSocket(static_cast<handle_type>(clientSock));
    clientSocket.setRecvTimeout(60000);
    clientSocket.setSendTimeout(60000);
    return clientSocket;
}

size_t TcpSocket::send(const uint8_t* data, size_t len) {
    if (m_handle == invalid_handle) {
        throw PeerSyncNetworkException("send called on invalid socket");
    }
    if (len == 0) return 0;
    if (data == nullptr) {
        throw PeerSyncNetworkException("send called with null buffer and non-zero length");
    }
    size_t totalSent = 0;
    while (totalSent < len) {
        size_t chunk = std::min<size_t>(len - totalSent, 1024 * 1024 * 64);
#ifdef _WIN32
        int res = ::send(static_cast<SOCKET>(m_handle), (const char*)(data + totalSent), static_cast<int>(chunk), 0);
#else
        ssize_t res = ::send(static_cast<int>(m_handle), data + totalSent, chunk, 0);
#endif
        if (res < 0) {
            int err = getLastError();
#ifndef _WIN32
            if (err == EINTR) continue;
#endif
            throwNetworkError("send", err);
        }
        if (res == 0) {
            break;
        }
        totalSent += static_cast<size_t>(res);
    }
    return totalSent;
}

size_t TcpSocket::recv(uint8_t* buffer, size_t maxLen) {
    if (m_handle == invalid_handle) {
        throw PeerSyncNetworkException("recv called on invalid socket");
    }
    if (maxLen == 0) return 0;
    if (buffer == nullptr) {
        throw PeerSyncNetworkException("recv called with null buffer and non-zero maxLen");
    }

    while (true) {
        size_t chunk = std::min<size_t>(maxLen, 1024 * 1024 * 64);
#ifdef _WIN32
        int res = ::recv(static_cast<SOCKET>(m_handle), (char*)buffer, static_cast<int>(chunk), 0);
#else
        ssize_t res = ::recv(static_cast<int>(m_handle), buffer, chunk, 0);
#endif
        if (res < 0) {
            int err = getLastError();
#ifndef _WIN32
            if (err == EINTR) continue;
#endif
#ifdef _WIN32
            if (err == WSAECONNRESET || err == WSAECONNABORTED) {
                return 0;
            }
#else
            if (err == ECONNRESET || err == ECONNABORTED) {
                return 0;
            }
#endif
            throwNetworkError("recv", err);
        }
        return static_cast<size_t>(res);
    }
}


uint16_t TcpSocket::getBoundPort() const {
    if (m_handle == invalid_handle) {
        throw PeerSyncNetworkException("getBoundPort called on invalid socket");
    }
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(static_cast<os_socket_t>(m_handle), (struct sockaddr*)&addr, &len) != 0) {
        throwNetworkError("getsockname");
    }
    return ntohs(addr.sin_port);
}

void TcpSocket::setRecvTimeout(int timeoutMs) {
    if (m_handle == invalid_handle) {
        throw PeerSyncNetworkException("setRecvTimeout called on invalid socket");
    }
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    if (::setsockopt(static_cast<SOCKET>(m_handle), SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) != 0) {
        throwNetworkError("setsockopt(SO_RCVTIMEO)");
    }
#else
    struct timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (::setsockopt(static_cast<int>(m_handle), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        throwNetworkError("setsockopt(SO_RCVTIMEO)");
    }
#endif
}

void TcpSocket::setSendTimeout(int timeoutMs) {
    if (m_handle == invalid_handle) {
        throw PeerSyncNetworkException("setSendTimeout called on invalid socket");
    }
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    if (::setsockopt(static_cast<SOCKET>(m_handle), SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout)) != 0) {
        throwNetworkError("setsockopt(SO_SNDTIMEO)");
    }
#else
    struct timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (::setsockopt(static_cast<int>(m_handle), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        throwNetworkError("setsockopt(SO_SNDTIMEO)");
    }
#endif
}

} // namespace peersync
