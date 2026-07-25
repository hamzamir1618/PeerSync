#include <peersync/message_framing.h>
#include <peersync/exceptions.h>
#include <string>

namespace peersync {

void sendFramedMessage(TcpSocket& sock, const uint8_t* data, size_t len) {
    if (len > MAX_MESSAGE_SIZE) {
        throw PeerSyncNetworkException("sendFramedMessage: payload size " + std::to_string(len) + 
                                       " exceeds MAX_MESSAGE_SIZE (" + std::to_string(MAX_MESSAGE_SIZE) + ")");
    }

    uint32_t netLen = static_cast<uint32_t>(len);
    uint8_t prefix[4];
    prefix[0] = static_cast<uint8_t>((netLen >> 24) & 0xFF);
    prefix[1] = static_cast<uint8_t>((netLen >> 16) & 0xFF);
    prefix[2] = static_cast<uint8_t>((netLen >> 8) & 0xFF);
    prefix[3] = static_cast<uint8_t>(netLen & 0xFF);

    sock.send(prefix, 4);
    if (len > 0 && data != nullptr) {
        sock.send(data, len);
    }
}

void sendFramedMessage(TcpSocket& sock, const std::vector<uint8_t>& payload) {
    sendFramedMessage(sock, payload.data(), payload.size());
}

static void recvExactly(TcpSocket& sock, uint8_t* buffer, size_t exactLen, const std::string& stage) {
    size_t totalReceived = 0;
    while (totalReceived < exactLen) {
        size_t recvd = sock.recv(buffer + totalReceived, exactLen - totalReceived);
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
