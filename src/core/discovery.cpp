// NOTE: Full multicast advertise+browse behavior across machines is verified manually
// rather than in automated CI, as CI network namespaces often filter or restrict multicast.
// See docs/architecture.md and tests/test_discovery.cpp for details.

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

#include <peersync/discovery.h>
#include <peersync/exceptions.h>
#include <mdns.h>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <iostream>

namespace peersync {

#ifdef _WIN32
namespace {
struct WinsockDiscoveryInit {
    WinsockDiscoveryInit() {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    }
    ~WinsockDiscoveryInit() {
        WSACleanup();
    }
} g_winsockDiscoveryInit;
} // anonymous namespace
#endif

namespace discovery {

std::string getLocalHostname() {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        std::string s(buf);
        size_t dot = s.find('.');
        if (dot != std::string::npos) {
            s = s.substr(0, dot);
        }
        if (!s.empty()) {
            return s;
        }
    }
    return "peersync-node";
}

std::string buildServiceInstanceName(const std::string& instanceName, const std::string& serviceType) {
    std::string inst = instanceName.empty() ? getLocalHostname() : instanceName;
    while (!inst.empty() && inst.back() == '.') inst.pop_back();

    std::string srv = serviceType;
    while (!srv.empty() && srv.front() == '.') srv.erase(srv.begin());
    while (!srv.empty() && srv.back() == '.') srv.pop_back();

    return inst + "." + srv + ".";
}

std::string buildServiceTargetName(const std::string& hostname) {
    std::string host = hostname.empty() ? getLocalHostname() : hostname;
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (host.length() > 6 && host.substr(host.length() - 6) == ".local") {
        host = host.substr(0, host.length() - 6);
    }
    return host + ".local.";
}

std::vector<std::string> buildTxtRecords(uint16_t port, const std::string& version) {
    return {
        "port=" + std::to_string(port),
        "version=" + version,
        "proto=peersync"
    };
}

bool matchServiceQuery(const std::string& queryName, const std::string& serviceType) {
    auto toLower = [](const std::string& s) {
        std::string res = s;
        for (char& c : res) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return res;
    };
    std::string q = toLower(queryName);
    std::string s = toLower(serviceType);

    while (!q.empty() && q.back() == '.') q.pop_back();
    while (!s.empty() && s.back() == '.') s.pop_back();

    if (q == s) return true;
    if (q.length() > s.length() && q.substr(q.length() - s.length() - 1) == ("." + s)) {
        return true;
    }
    if (q == "_services._dns-sd._udp.local") {
        return true;
    }
    return false;
}

std::vector<uint8_t> encodeTxtRecordData(const std::vector<std::string>& records) {
    std::vector<uint8_t> wire;
    for (const auto& rec : records) {
        if (rec.length() > 255) continue;
        wire.push_back(static_cast<uint8_t>(rec.length()));
        for (char c : rec) {
            wire.push_back(static_cast<uint8_t>(c));
        }
    }
    return wire;
}

std::vector<std::string> decodeTxtRecordData(const uint8_t* data, size_t len) {
    std::vector<std::string> records;
    if (!data || len == 0) return records;
    size_t i = 0;
    while (i < len) {
        uint8_t slen = data[i++];
        if (i + slen > len) break;
        std::string str(reinterpret_cast<const char*>(data + i), slen);
        records.push_back(str);
        i += slen;
    }
    return records;
}

} // namespace discovery

namespace {
std::string getLocalIpAddress() {
    int sock = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
    if (sock < 0) return "127.0.0.1";
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr("8.8.8.8");
    serv.sin_port = htons(53);
    if (connect(sock, reinterpret_cast<struct sockaddr*>(&serv), sizeof(serv)) < 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return "127.0.0.1";
    }
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    if (getsockname(sock, reinterpret_cast<struct sockaddr*>(&name), &namelen) < 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return "127.0.0.1";
    }
    char buffer[128];
    inet_ntop(AF_INET, &name.sin_addr, buffer, sizeof(buffer));
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    return buffer;
}

static int mdnsCallback(int sock, const struct sockaddr* from, size_t addrlen,
                        mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                        uint16_t rclass, uint32_t ttl, const void* data,
                        size_t size, size_t name_offset, size_t name_length,
                        size_t record_offset, size_t record_length, void* user_data) {
    return PeerAdvertiser::queryCallbackStatic(sock, from, addrlen, static_cast<int>(entry),
                                               query_id, rtype, rclass, ttl, data, size,
                                               name_offset, name_length, record_offset,
                                               record_length, user_data);
}
} // anonymous namespace

PeerAdvertiser::PeerAdvertiser(uint16_t tcpPort, const std::string& instanceName)
    : m_tcpPort(tcpPort),
      m_instanceName(instanceName.empty() ? discovery::getLocalHostname() : instanceName),
      m_serviceType("_peersync._tcp.local."),
      m_fullServiceName(discovery::buildServiceInstanceName(m_instanceName, "_peersync._tcp.local")),
      m_targetName(discovery::buildServiceTargetName(m_instanceName)),
      m_txtRecords(discovery::buildTxtRecords(tcpPort)) {
}

PeerAdvertiser::~PeerAdvertiser() {
    stop();
}

bool PeerAdvertiser::start() {
    if (m_running) return true;

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(MDNS_PORT);

    m_socket = mdns_socket_open_ipv4(&saddr);
    if (m_socket < 0) {
        return false;
    }

    m_running = true;
    m_thread = std::thread(&PeerAdvertiser::responderLoop, this);

    announce();
    return true;
}

void PeerAdvertiser::stop() {
    if (!m_running) return;

    if (m_socket >= 0) {
        goodbye();
    }

    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }

    if (m_socket >= 0) {
        mdns_socket_close(m_socket);
        m_socket = -1;
    }
}

void PeerAdvertiser::responderLoop() {
    std::vector<uint8_t> buffer(2048);
    while (m_running) {
        fd_set readfs;
        FD_ZERO(&readfs);
        FD_SET(m_socket, &readfs);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms check interval
        int res = select(static_cast<int>(m_socket + 1), &readfs, nullptr, nullptr, &tv);
        if (res > 0 && FD_ISSET(m_socket, &readfs)) {
            mdns_socket_listen(m_socket, buffer.data(), buffer.size(), mdnsCallback, this);
        }
    }
}

void PeerAdvertiser::announce() {
    if (m_socket < 0) return;
    std::vector<uint8_t> buffer(2048);

    mdns_record_t answer;
    memset(&answer, 0, sizeof(answer));
    answer.name = {m_serviceType.c_str(), m_serviceType.length()};
    answer.type = MDNS_RECORDTYPE_PTR;
    answer.data.ptr.name = {m_fullServiceName.c_str(), m_fullServiceName.length()};

    std::vector<mdns_record_t> additional;

    mdns_record_t srv;
    memset(&srv, 0, sizeof(srv));
    srv.name = {m_fullServiceName.c_str(), m_fullServiceName.length()};
    srv.type = MDNS_RECORDTYPE_SRV;
    srv.data.srv.priority = 0;
    srv.data.srv.weight = 0;
    srv.data.srv.port = m_tcpPort;
    srv.data.srv.name = {m_targetName.c_str(), m_targetName.length()};
    additional.push_back(srv);

    std::string ipStr = getLocalIpAddress();
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ipStr.c_str(), &addr.sin_addr);

    mdns_record_t a;
    memset(&a, 0, sizeof(a));
    a.name = {m_targetName.c_str(), m_targetName.length()};
    a.type = MDNS_RECORDTYPE_A;
    a.data.a.addr = addr;
    additional.push_back(a);

    std::vector<std::pair<std::string, std::string>> kvPairs;
    for (const auto& kv : m_txtRecords) {
        size_t eq = kv.find('=');
        std::string k = (eq != std::string::npos) ? kv.substr(0, eq) : kv;
        std::string v = (eq != std::string::npos) ? kv.substr(eq + 1) : "";
        kvPairs.push_back({k, v});
    }
    for (const auto& p : kvPairs) {
        mdns_record_t txt;
        memset(&txt, 0, sizeof(txt));
        txt.name = {m_fullServiceName.c_str(), m_fullServiceName.length()};
        txt.type = MDNS_RECORDTYPE_TXT;
        txt.data.txt.key = {p.first.c_str(), p.first.length()};
        txt.data.txt.value = {p.second.c_str(), p.second.length()};
        additional.push_back(txt);
    }

    mdns_announce_multicast(m_socket, buffer.data(), buffer.size(), answer, nullptr, 0, additional.data(), additional.size());
}

void PeerAdvertiser::goodbye() {
    if (m_socket < 0) return;
    std::vector<uint8_t> buffer(2048);

    mdns_record_t answer;
    memset(&answer, 0, sizeof(answer));
    answer.name = {m_serviceType.c_str(), m_serviceType.length()};
    answer.type = MDNS_RECORDTYPE_PTR;
    answer.data.ptr.name = {m_fullServiceName.c_str(), m_fullServiceName.length()};

    std::vector<mdns_record_t> additional;

    mdns_record_t srv;
    memset(&srv, 0, sizeof(srv));
    srv.name = {m_fullServiceName.c_str(), m_fullServiceName.length()};
    srv.type = MDNS_RECORDTYPE_SRV;
    srv.data.srv.priority = 0;
    srv.data.srv.weight = 0;
    srv.data.srv.port = m_tcpPort;
    srv.data.srv.name = {m_targetName.c_str(), m_targetName.length()};
    additional.push_back(srv);

    std::string ipStr = getLocalIpAddress();
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ipStr.c_str(), &addr.sin_addr);

    mdns_record_t a;
    memset(&a, 0, sizeof(a));
    a.name = {m_targetName.c_str(), m_targetName.length()};
    a.type = MDNS_RECORDTYPE_A;
    a.data.a.addr = addr;
    additional.push_back(a);

    std::vector<std::pair<std::string, std::string>> kvPairs;
    for (const auto& kv : m_txtRecords) {
        size_t eq = kv.find('=');
        std::string k = (eq != std::string::npos) ? kv.substr(0, eq) : kv;
        std::string v = (eq != std::string::npos) ? kv.substr(eq + 1) : "";
        kvPairs.push_back({k, v});
    }
    for (const auto& p : kvPairs) {
        mdns_record_t txt;
        memset(&txt, 0, sizeof(txt));
        txt.name = {m_fullServiceName.c_str(), m_fullServiceName.length()};
        txt.type = MDNS_RECORDTYPE_TXT;
        txt.data.txt.key = {p.first.c_str(), p.first.length()};
        txt.data.txt.value = {p.second.c_str(), p.second.length()};
        additional.push_back(txt);
    }

    mdns_goodbye_multicast(m_socket, buffer.data(), buffer.size(), answer, nullptr, 0, additional.data(), additional.size());
}

int PeerAdvertiser::queryCallbackStatic(int sock, const struct sockaddr* from, size_t addrlen,
                                        int entry, uint16_t query_id, uint16_t rtype,
                                        uint16_t rclass, uint32_t ttl, const void* data,
                                        size_t size, size_t name_offset, size_t name_length,
                                        size_t record_offset, size_t record_length, void* user_data) {
    auto* self = static_cast<PeerAdvertiser*>(user_data);
    return self->handleQuery(sock, from, addrlen, entry, query_id, rtype, rclass, ttl, data, size, name_offset, name_length);
}

int PeerAdvertiser::handleQuery(int sock, const struct sockaddr* from, size_t addrlen,
                                int entry, uint16_t query_id, uint16_t rtype,
                                uint16_t rclass, uint32_t ttl, const void* data,
                                size_t size, size_t name_offset, size_t name_length) {
    if (entry != MDNS_ENTRYTYPE_QUESTION) return 0;

    char namebuf[512];
    size_t offset = name_offset;
    mdns_string_t nameStr = mdns_string_extract(data, size, &offset, namebuf, sizeof(namebuf));
    if (nameStr.length == 0 || nameStr.str == nullptr) return 0;
    std::string qname(nameStr.str, nameStr.length);

    if (!discovery::matchServiceQuery(qname, m_serviceType) && !discovery::matchServiceQuery(qname, m_fullServiceName)) {
        return 0;
    }

    mdns_record_t answer;
    memset(&answer, 0, sizeof(answer));
    answer.name = {m_serviceType.c_str(), m_serviceType.length()};
    answer.type = MDNS_RECORDTYPE_PTR;
    answer.data.ptr.name = {m_fullServiceName.c_str(), m_fullServiceName.length()};

    std::vector<mdns_record_t> additional;

    mdns_record_t srv;
    memset(&srv, 0, sizeof(srv));
    srv.name = {m_fullServiceName.c_str(), m_fullServiceName.length()};
    srv.type = MDNS_RECORDTYPE_SRV;
    srv.data.srv.priority = 0;
    srv.data.srv.weight = 0;
    srv.data.srv.port = m_tcpPort;
    srv.data.srv.name = {m_targetName.c_str(), m_targetName.length()};
    additional.push_back(srv);

    std::string ipStr = getLocalIpAddress();
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ipStr.c_str(), &addr.sin_addr);

    mdns_record_t a;
    memset(&a, 0, sizeof(a));
    a.name = {m_targetName.c_str(), m_targetName.length()};
    a.type = MDNS_RECORDTYPE_A;
    a.data.a.addr = addr;
    additional.push_back(a);

    std::vector<std::pair<std::string, std::string>> kvPairs;
    for (const auto& kv : m_txtRecords) {
        size_t eq = kv.find('=');
        std::string k = (eq != std::string::npos) ? kv.substr(0, eq) : kv;
        std::string v = (eq != std::string::npos) ? kv.substr(eq + 1) : "";
        kvPairs.push_back({k, v});
    }
    for (const auto& p : kvPairs) {
        mdns_record_t txt;
        memset(&txt, 0, sizeof(txt));
        txt.name = {m_fullServiceName.c_str(), m_fullServiceName.length()};
        txt.type = MDNS_RECORDTYPE_TXT;
        txt.data.txt.key = {p.first.c_str(), p.first.length()};
        txt.data.txt.value = {p.second.c_str(), p.second.length()};
        additional.push_back(txt);
    }

    bool unicast = (rclass & MDNS_UNICAST_RESPONSE) != 0;
    std::vector<uint8_t> respBuf(2048);
    if (unicast) {
        mdns_query_answer_unicast(sock, from, addrlen, respBuf.data(), respBuf.size(), query_id,
                                  static_cast<mdns_record_type_t>(rtype),
                                  qname.c_str(), qname.length(),
                                  answer, nullptr, 0, additional.data(), additional.size());
    } else {
        mdns_query_answer_multicast(sock, respBuf.data(), respBuf.size(),
                                    answer, nullptr, 0, additional.data(), additional.size());
    }
    return 0;
}

} // namespace peersync
