#ifndef PEERSYNC_CONFLICT_RESOLUTION_H
#define PEERSYNC_CONFLICT_RESOLUTION_H

#include <string>
#include <cstdint>

namespace peersync {

enum class SimultaneousSyncRole {
    Sender,
    Receiver
};

struct PeerIdentifier {
    std::string deviceName;
    std::string ipAddress;
    uint16_t port{0};

    bool operator==(const PeerIdentifier& other) const {
        return deviceName == other.deviceName && ipAddress == other.ipAddress && port == other.port;
    }
};

// Determines which peer should act as Sender vs Receiver when both initiate a sync of the same path simultaneously.
// Lexicographically compares deviceName, tie-breaking on ipAddress, then port.
// Returns SimultaneousSyncRole::Sender if localPeer < remotePeer, otherwise SimultaneousSyncRole::Receiver.
// If identical (same device, IP, and port), defaults to Sender.
SimultaneousSyncRole resolveSimultaneousSyncRole(const PeerIdentifier& localPeer, const PeerIdentifier& remotePeer);

} // namespace peersync

#endif // PEERSYNC_CONFLICT_RESOLUTION_H
