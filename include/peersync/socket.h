#ifndef PEERSYNC_SOCKET_H
#define PEERSYNC_SOCKET_H

#include <cstdint>
#include <cstddef>
#include <string>

namespace peersync {

class TcpSocket {
public:
    using handle_type = uintptr_t;
    static constexpr handle_type invalid_handle = static_cast<handle_type>(-1);

    TcpSocket() noexcept : m_handle(invalid_handle) {}
    explicit TcpSocket(handle_type handle) noexcept : m_handle(handle) {}
    ~TcpSocket();

    // Non-copyable
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    // Move-constructible and move-assignable
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    static TcpSocket listen(uint16_t port, const std::string& bindAddr = "0.0.0.0");
    static TcpSocket connect(const std::string& host, uint16_t port, int timeoutMs = 3000);

    TcpSocket accept();
    size_t send(const uint8_t* data, size_t len);
    size_t recv(uint8_t* buffer, size_t maxLen);

    uint16_t getBoundPort() const;
    void setRecvTimeout(int timeoutMs);
    void setSendTimeout(int timeoutMs);
    void setNoDelay(bool enable);
    bool getNoDelay() const;
    void getBufferSize(int& sndbuf, int& rcvbuf) const;
    void setBufferSize(int size);
    void close() noexcept;
    bool isValid() const noexcept { return m_handle != invalid_handle; }

private:
    handle_type m_handle;
};

} // namespace peersync

#endif // PEERSYNC_SOCKET_H
