#include <gtest/gtest.h>
#include <peersync/socket.h>
#include <peersync/exceptions.h>
#include <thread>
#include <vector>
#include <string>
#include <chrono>

using namespace peersync;

TEST(TcpSocketTest, ListenAndGetBoundPort) {
    TcpSocket server = TcpSocket::listen(0, "127.0.0.1");
    EXPECT_TRUE(server.isValid());
    uint16_t port = server.getBoundPort();
    EXPECT_GT(port, 0);
}

TEST(TcpSocketTest, ConnectAndAccept) {
    TcpSocket server = TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();
    ASSERT_GT(port, 0);

    TcpSocket acceptedSocket;
    std::thread serverThread([&]() {
        acceptedSocket = server.accept();
    });

    TcpSocket client = TcpSocket::connect("127.0.0.1", port);
    EXPECT_TRUE(client.isValid());

    if (serverThread.joinable()) {
        serverThread.join();
    }
    EXPECT_TRUE(acceptedSocket.isValid());
}

TEST(TcpSocketTest, BidirectionalSendAndReceive) {
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

    // Client sends to server
    std::string clientMsg = "Hello from client!";
    size_t sent1 = client.send(reinterpret_cast<const uint8_t*>(clientMsg.data()), clientMsg.size());
    EXPECT_EQ(sent1, clientMsg.size());

    std::vector<uint8_t> buf1(128, 0);
    size_t recvd1 = acceptedSocket.recv(buf1.data(), buf1.size());
    EXPECT_EQ(recvd1, clientMsg.size());
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf1.data()), recvd1), clientMsg);

    // Server sends to client
    std::string serverMsg = "Hello back from server!";
    size_t sent2 = acceptedSocket.send(reinterpret_cast<const uint8_t*>(serverMsg.data()), serverMsg.size());
    EXPECT_EQ(sent2, serverMsg.size());

    std::vector<uint8_t> buf2(128, 0);
    size_t recvd2 = client.recv(buf2.data(), buf2.size());
    EXPECT_EQ(recvd2, serverMsg.size());
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf2.data()), recvd2), serverMsg);
}

TEST(TcpSocketTest, CleanDisconnectReturnsZero) {
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

    // Close client side
    client.close();

    // Server recv should return 0 (clean disconnect) without throwing
    std::vector<uint8_t> buf(128, 0);
    size_t recvd = 0;
    EXPECT_NO_THROW({
        recvd = acceptedSocket.recv(buf.data(), buf.size());
    });
    EXPECT_EQ(recvd, 0);
}

TEST(TcpSocketTest, ConnectToUnusedPortThrowsException) {
    // Bind a socket to ephemeral port to discover an unused port, then close it immediately
    uint16_t unusedPort = 0;
    {
        TcpSocket temp = TcpSocket::listen(0, "127.0.0.1");
        unusedPort = temp.getBoundPort();
        temp.close();
    }
    ASSERT_GT(unusedPort, 0);

    // Connecting to this closed port should throw PeerSyncNetworkException within our timeout
    auto start = std::chrono::steady_clock::now();
    EXPECT_THROW({
        TcpSocket::connect("127.0.0.1", unusedPort, 1000); // 1s timeout for fast testing
    }, PeerSyncNetworkException);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    
    // Ensure it didn't hang indefinitely (should fail fast or within timeout ~1000ms)
    EXPECT_LT(elapsed, 3000);
}
