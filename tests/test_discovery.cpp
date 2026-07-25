// NOTE: Full multicast advertise+browse behavior across machines is verified manually
// rather than in automated CI, as CI network namespaces often filter or restrict multicast.
// This test file exercises the deterministic, standalone mDNS packet/formatting functions.

#include <gtest/gtest.h>
#include <peersync/discovery.h>
#include <vector>
#include <string>

using namespace peersync::discovery;

TEST(DiscoveryTest, GetLocalHostnameReturnsNonEmpty) {
    std::string host = getLocalHostname();
    EXPECT_FALSE(host.empty());
    EXPECT_NE(host.back(), '.');
}

TEST(DiscoveryTest, BuildServiceInstanceNameFormatsCorrectly) {
    std::string name1 = buildServiceInstanceName("MyLaptop");
    EXPECT_EQ(name1, "MyLaptop._peersync._tcp.local.");

    // Handle trailing dot in instance name
    std::string name2 = buildServiceInstanceName("MyLaptop.");
    EXPECT_EQ(name2, "MyLaptop._peersync._tcp.local.");

    // Custom service type
    std::string name3 = buildServiceInstanceName("NodeX", "_custom._tcp.local");
    EXPECT_EQ(name3, "NodeX._custom._tcp.local.");
}

TEST(DiscoveryTest, BuildServiceTargetNameFormatsCorrectly) {
    EXPECT_EQ(buildServiceTargetName("MyLaptop"), "MyLaptop.local.");
    EXPECT_EQ(buildServiceTargetName("MyLaptop."), "MyLaptop.local.");
    EXPECT_EQ(buildServiceTargetName("MyLaptop.local"), "MyLaptop.local.");
    EXPECT_EQ(buildServiceTargetName("MyLaptop.local."), "MyLaptop.local.");
}

TEST(DiscoveryTest, BuildTxtRecordsFormatsKeyValues) {
    auto recs = buildTxtRecords(12345, "2.0");
    ASSERT_EQ(recs.size(), 3u);
    EXPECT_EQ(recs[0], "port=12345");
    EXPECT_EQ(recs[1], "version=2.0");
    EXPECT_EQ(recs[2], "proto=peersync");
}

TEST(DiscoveryTest, MatchServiceQueryMatchesCorrectly) {
    EXPECT_TRUE(matchServiceQuery("_peersync._tcp.local", "_peersync._tcp.local"));
    EXPECT_TRUE(matchServiceQuery("_peersync._tcp.local.", "_peersync._tcp.local"));
    EXPECT_TRUE(matchServiceQuery("MyLaptop._peersync._tcp.local.", "_peersync._tcp.local"));
    EXPECT_TRUE(matchServiceQuery("mylaptop._peersync._tcp.local", "_peersync._tcp.local"));
    EXPECT_TRUE(matchServiceQuery("_services._dns-sd._udp.local", "_peersync._tcp.local"));

    EXPECT_FALSE(matchServiceQuery("_http._tcp.local", "_peersync._tcp.local"));
    EXPECT_FALSE(matchServiceQuery("MyLaptop._http._tcp.local.", "_peersync._tcp.local"));
}

TEST(DiscoveryTest, EncodeDecodeTxtRecordDataRoundTrip) {
    std::vector<std::string> orig = {"port=9999", "ver=1.5", "auth=none"};
    auto wire = encodeTxtRecordData(orig);

    // Verify length-prefixed wire format: \x09port=9999\x07ver=1.5\x09auth=none
    ASSERT_EQ(wire.size(), 1 + 9 + 1 + 7 + 1 + 9u);
    EXPECT_EQ(wire[0], 9u);
    EXPECT_EQ(wire[10], 7u);
    EXPECT_EQ(wire[18], 9u);

    auto decoded = decodeTxtRecordData(wire.data(), wire.size());
    EXPECT_EQ(decoded, orig);

    // Edge case: empty vector
    std::vector<std::string> emptyOrig;
    auto emptyWire = encodeTxtRecordData(emptyOrig);
    EXPECT_TRUE(emptyWire.empty());
    auto emptyDecoded = decodeTxtRecordData(nullptr, 0);
    EXPECT_TRUE(emptyDecoded.empty());
}

TEST(DiscoveryTest, AdvertiserLifeCycleCleanStop) {
    // We can construct a PeerAdvertiser with port 0 or an ephemeral port, and test start()/stop() cleanly!
    peersync::PeerAdvertiser advertiser(12345, "TestAdvertiserInstance");
    EXPECT_EQ(advertiser.getPort(), 12345u);
    EXPECT_EQ(advertiser.getInstanceName(), "TestAdvertiserInstance");
    EXPECT_EQ(advertiser.getFullServiceName(), "TestAdvertiserInstance._peersync._tcp.local.");
    EXPECT_FALSE(advertiser.isRunning());

    // Note: In some restricted CI environments or if another process binds 5353 exclusively without REUSEPORT,
    // start() might return false. But we can test that calling stop() or destructor when running or not running is 100% clean without hanging.
    bool started = advertiser.start();
    if (started) {
        EXPECT_TRUE(advertiser.isRunning());
    }
    advertiser.stop();
    EXPECT_FALSE(advertiser.isRunning());
}
