#include <peersync/message_framing.h>
#include <peersync/exceptions.h>
#include <string>
#include <cstring>
#include <chrono>
#include <atomic>
#include <vector>

extern std::atomic<int> g_metricsCount;
extern std::vector<int> g_t_recv_syscall_count;
extern std::vector<double> g_t_recv_syscall_avg;

namespace peersync {

void sendFramedMessage(TcpSocket& sock, const uint8_t* data, size_t len) {
    if (len > MAX_MESSAGE_SIZE) {
        throw PeerSyncNetworkException("sendFramedMessage: payload size " + std::to_string(len) + 
                                       " exceeds MAX_MESSAGE_SIZE (" + std::to_string(MAX_MESSAGE_SIZE) + ")");
    }
    if (len > 0 && data == nullptr) {
        throw PeerSyncNetworkException("sendFramedMessage: data pointer is null with non-zero length " + std::to_string(len));
    }

    uint32_t netLen = static_cast<uint32_t>(len);
    std::vector<uint8_t> buffer(len + 4);
    buffer[0] = static_cast<uint8_t>((netLen >> 24) & 0xFF);
    buffer[1] = static_cast<uint8_t>((netLen >> 16) & 0xFF);
    buffer[2] = static_cast<uint8_t>((netLen >> 8) & 0xFF);
    buffer[3] = static_cast<uint8_t>(netLen & 0xFF);

    if (len > 0) {
        std::memcpy(buffer.data() + 4, data, len);
    }
    
    sock.send(buffer.data(), buffer.size());
}

void sendFramedMessage(TcpSocket& sock, const std::vector<uint8_t>& payload) {
    sendFramedMessage(sock, payload.data(), payload.size());
}

static void recvExactly(TcpSocket& sock, uint8_t* buffer, size_t exactLen, const std::string& stage) {
    if (exactLen == 0) return;
    if (buffer == nullptr) {
        throw PeerSyncNetworkException("recvExactly: null buffer with non-zero exactLen");
    }
    size_t totalReceived = 0;
    int localRecvCount = 0;
    double localRecvTime = 0.0;
    while (totalReceived < exactLen) {
        auto t0 = std::chrono::high_resolution_clock::now();
        size_t recvd = sock.recv(buffer + totalReceived, exactLen - totalReceived);
        auto t1 = std::chrono::high_resolution_clock::now();
        localRecvCount++;
        localRecvTime += std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (recvd == 0) {
            if (totalReceived > 0) {
                throw PeerSyncNetworkException("Connection closed mid-message during " + stage + 
                                               " (received " + std::to_string(totalReceived) + "/" + 
                                               std::to_string(exactLen) + " bytes)");
            } else {
                throw PeerSyncNetworkException("Connection closed before reading " + stage);
            }
        }
        totalReceived += recvd;
    }

    if (stage == "payload" && exactLen > 100000) {
        int batch = g_metricsCount.load();
        if (batch < 20) {
            g_t_recv_syscall_count[batch] = localRecvCount;
            g_t_recv_syscall_avg[batch] = localRecvTime / localRecvCount;
        }
    }
}

std::vector<uint8_t> recvFramedMessage(TcpSocket& sock) {
    uint8_t prefix[4];
    recvExactly(sock, prefix, 4, "length prefix");

    uint32_t len = (static_cast<uint32_t>(prefix[0]) << 24) |
                   (static_cast<uint32_t>(prefix[1]) << 16) |
                   (static_cast<uint32_t>(prefix[2]) << 8)  |
                   (static_cast<uint32_t>(prefix[3]));

    if (len > MAX_MESSAGE_SIZE) {
        throw PeerSyncNetworkException("recvFramedMessage: length prefix " + std::to_string(len) + 
                                       " exceeds MAX_MESSAGE_SIZE (" + std::to_string(MAX_MESSAGE_SIZE) + ")");
    }

    std::vector<uint8_t> payload(len);
    if (len > 0) {
        recvExactly(sock, payload.data(), len, "payload");
    }

    return payload;
}

} // namespace peersync
