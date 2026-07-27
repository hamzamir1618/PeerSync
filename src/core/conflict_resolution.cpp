#include <peersync/conflict_resolution.h>
#include <tuple>

namespace peersync {

SimultaneousSyncRole resolveSimultaneousSyncRole(const PeerIdentifier& localPeer, const PeerIdentifier& remotePeer) {
    auto localKey = std::tie(localPeer.deviceName, localPeer.ipAddress, localPeer.port);
    auto remoteKey = std::tie(remotePeer.deviceName, remotePeer.ipAddress, remotePeer.port);
    if (localKey <= remoteKey) {
        return SimultaneousSyncRole::Sender;
    }
    return SimultaneousSyncRole::Receiver;
}

} // namespace peersync
