#include <gtest/gtest.h>
#include <peersync/pairing.h>
#include <string>
#include <vector>
#include <set>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace {

std::string toHex(const std::vector<uint8_t>& bytes) {
    std::stringstream ss;
    for (uint8_t b : bytes) {
        ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(b);
    }
    return ss.str();
}

} // anonymous namespace

TEST(PairingTest, GeneratePinStatisticalSanity) {
    std::set<std::string> uniquePins;
    const int kIterations = 1000;

    for (int i = 0; i < kIterations; ++i) {
        std::string pin = peersync::generatePin();
        ASSERT_EQ(pin.length(), 6u) << "PIN must be exactly 6 characters long";
        for (char c : pin) {
            EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(c))) << "PIN character must be a numeric digit";
        }
        uniquePins.insert(pin);
    }

    // With 1000 generated 6-digit PINs, unique count should be very high (>500)
    EXPECT_GT(uniquePins.size(), 500u) << "PIN generator produced too many duplicate values";
}

TEST(PairingTest, DeriveSessionKeyDeterministic) {
    std::string pin = "123456";
    std::string salt = "random_salt_bytes";

    auto key1 = peersync::deriveSessionKey(pin, salt);
    auto key2 = peersync::deriveSessionKey(pin, salt);

    EXPECT_EQ(key1, key2) << "Deriving session key twice with identical PIN and salt must produce identical keys";
    EXPECT_FALSE(key1.empty());
}

TEST(PairingTest, DeriveSessionKeyDifferentPinsOrSaltsProduceDifferentKeys) {
    std::string pin = "123456";
    std::string salt = "random_salt_bytes";

    auto keyBase = peersync::deriveSessionKey(pin, salt);
    auto keyDiffPin = peersync::deriveSessionKey("654321", salt);
    auto keyDiffSalt = peersync::deriveSessionKey(pin, "other_salt_bytes");

    EXPECT_NE(keyBase, keyDiffPin) << "Different PINs must yield different session keys";
    EXPECT_NE(keyBase, keyDiffSalt) << "Different salts must yield different session keys";
    EXPECT_NE(keyDiffPin, keyDiffSalt);
}

TEST(PairingTest, DeriveSessionKeyExpectedFixedLength) {
    auto key32 = peersync::deriveSessionKey("000000", "salt", 100, 32);
    EXPECT_EQ(key32.size(), 32u);

    auto key64 = peersync::deriveSessionKey("000000", "salt", 100, 64);
    EXPECT_EQ(key64.size(), 64u);

    auto key16 = peersync::deriveSessionKey("000000", "salt", 100, 16);
    EXPECT_EQ(key16.size(), 16u);
}

TEST(PairingTest, Sha256KnownVectors) {
    // Empty string test vector
    EXPECT_EQ(toHex(peersync::pairing::sha256("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // "abc" test vector
    EXPECT_EQ(toHex(peersync::pairing::sha256("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(PairingTest, HmacSha256KnownVectors) {
    // RFC 4231 Test Case 1: 20 bytes of 0x0b with data "Hi There"
    std::vector<uint8_t> key(20, 0x0b);
    std::string data = "Hi There";
    EXPECT_EQ(toHex(peersync::pairing::hmacSha256(key, std::vector<uint8_t>(data.begin(), data.end()))),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(PairingTest, Pbkdf2HmacSha256KnownVectors) {
    // RFC 7914 / RFC 6070 derived test vectors for PBKDF2-HMAC-SHA256
    std::string pass = "password";
    std::string salt = "salt";

    auto res1 = peersync::pairing::pbkdf2HmacSha256(
        reinterpret_cast<const uint8_t*>(pass.data()), pass.size(),
        reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
        1, 32);
    EXPECT_EQ(toHex(res1), "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");

    auto res2 = peersync::pairing::pbkdf2HmacSha256(
        reinterpret_cast<const uint8_t*>(pass.data()), pass.size(),
        reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
        2, 32);
    EXPECT_EQ(toHex(res2), "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
}
