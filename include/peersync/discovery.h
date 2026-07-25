#ifndef PEERSYNC_DISCOVERY_H
#define PEERSYNC_DISCOVERY_H

#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <thread>

namespace peersync {
namespace discovery {

// Small testable helper functions for mDNS / DNS-SD formatting and parsing

// Returns the local hostname (stripping any trailing domain/local parts).
// Fallbacks to "peersync-node" if hostname resolution fails.
std::string getLocalHostname();

// Builds the full DNS-SD service instance name (e.g., "MyLaptop._peersync._tcp.local.").
// Ensures proper dot separation and trailing dot according to RFC 6763.
std::string buildServiceInstanceName(const std::string& instanceName,
                                     const std::string& serviceType = "_peersync._tcp.local");

// Builds the target hostname for SRV records (e.g., "MyLaptop.local.").
std::string buildServiceTargetName(const std::string& hostname);

// Builds standard TXT record strings (key=value format) advertising port and protocol version.
std::vector<std::string> buildTxtRecords(uint16_t port, const std::string& version = "1.0");

// Checks if a received mDNS query name matches our advertised service type or instance name.
bool matchServiceQuery(const std::string& queryName,
                       const std::string& serviceType = "_peersync._tcp.local");

// Encodes a list of "key=value" strings into standard DNS TXT record wire byte format
// (each string prefixed by a 1-byte length field).
std::vector<uint8_t> encodeTxtRecordData(const std::vector<std::string>& records);

// Decodes standard DNS TXT record wire byte format back into a vector of "key=value" strings.
std::vector<std::string> decodeTxtRecordData(const uint8_t* data, size_t len);

} // namespace discovery

// PeerAdvertiser: Advertises this instance as an mDNS/DNS-SD service of type
// "_peersync._tcp.local" on a background thread.
//
// NOTE: Full multicast advertise+browse behavior across machines is verified manually
// rather than in automated CI, as CI network namespaces often filter or restrict multicast.
// See docs/architecture.md and tests/test_discovery.cpp for details.
class PeerAdvertiser {
public:
    // Constructs the advertiser with the actual TCP port of the listening socket.
    // If instanceName is empty, defaults to discovery::getLocalHostname().
    explicit PeerAdvertiser(uint16_t tcpPort, const std::string& instanceName = "");
    ~PeerAdvertiser();

    // Starts the background responder thread and announces the service.
    // Returns true if the multicast socket was opened and listening started.
    bool start();

    // Sends a goodbye packet and cleanly shuts down the responder thread without hanging.
    void stop();

    // Accessors
    uint16_t getPort() const { return m_tcpPort; }
    const std::string& getInstanceName() const { return m_instanceName; }
    const std::string& getServiceType() const { return m_serviceType; }
    const std::string& getFullServiceName() const { return m_fullServiceName; }
    const std::string& getTargetName() const { return m_targetName; }
    bool isRunning() const { return m_running; }

    static int queryCallbackStatic(int sock, const struct sockaddr* from, size_t addrlen,
                                   int entry, uint16_t query_id, uint16_t rtype,
                                   uint16_t rclass, uint32_t ttl, const void* data,
                                   size_t size, size_t name_offset, size_t name_length,
                                   size_t record_offset, size_t record_length, void* user_data);

private:
    uint16_t m_tcpPort;
    std::string m_instanceName;
    std::string m_serviceType;
    std::string m_fullServiceName;
    std::string m_targetName;
    std::vector<std::string> m_txtRecords;

    std::atomic<bool> m_running{false};
    std::thread m_thread;
    int m_socket{-1};

    void responderLoop();
    void announce();
    void goodbye();

    int handleQuery(int sock, const struct sockaddr* from, size_t addrlen,
                    int entry, uint16_t query_id, uint16_t rtype,
                    uint16_t rclass, uint32_t ttl, const void* data,
                    size_t size, size_t name_offset, size_t name_length);
};

} // namespace peersync

#endif // PEERSYNC_DISCOVERY_H
