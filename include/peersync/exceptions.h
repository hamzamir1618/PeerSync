#ifndef PEERSYNC_EXCEPTIONS_H
#define PEERSYNC_EXCEPTIONS_H

#include <stdexcept>
#include <string>

namespace peersync {

class PeerSyncNetworkException : public std::runtime_error {
public:
    explicit PeerSyncNetworkException(const std::string& message)
        : std::runtime_error(message) {}

    PeerSyncNetworkException(const std::string& action, int errCode, const std::string& errMessage)
        : std::runtime_error(action + " failed [error " + std::to_string(errCode) + "]: " + errMessage) {}
};

class PeerSyncProtocolException : public std::runtime_error {
public:
    explicit PeerSyncProtocolException(const std::string& message)
        : std::runtime_error(message) {}
};

class PeerSyncDeltaException : public std::runtime_error {
public:
    explicit PeerSyncDeltaException(const std::string& message)
        : std::runtime_error(message) {}
};

} // namespace peersync

#endif // PEERSYNC_EXCEPTIONS_H
