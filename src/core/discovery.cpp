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
#include <chrono>
#include <map>
#include <mutex>

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

namespace discovery {

struct RecordHarvest {
    std::vector<std::string> ptrInstances;
    std::map<std::string, std::pair<uint16_t, std::string>> srvRecords;
    std::map<std::string, std::string> srvOriginalNames;
    std::map<std::string, std::string> aRecords;
    std::string senderIp;
};

static int harvestCallback(int sock, const struct sockaddr* from, size_t addrlen,
                           mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                           uint16_t rclass, uint32_t ttl, const void* data,
                           size_t size, size_t name_offset, size_t name_length,
                           size_t record_offset, size_t record_length, void* user_data) {
    if (entry != MDNS_ENTRYTYPE_ANSWER && entry != MDNS_ENTRYTYPE_AUTHORITY && entry != MDNS_ENTRYTYPE_ADDITIONAL) {
        return 0;
    }
    auto* harvest = static_cast<RecordHarvest*>(user_data);
    char namebuf[512];
    size_t noff = name_offset;
    mdns_string_t nameStr = mdns_string_extract(data, size, &noff, namebuf, sizeof(namebuf));
    if (nameStr.length == 0 || !nameStr.str) return 0;
    std::string recName(nameStr.str, nameStr.length);

    auto toLower = [](const std::string& s) {
        std::string res = s;
        for (char& c : res) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return res;
    };
    std::string lowerRecName = toLower(recName);

    if (rtype == MDNS_RECORDTYPE_PTR) {
        char valbuf[512];
        mdns_string_t valStr = mdns_record_parse_ptr(data, size, record_offset, record_length, valbuf, sizeof(valbuf));
        if (valStr.length > 0 && valStr.str) {
            std::string ptrVal(valStr.str, valStr.length);
            harvest->ptrInstances.push_back(toLower(ptrVal));
        }
    } else if (rtype == MDNS_RECORDTYPE_SRV) {
        char valbuf[512];
        mdns_record_srv_t srv = mdns_record_parse_srv(data, size, record_offset, record_length, valbuf, sizeof(valbuf));
        if (srv.name.length > 0 && srv.name.str) {
            std::string target(srv.name.str, srv.name.length);
            harvest->srvRecords[lowerRecName] = {srv.port, toLower(target)};
            harvest->srvOriginalNames[lowerRecName] = recName;
        }
    } else if (rtype == MDNS_RECORDTYPE_A) {
        struct sockaddr_in addr;
        if (mdns_record_parse_a(data, size, record_offset, record_length, &addr)) {
            char ipbuf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf))) {
                harvest->aRecords[lowerRecName] = ipbuf;
            }
        }
    }
    return 0;
}

std::vector<DiscoveredPeer> parseMdnsResponsePacket(const uint8_t* buffer, size_t size) {
    std::vector<DiscoveredPeer> result;
    if (!buffer || size < 12) return result;

    const uint16_t* data = reinterpret_cast<const uint16_t*>(buffer);
    uint16_t query_id = mdns_ntohs(data++);
    uint16_t flags = mdns_ntohs(data++);
    uint16_t questions = mdns_ntohs(data++);
    uint16_t answer_rrs = mdns_ntohs(data++);
    uint16_t authority_rrs = mdns_ntohs(data++);
    uint16_t additional_rrs = mdns_ntohs(data++);

    if ((answer_rrs + authority_rrs + additional_rrs) == 0 || (answer_rrs + authority_rrs + additional_rrs) > 500) {
        return result;
    }

    size_t offset = 12;
    for (int i = 0; i < questions; ++i) {
        mdns_string_skip(buffer, size, &offset);
        if (offset + 4 > size) return result;
        offset += 4;
    }

    RecordHarvest harvest;
    mdns_records_parse(-1, nullptr, 0, buffer, size, &offset, MDNS_ENTRYTYPE_ANSWER, query_id, answer_rrs, harvestCallback, &harvest);
    mdns_records_parse(-1, nullptr, 0, buffer, size, &offset, MDNS_ENTRYTYPE_AUTHORITY, query_id, authority_rrs, harvestCallback, &harvest);
    mdns_records_parse(-1, nullptr, 0, buffer, size, &offset, MDNS_ENTRYTYPE_ADDITIONAL, query_id, additional_rrs, harvestCallback, &harvest);

    auto toLower = [](const std::string& s) {
        std::string res = s;
        for (char& c : res) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return res;
    };

    std::vector<std::string> candidates = harvest.ptrInstances;
    for (const auto& kv : harvest.srvRecords) {
        candidates.push_back(kv.first);
    }

    for (const auto& cand : candidates) {
        std::string lowerCand = toLower(cand);
        if (lowerCand.find("._peersync._tcp.local") == std::string::npos) continue;

        auto srvIt = harvest.srvRecords.find(lowerCand);
        if (srvIt == harvest.srvRecords.end()) continue;

        uint16_t port = srvIt->second.first;
        std::string targetHost = srvIt->second.second;

        std::string ip;
        auto aIt = harvest.aRecords.find(targetHost);
        if (aIt != harvest.aRecords.end()) {
            ip = aIt->second;
        } else if (!harvest.senderIp.empty()) {
            ip = harvest.senderIp;
        }

        if (port == 0 || ip.empty()) continue;

        std::string origName = cand;
        auto origIt = harvest.srvOriginalNames.find(lowerCand);
        if (origIt != harvest.srvOriginalNames.end()) {
            origName = origIt->second;
        }

        std::string cleanName = origName;
        auto pos = toLower(cleanName).find("._peersync._tcp.local");
        if (pos != std::string::npos) {
            cleanName = cleanName.substr(0, pos);
        }

        DiscoveredPeer peer;
        peer.instanceName = cleanName;
        peer.ipAddress = ip;
        peer.port = port;
        peer.lastSeen = 0;

        auto exists = std::find(result.begin(), result.end(), peer);
        if (exists == result.end()) {
            result.push_back(peer);
        }
    }
    return result;
}

std::vector<uint8_t> buildHandConstructedMdnsPacket(const std::string& instance, const std::string& ip, uint16_t port) {
    std::vector<uint8_t> pkt;
    auto writeU16 = [&](uint16_t val) {
        pkt.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        pkt.push_back(static_cast<uint8_t>(val & 0xFF));
    };
    auto writeU32 = [&](uint32_t val) {
        pkt.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
        pkt.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        pkt.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        pkt.push_back(static_cast<uint8_t>(val & 0xFF));
    };
    auto writeName = [&](const std::string& name) {
        size_t pos = 0;
        while (pos < name.length()) {
            size_t dot = name.find('.', pos);
            if (dot == std::string::npos) dot = name.length();
            size_t len = dot - pos;
            if (len > 0) {
                pkt.push_back(static_cast<uint8_t>(len));
                for (size_t i = pos; i < dot; ++i) {
                    pkt.push_back(static_cast<uint8_t>(name[i]));
                }
            }
            pos = dot + 1;
        }
        pkt.push_back(0);
    };

    writeU16(0x0000); // query_id
    writeU16(0x8400); // flags: response, authoritative
    writeU16(0x0000); // questions
    writeU16(0x0001); // answer RRs (PTR)
    writeU16(0x0000); // authority RRs
    writeU16(0x0002); // additional RRs (SRV, A)

    std::string serviceName = "_peersync._tcp.local.";
    std::string fullInstName = instance + "." + serviceName;
    std::string targetHost = instance + ".local.";

    // PTR record
    writeName(serviceName);
    writeU16(12); // PTR
    writeU16(1);  // IN
    writeU32(3600); // TTL
    size_t ptrLenPos = pkt.size();
    writeU16(0); // placeholder
    size_t ptrRdataStart = pkt.size();
    writeName(fullInstName);
    uint16_t ptrLen = static_cast<uint16_t>(pkt.size() - ptrRdataStart);
    pkt[ptrLenPos] = static_cast<uint8_t>((ptrLen >> 8) & 0xFF);
    pkt[ptrLenPos + 1] = static_cast<uint8_t>(ptrLen & 0xFF);

    // SRV record
    writeName(fullInstName);
    writeU16(33); // SRV
    writeU16(1);  // IN
    writeU32(3600); // TTL
    size_t srvLenPos = pkt.size();
    writeU16(0); // placeholder
    size_t srvRdataStart = pkt.size();
    writeU16(0); // priority
    writeU16(0); // weight
    writeU16(port); // port
    writeName(targetHost);
    uint16_t srvLen = static_cast<uint16_t>(pkt.size() - srvRdataStart);
    pkt[srvLenPos] = static_cast<uint8_t>((srvLen >> 8) & 0xFF);
    pkt[srvLenPos + 1] = static_cast<uint8_t>(srvLen & 0xFF);

    // A record
    writeName(targetHost);
    writeU16(1); // A
    writeU16(1); // IN
    writeU32(3600); // TTL
    writeU16(4); // RDLENGTH = 4
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    const uint8_t* ipBytes = reinterpret_cast<const uint8_t*>(&addr.sin_addr.s_addr);
    for (int i = 0; i < 4; ++i) {
        pkt.push_back(ipBytes[i]);
    }

    return pkt;
}

} // namespace discovery

PeerBrowser::PeerBrowser() {
    // Winsock initialized globally via g_winsockDiscoveryInit
}

PeerBrowser::~PeerBrowser() {
    stop();
}

bool PeerBrowser::start(PeerCallback callback) {
    if (m_running) return false;

    m_callback = callback;

    m_socket = mdns_socket_open_ipv4(nullptr);
    if (m_socket < 0) {
        return false;
    }

    m_running = true;
    m_thread = std::thread(&PeerBrowser::browserLoop, this);
    return true;
}

void PeerBrowser::stop() {
    if (!m_running) return;

    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }

    if (m_socket >= 0) {
        mdns_socket_close(m_socket);
        m_socket = -1;
    }
}

std::vector<DiscoveredPeer> PeerBrowser::getCurrentPeers() const {
    std::lock_guard<std::mutex> lock(m_peersMutex);
    return m_peers;
}

void PeerBrowser::clearPeers() {
    std::lock_guard<std::mutex> lock(m_peersMutex);
    m_peers.clear();
}

void PeerBrowser::sendDiscoveryQuery() {
    if (m_socket < 0) return;
    std::vector<uint8_t> buffer(2048);
    mdns_query_send(m_socket, MDNS_RECORDTYPE_PTR, "_peersync._tcp.local.", 21, buffer.data(), buffer.size(), 0);
    mdns_discovery_send(m_socket);
}

void PeerBrowser::browserLoop() {
    std::vector<uint8_t> buffer(2048);
    sendDiscoveryQuery();

    auto lastQueryTime = std::chrono::steady_clock::now();

    while (m_running) {
        fd_set readfs;
        FD_ZERO(&readfs);
        FD_SET(m_socket, &readfs);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms
        int res = select(static_cast<int>(m_socket + 1), &readfs, nullptr, nullptr, &tv);
        if (res > 0 && FD_ISSET(m_socket, &readfs)) {
            struct sockaddr_in6 addr;
            struct sockaddr* saddr = reinterpret_cast<struct sockaddr*>(&addr);
            socklen_t addrlen = sizeof(addr);
            memset(&addr, 0, sizeof(addr));
            int ret = static_cast<int>(recvfrom(m_socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0, saddr, &addrlen));
            if (ret > 0) {
                std::vector<DiscoveredPeer> newPeers = discovery::parseMdnsResponsePacket(buffer.data(), static_cast<size_t>(ret));
                if (!newPeers.empty()) {
                    auto nowMillis = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                    std::lock_guard<std::mutex> lock(m_peersMutex);
                    for (auto p : newPeers) {
                        p.lastSeen = nowMillis;
                        auto it = std::find_if(m_peers.begin(), m_peers.end(), [&](const DiscoveredPeer& ep) {
                            return ep.instanceName == p.instanceName && ep.port == p.port;
                        });
                        if (it != m_peers.end()) {
                            it->ipAddress = p.ipAddress;
                            it->lastSeen = p.lastSeen;
                        } else {
                            m_peers.push_back(p);
                            if (m_callback) {
                                m_callback(p);
                            }
                        }
                    }
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastQueryTime).count() >= 5) {
            sendDiscoveryQuery();
            lastQueryTime = now;
        }
    }
}

int PeerBrowser::responseCallbackStatic(int sock, const struct sockaddr* from, size_t addrlen,
                                        int entry, uint16_t query_id, uint16_t rtype,
                                        uint16_t rclass, uint32_t ttl, const void* data,
                                        size_t size, size_t name_offset, size_t name_length,
                                        size_t record_offset, size_t record_length, void* user_data) {
    return 0;
}

} // namespace peersync
