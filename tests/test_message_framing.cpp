#include <gtest/gtest.h>
#include <peersync/socket.h>
#include <peersync/message_framing.h>
#include <peersync/exceptions.h>
#include <thread>
#include <vector>
#include <string>

using namespace peersync;

TEST(MessageFramingTest, SmallPayloadRoundTrip) {
    TcpSocket server = TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    TcpSocket acceptedSocket;
    std::thread serverThread([&]() {
        acceptedSocket = server.accept();
    });

    TcpSocket client = TcpSocket::connect("127.0.0.1", port);
    if (serverThread.joinable()) {
        serverThread.join();
    }
    ASSERT_TRUE(client.isValid());
    ASSERT_TRUE(acceptedSocket.isValid());

    std::string msg = "Hello, Framed World!";
    std::vector<uint8_t> payload(msg.begin(), msg.end());

    // Client sends, server receives
    sendFramedMessage(client, payload);
    std::vector<uint8_t> received = recvFramedMessage(acceptedSocket);

    EXPECT_EQ(received, payload);
}

TEST(MessageFramingTest, LargePayloadRoundTrip) {
    TcpSocket server = TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    TcpSocket acceptedSocket;
    std::thread serverThread([&]() {
        acceptedSocket = server.accept();
    });

    TcpSocket client = TcpSocket::connect("127.0.0.1", port);
    if (serverThread.joinable()) {
        serverThread.join();
    }
    ASSERT_TRUE(client.isValid());
    ASSERT_TRUE(acceptedSocket.isValid());

    // Create a 2 MB payload (larger than standard socket send/recv buffers)
    size_t largeSize = 2 * 1024 * 1024;
    std::vector<uint8_t> payload(largeSize);
    for (size_t i = 0; i < largeSize; ++i) {
        payload[i] = static_cast<uint8_t>((i * 17) & 0xFF);
    }

    // Run send in a background thread to prevent potential socket buffer deadlock if OS buffers fill up
    std::thread sendThread([&]() {
        sendFramedMessage(client, payload);
    });

    std::vector<uint8_t> received = recvFramedMessage(acceptedSocket);

    if (sendThread.joinable()) {
        sendThread.join();
    }

    EXPECT_EQ(received.size(), payload.size());
    EXPECT_EQ(received, payload);
}

TEST(MessageFramingTest, RejectExceedsMaxMessageSize) {
    TcpSocket server = TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    TcpSocket acceptedSocket;
    std::thread serverThread([&]() {
        acceptedSocket = server.accept();
    });

    TcpSocket client = TcpSocket::connect("127.0.0.1", port);
    if (serverThread.joinable()) {
        serverThread.join();
    }
    ASSERT_TRUE(client.isValid());
    ASSERT_TRUE(acceptedSocket.isValid());

    // Craft a fake 4-byte prefix claiming length > MAX_MESSAGE_SIZE
    uint32_t fakeLen = static_cast<uint32_t>(MAX_MESSAGE_SIZE + 100);
    uint8_t prefix[4];
    prefix[0] = static_cast<uint8_t>((fakeLen >> 24) & 0xFF);
    prefix[1] = static_cast<uint8_t>((fakeLen >> 16) & 0xFF);
    prefix[2] = static_cast<uint8_t>((fakeLen >> 8) & 0xFF);
    prefix[3] = static_cast<uint8_t>(fakeLen & 0xFF);

    client.send(prefix, 4);

    // Receiver should throw PeerSyncNetworkException when attempting to read the framed message
    EXPECT_THROW({
        recvFramedMessage(acceptedSocket);
    }, PeerSyncNetworkException);
}

TEST(MessageFramingTest, MidMessageDisconnectThrowsException) {
    TcpSocket server = TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    TcpSocket acceptedSocket;
    std::thread serverThread([&]() {
        acceptedSocket = server.accept();
    });

    TcpSocket client = TcpSocket::connect("127.0.0.1", port);
    if (serverThread.joinable()) {
        serverThread.join();
    }
    ASSERT_TRUE(client.isValid());
    ASSERT_TRUE(acceptedSocket.isValid());

    // Send only a 4-byte length prefix claiming 1000 bytes of payload, then close sender socket immediately
    uint32_t netLen = 1000;
    uint8_t prefix[4];
    prefix[0] = static_cast<uint8_t>((netLen >> 24) & 0xFF);
    prefix[1] = static_cast<uint8_t>((netLen >> 16) & 0xFF);
    prefix[2] = static_cast<uint8_t>((netLen >> 8) & 0xFF);
    prefix[3] = static_cast<uint8_t>(netLen & 0xFF);

    client.send(prefix, 4);
    client.close();

    // Receiver should read prefix, attempt to read payload, get 0 bytes on recv(), and throw PeerSyncNetworkException
    EXPECT_THROW({
        recvFramedMessage(acceptedSocket);
    }, PeerSyncNetworkException);
}
