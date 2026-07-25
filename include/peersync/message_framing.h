#ifndef PEERSYNC_MESSAGE_FRAMING_H
#define PEERSYNC_MESSAGE_FRAMING_H

#include <peersync/socket.h>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace peersync {

constexpr size_t MAX_MESSAGE_SIZE = 64 * 1024 * 1024; // 64 MB

void sendFramedMessage(TcpSocket& sock, const uint8_t* data, size_t len);
void sendFramedMessage(TcpSocket& sock, const std::vector<uint8_t>& payload);
std::vector<uint8_t> recvFramedMessage(TcpSocket& sock);

} // namespace peersync

#endif // PEERSYNC_MESSAGE_FRAMING_H
